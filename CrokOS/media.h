// ============================================================================
//  CrokOS - плеер: видео AVI (MJPEG + PCM) и музыка MP3 / WAV
//  Звук: I2S-усилитель MAX98357A на тех же выводах, что и в Retro-Go
//  (BCLK 41, LRC 42, DIN 40). Видео готовит приложение CrokPocket для Android.
// ============================================================================
#pragma once
#include "ui.h"
#include "settings.h"
#include <driver/i2s_std.h>
#include <freertos/stream_buffer.h>
#include <deque>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include <minimp3.h>

#define PIN_SPK_BCLK 41
#define PIN_SPK_LRC  42
#define PIN_SPK_DIN  40

inline bool isVideoExt(const String& e) { return e == "avi"; }
inline bool isAudioExt(const String& e) { return e == "mp3" || e == "wav"; }
inline bool isMediaExt(const String& e) { return isVideoExt(e) || isAudioExt(e); }

// ---------------------------------------------------------------------------
// Вывод звука
static i2s_chan_handle_t spk = nullptr;
static uint32_t spkRate = 0;

inline bool spkBegin(uint32_t rate) {
  if (spk && rate == spkRate) return true;
  if (spk) {   // смена частоты дискретизации
    i2s_channel_disable(spk);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(spk, &clk);
    i2s_channel_enable(spk);
    spkRate = rate;
    return true;
  }
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6;
  cc.dma_frame_num = 1000;   // ~136 мс при 44.1 кГц: хватает на перерисовку экрана
  cc.auto_clear = true;      // нет данных - тишина, а не повтор последнего куска
  if (i2s_new_channel(&cc, &spk, nullptr) != ESP_OK) { spk = nullptr; return false; }
  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PIN_SPK_BCLK,
      .ws = (gpio_num_t)PIN_SPK_LRC,
      .dout = (gpio_num_t)PIN_SPK_DIN,
      .din = I2S_GPIO_UNUSED,
      .invert_flags = {false, false, false},
    },
  };
  if (i2s_channel_init_std_mode(spk, &sc) != ESP_OK || i2s_channel_enable(spk) != ESP_OK) {
    i2s_del_channel(spk);
    spk = nullptr;
    return false;
  }
  spkRate = rate;
  return true;
}

inline void spkEnd() {
  if (!spk) return;
  i2s_channel_disable(spk);
  i2s_del_channel(spk);
  spk = nullptr;
  spkRate = 0;
}

// Громкость 0..20 -> множитель (квадратичная шкала, на слух ровнее)
inline int volGain() { int v = constrain(settings.volume, 0, 20); return v * v * 256 / 400; }   // 256 = 1.0

// PCM (8/16 бит, моно/стерео) -> стерео 16 бит с громкостью, запись в I2S (блокирует, пока не влезет)
inline void spkPlay(const uint8_t* data, size_t bytes, int channels, int bits) {
  static int16_t out[512 * 2];
  int g = volGain();
  size_t bps = bits / 8 * channels;   // байт на кадр
  size_t frames = bytes / bps;
  size_t i = 0;
  while (i < frames) {
    size_t n = min((size_t)512, frames - i);
    for (size_t k = 0; k < n; k++) {
      const uint8_t* p = data + (i + k) * bps;
      int l, r;
      if (bits == 16) { l = (int16_t)(p[0] | (p[1] << 8)); r = channels > 1 ? (int16_t)(p[2] | (p[3] << 8)) : l; }
      else { l = ((int)p[0] - 128) << 8; r = channels > 1 ? ((int)p[1] - 128) << 8 : l; }
      out[2 * k] = (int16_t)constrain(l * g / 256, -32768, 32767);
      out[2 * k + 1] = (int16_t)constrain(r * g / 256, -32768, 32767);
    }
    size_t w = 0;
    if (spk) i2s_channel_write(spk, out, n * 4, &w, portMAX_DELAY);
    i += n;
  }
}

// Сигнал 880 Гц, 0.15 с - проверка громкости в настройках
void volumeBeep() {
  if (settings.volume == 0) return;
  if (!spkBegin(22050)) return;
  static int16_t tone[22050 * 15 / 100];
  const int n = sizeof(tone) / 2;
  for (int i = 0; i < n; i++) {
    float env = i < 300 ? i / 300.f : (n - i < 600 ? (n - i) / 600.f : 1.f);   // без щелчков
    tone[i] = (int16_t)(sinf(2 * 3.14159265f * 880 * i / 22050) * 12000 * env);
  }
  spkPlay((const uint8_t*)tone, sizeof(tone), 1, 16);
  delay(300);   // дать DMA доиграть
  spkEnd();
}

inline String mmss(uint32_t s) { char b[16]; snprintf(b, sizeof b, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); return b; }

// ---------------------------------------------------------------------------
// Музыка: MP3 (minimp3) и WAV
struct MusicFile {
  File f;
  bool mp3 = false;
  uint32_t dataStart = 0, dataLen = 0;    // WAV: начало и длина данных; MP3: без ID3
  int rate = 44100, ch = 2, bits = 16;
  int kbps = 0;                            // MP3: битрейт первого кадра
  uint32_t played = 0;                     // проиграно кадров (сэмплов на канал)
  uint32_t baseSec = 0;                    // MP3: с какой секунды начали после перемотки
  // MP3
  mp3dec_t dec;
  uint8_t* buf = nullptr;
  size_t bufLen = 0;
  static const size_t BUF = 16384;

  ~MusicFile() { if (buf) free(buf); if (f) f.close(); }

  bool open(const String& path, String& err) {
    f = sdOpenRead(path);
    if (!f) { err = "Не удалось открыть файл"; return false; }
    mp3 = strLower(fileExt(path)) == "mp3";
    if (mp3) {
      uint8_t h[10];
      dataStart = 0;
      if (f.read(h, 10) == 10 && !memcmp(h, "ID3", 3)) {   // пропускаем ID3v2 (там бывает обложка на сотни КБ)
        dataStart = 10 + ((h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F));
        if (h[5] & 0x10) dataStart += 10;
      }
      dataLen = f.size() > dataStart ? f.size() - dataStart : 0;
      f.seek(dataStart);
      buf = (uint8_t*)ps_malloc(BUF);
      if (!buf) { err = "Не хватает памяти"; return false; }
      mp3dec_init(&dec);
      return true;
    }
    // WAV: ищем блоки fmt и data
    uint8_t h[12];
    if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { err = "Это не WAV-файл"; return false; }
    bool fmt = false;
    while (f.available() >= 8) {
      uint8_t c[8];
      f.read(c, 8);
      uint32_t sz = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
      if (!memcmp(c, "fmt ", 4)) {
        uint8_t fm[16];
        f.read(fm, 16);
        int tag = fm[0] | fm[1] << 8;
        ch = fm[2] | fm[3] << 8;
        rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24;
        bits = fm[14] | fm[15] << 8;
        if (tag != 1 && tag != 0xFFFE) { err = "WAV сжат (не PCM) - не поддерживается"; return false; }
        if ((bits != 8 && bits != 16) || ch < 1 || ch > 2) { err = "Поддерживается WAV 8/16 бит, моно или стерео"; return false; }
        f.seek(f.position() + sz - 16 + (sz & 1));
        fmt = true;
      } else if (!memcmp(c, "data", 4)) {
        dataStart = f.position();
        dataLen = min(sz, (uint32_t)(f.size() - dataStart));
        break;
      } else f.seek(f.position() + sz + (sz & 1));
    }
    if (!fmt || !dataStart) { err = "Повреждённый WAV"; return false; }
    f.seek(dataStart);
    return true;
  }

  uint32_t durationSec() const {
    if (!mp3) return dataLen / (uint32_t)max(1, ch * bits / 8) / (uint32_t)max(1, rate);
    return kbps ? (uint32_t)((uint64_t)dataLen * 8 / ((uint32_t)kbps * 1000)) : 0;
  }
  uint32_t posSec() const { return (mp3 ? baseSec : 0) + played / (uint32_t)max(1, rate); }

  void seekSec(int32_t s) {
    uint32_t dur = durationSec();
    if (s < 0) s = 0;
    if (dur && (uint32_t)s >= dur) s = dur - 1;
    if (mp3) {
      if (!kbps) return;
      uint32_t off = (uint32_t)((uint64_t)s * kbps * 1000 / 8);
      f.seek(dataStart + min(off, dataLen));
      bufLen = 0;
      mp3dec_init(&dec);
      baseSec = s;
      played = 0;
    } else {
      uint32_t fb = ch * bits / 8;
      uint32_t off = (uint32_t)s * rate * fb;
      f.seek(dataStart + min(off, dataLen));
      played = (uint32_t)s * rate;
    }
  }

  // Следующая порция звука в I2S. false - конец файла
  bool step() {
    if (!mp3) {
      static uint8_t b[4096];
      uint32_t end = dataStart + dataLen;
      uint32_t pos = f.position();
      if (pos >= end) return false;
      size_t n = f.read(b, min((uint32_t)sizeof(b), end - pos));
      if (n == 0) return false;
      spkPlay(b, n, ch, bits);
      played += n / (ch * bits / 8);
      return true;
    }
    if (bufLen < BUF / 2 && f.available()) {
      int n = f.read(buf + bufLen, BUF - bufLen);
      if (n > 0) bufLen += n;
    }
    if (bufLen == 0) return false;
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&dec, buf, bufLen, pcm, &info);
    if (info.frame_bytes == 0) { bufLen = 0; return f.available() > 0; }   // мусор до конца буфера
    memmove(buf, buf + info.frame_bytes, bufLen - info.frame_bytes);
    bufLen -= info.frame_bytes;
    if (samples > 0) {
      if (!kbps && info.bitrate_kbps) kbps = info.bitrate_kbps;
      if ((int)info.hz != rate || info.channels != ch) { rate = info.hz; ch = info.channels; spkBegin(rate); }
      spkPlay((const uint8_t*)pcm, samples * ch * 2, ch, 16);
      played += samples;
    }
    return true;
  }
};

inline void drawMusicScreen(const String& name, MusicFile& m, bool paused, int idx, int count) {
  canvas.fillScreen(C_BG);
  drawHeader("Музыка  " + String(idx + 1) + "/" + String(count));
  // «пластинка»
  int cx = SCR_W / 2, cy = 82;
  canvas.fillCircle(cx, cy, 44, C_PANEL);
  canvas.drawCircle(cx, cy, 34, C_DIM);
  canvas.drawCircle(cx, cy, 24, C_DIM);
  canvas.fillCircle(cx, cy, 12, C_ACCENT);
  if (paused) { canvas.fillRect(cx - 7, cy - 8, 5, 16, C_BG); canvas.fillRect(cx + 2, cy - 8, 5, 16, C_BG); }
  else canvas.fillTriangle(cx - 5, cy - 8, cx - 5, cy + 8, cx + 8, cy, C_BG);
  canvas.setFont(FONT);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(fitText(name, SCR_W - 20), cx, 140);
  // прогресс
  uint32_t pos = m.posSec(), dur = m.durationSec();
  int x0 = 20, w = SCR_W - 40, y = 160;
  canvas.fillRoundRect(x0, y, w, 6, 3, C_PANEL);
  if (dur) canvas.fillRoundRect(x0, y, max(6, (int)((uint64_t)w * min(pos, dur) / dur)), 6, 3, C_ACCENT);
  canvas.setFont(FONT_S);
  canvas.setTextColor(C_DIM);
  canvas.setTextDatum(top_left);
  canvas.drawString(mmss(pos), x0, y + 10);
  canvas.setTextDatum(top_right);
  canvas.drawString(dur ? mmss(dur) : String("--:--"), x0 + w, y + 10);
  // громкость
  canvas.setTextDatum(top_center);
  String vol = "Громкость ";
  canvas.drawString(vol + String(settings.volume * 5) + "%", cx, y + 10);
  drawFooter("A:пауза ←→:10с ↑↓:громк. SEL/OPT:трек B:выход");
  present();
}

// Плеер музыки: список файлов папки, начинаем с index
inline void musicPlay(const String& dir, const std::vector<String>& names, int index) {
  if (names.empty()) return;
  input.waitRelease();
  int volAtStart = settings.volume;
  bool quit = false;
  while (!quit && index >= 0 && index < (int)names.size()) {
    MusicFile* m = new MusicFile();
    String err;
    String path = pathJoin(dir, names[index]);
    mark("Плеер: открытие");
    if (!m->open(path, err)) {
      delete m;
      messageBox("Музыка", names[index] + ": " + err);
      index++;
      continue;
    }
    if (!spkBegin(m->rate)) { delete m; messageBox("Звук", "Не удалось запустить I2S"); break; }
    bool paused = false, ended = false;
    int next = index + 1;
    uint32_t lastDraw = 0;
    mark("Плеер: музыка");
    while (true) {
      if (!paused && !m->step()) { ended = true; break; }
      if (paused) delay(20);
      input.update();
      if (input.pressed(B_B) || input.pressed(B_MENU)) { quit = true; break; }
      if (input.pressed(B_A)) paused = !paused;
      if (input.pressed(B_RIGHT)) m->seekSec(m->posSec() + 10);
      if (input.pressed(B_LEFT)) m->seekSec((int32_t)m->posSec() - 10);
      if (input.pressed(B_UP)) settings.volume = min(20, settings.volume + 1);
      if (input.pressed(B_DOWN)) settings.volume = max(0, settings.volume - 1);
      if (input.pressed(B_SELECT)) { next = index + 1; break; }
      if (input.pressed(B_OPTION)) { next = m->posSec() > 3 ? index : max(0, index - 1); break; }
      if (millis() - lastDraw > (input.any() ? 0u : 400u)) {
        drawMusicScreen(names[index], *m, paused, index, names.size());
        lastDraw = millis();
      }
    }
    delete m;
    if (ended) next = index + 1;
    index = next;
  }
  spkEnd();
  if (settings.volume != volAtStart) settings.save();
  input.waitRelease();
}

// ---------------------------------------------------------------------------
// Видео AVI: MJPEG + PCM
struct AviInfo {
  uint32_t usPerFrame = 66666;
  int w = 0, h = 0;
  bool audio = false;
  int rate = 22050, ch = 1, bits = 16;
  uint32_t moviStart = 0, moviEnd = 0;   // данные внутри LIST movi
  uint32_t frames = 0;
  std::vector<uint32_t> vidOff;          // смещения кадров (из idx1), для перемотки
};

static inline uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

inline bool aviParse(File& f, AviInfo& a, String& err) {
  uint8_t h[12];
  f.seek(0);
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4)) { err = "Это не AVI"; return false; }
  uint32_t fsz = f.size();
  uint32_t pos = 12;
  int streamType = 0;   // 1 - видео, 2 - звук (для strf)
  while (pos + 8 <= fsz) {
    f.seek(pos);
    uint8_t c[12];
    if (f.read(c, 8) != 8) break;
    uint32_t sz = rd32(c + 4);
    if (!memcmp(c, "LIST", 4)) {
      f.read(c + 8, 4);
      if (!memcmp(c + 8, "movi", 4)) { a.moviStart = pos + 12; a.moviEnd = min(fsz, pos + 8 + sz); pos += 8 + sz + (sz & 1); continue; }
      pos += 12;   // заходим внутрь hdrl / strl
      continue;
    }
    if (!memcmp(c, "avih", 4)) {
      uint8_t d[40];
      f.read(d, 40);
      a.usPerFrame = rd32(d);
      a.frames = rd32(d + 16);
      a.w = rd32(d + 32);
      a.h = rd32(d + 36);
    } else if (!memcmp(c, "strh", 4)) {
      uint8_t d[4];
      f.read(d, 4);
      streamType = !memcmp(d, "vids", 4) ? 1 : !memcmp(d, "auds", 4) ? 2 : 0;
    } else if (!memcmp(c, "strf", 4)) {
      if (streamType == 1) {
        uint8_t d[20];
        f.read(d, 20);
        if (memcmp(d + 16, "MJPG", 4) && memcmp(d + 16, "mjpg", 4)) { err = "Видео не в формате MJPEG. Сконвертируйте его приложением CrokPocket."; return false; }
      } else if (streamType == 2) {
        uint8_t d[16];
        f.read(d, 16);
        int tag = d[0] | d[1] << 8;
        if (tag == 1) {
          a.audio = true;
          a.ch = d[2] | d[3] << 8;
          a.rate = rd32(d + 4);
          a.bits = d[14] | d[15] << 8;
          if ((a.bits != 8 && a.bits != 16) || a.ch < 1 || a.ch > 2) a.audio = false;
        }
      }
    } else if (!memcmp(c, "idx1", 4) && a.moviStart) {
      // индекс: берём только кадры видео (00dc/00db)
      uint32_t n = sz / 16;
      a.vidOff.reserve(min(n, (uint32_t)200000));
      uint8_t e[16];
      bool relative = true, first = true;
      for (uint32_t i = 0; i < n; i++) {
        if (f.read(e, 16) != 16) break;
        if (e[2] != 'd' || (e[3] != 'c' && e[3] != 'b')) continue;
        uint32_t off = rd32(e + 8);
        if (first) { relative = off < a.moviStart; first = false; }
        a.vidOff.push_back(relative ? a.moviStart - 4 + off : off);
      }
    }
    pos += 8 + sz + (sz & 1);
  }
  if (!a.moviStart) { err = "В файле нет данных (movi)"; return false; }
  if (a.w <= 0 || a.h <= 0 || a.w > 640 || a.h > 480) { err = "Неподдерживаемый размер кадра " + String(a.w) + "x" + String(a.h); return false; }
  if (!a.usPerFrame) a.usPerFrame = 66666;
  return true;
}

// Звук видео идёт отдельной задачей из кольцевого буфера: чтение карты и вывод кадров его не задерживают
struct AviAudio {
  StreamBufferHandle_t sb = nullptr;
  StaticStreamBuffer_t sbs;
  uint8_t* store = nullptr;
  size_t size = 0;
  int ch = 1, bits = 16;
  volatile bool run = false, paused = false, flush = false, done = false;
  TaskHandle_t task = nullptr;

  static void loop(void* arg) {
    AviAudio* a = (AviAudio*)arg;
    static uint8_t b[2048];
    while (a->run) {
      if (a->flush) { xStreamBufferReset(a->sb); a->flush = false; }
      if (a->paused) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
      size_t n = xStreamBufferReceive(a->sb, b, sizeof(b), pdMS_TO_TICKS(20));
      if (n) spkPlay(b, n - n % (a->ch * a->bits / 8), a->ch, a->bits);
    }
    a->done = true;
    vTaskDelete(nullptr);
  }
  bool begin(int rate, int c, int bt) {
    ch = c; bits = bt;
    size = (size_t)rate * ch * bits / 8 * 3;   // 3 секунды запаса
    store = (uint8_t*)ps_malloc(size + 1);
    if (!store) return false;
    sb = xStreamBufferCreateStatic(size, 1, store, &sbs);
    if (!spkBegin(rate)) return false;
    run = true;
    xTaskCreatePinnedToCore(loop, "aviaudio", 4096, this, 5, &task, 0);
    return true;
  }
  size_t queued() const { return sb ? xStreamBufferBytesAvailable(sb) : 0; }
  size_t space() const { return sb ? xStreamBufferSpacesAvailable(sb) : 0; }
  void push(const uint8_t* d, size_t n) { if (sb) xStreamBufferSend(sb, d, n, pdMS_TO_TICKS(500)); }
  void end() {
    if (run) { run = false; uint32_t t0 = millis(); while (!done && millis() - t0 < 500) delay(5); }
    spkEnd();
    if (store) { free(store); store = nullptr; }
  }
};

inline void videoPlay(const String& path) {
  input.waitRelease();
  File f = sdOpenRead(path);
  if (!f) { messageBox("Видео", "Не удалось открыть файл"); return; }
  AviInfo ai;
  String err;
  toast("Открываю видео...");
  mark("Видео: разбор AVI");
  if (!aviParse(f, ai, err)) { f.close(); messageBox("Видео", err); return; }

  AviAudio* au = new AviAudio();
  if (ai.audio && !au->begin(ai.rate, ai.ch, ai.bits)) { ai.audio = false; au->end(); }
  au->paused = true;   // звук стартует вместе с часами видео
  // задержка звука в DMA I2S (буфер почти всегда полон): на столько же сдвигаем кадры
  const int64_t audioLatUs = ai.audio ? (int64_t)5500 * 1000000 / ai.rate : 0;   // ~5.5 буфера DMA по 1000 кадров
  const uint32_t bytesPerSec = ai.audio ? ai.rate * ai.ch * ai.bits / 8 : 0;

  struct Frame { uint8_t* d; uint32_t len; uint32_t idx; };
  std::deque<Frame> q;
  const size_t MAXQ = 30;
  uint32_t readPos = ai.moviStart;
  uint32_t vidIdx = 0;          // номер следующего прочитанного кадра
  uint32_t shown = 0, dropped = 0;
  bool eof = false, paused = false, quit = false;
  int x0 = (SCR_W - ai.w) / 2, y0 = (SCR_H - ai.h) / 2;
  int64_t clock0 = 0;           // esp_timer в момент кадра 0
  bool started = false;
  int64_t pausedAt = 0;
  uint32_t osdUntil = 0;
  int volAtStart = settings.volume;
  uint8_t* audBuf = (uint8_t*)ps_malloc(16384);
  lcd.fillScreen(0x0000);

  auto clearQ = [&]() { for (auto& fr : q) free(fr.d); q.clear(); };
  auto curSec = [&]() -> uint32_t {
    if (!started) return 0;
    int64_t now = paused ? pausedAt : esp_timer_get_time();
    return (uint32_t)((now - clock0) / 1000000);
  };
  auto totalSec = [&]() -> uint32_t { return (uint32_t)((uint64_t)max(ai.frames, (uint32_t)ai.vidOff.size()) * ai.usPerFrame / 1000000); };
  auto drawOsd = [&]() {
    int y = SCR_H - 22;
    lcd.fillRect(0, y, SCR_W, 22, 0x0000);
    lcd.setTextColor(0xFFFF, 0x0000);
    lcd.setFont(&fonts::Font0);
    lcd.setTextDatum(middle_left);
    lcd.drawString(mmss(curSec()) + " / " + mmss(totalSec()), 6, y + 11);
    lcd.setTextDatum(middle_right);
    lcd.drawString(String(paused ? "II  " : "") + "vol " + String(settings.volume * 5) + "%", SCR_W - 6, y + 11);
    uint32_t tot = totalSec();
    if (tot) lcd.fillRect(0, y, (int)((uint64_t)SCR_W * min(curSec(), tot) / tot), 2, C_ACCENT);
  };
  auto seekTo = [&](int32_t sec) {
    if (ai.vidOff.empty()) { osdUntil = millis() + 1500; return; }
    if (sec < 0) sec = 0;
    uint32_t k = (uint32_t)((uint64_t)sec * 1000000 / ai.usPerFrame);
    if (k >= ai.vidOff.size()) k = ai.vidOff.size() - 1;
    clearQ();
    if (ai.audio) {
      au->paused = true;
      au->flush = true;
      uint32_t t0 = millis();
      while (au->flush && millis() - t0 < 300) delay(1);   // буфер очищен - только потом подкачиваем новое
    }
    readPos = ai.vidOff[k];
    vidIdx = k;
    eof = false;
    started = false;   // перезапуск часов после подкачки
    lcd.fillScreen(0x0000);
    osdUntil = millis() + 1500;
  };

  mark("Видео: воспроизведение");
  while (!quit) {
    // 1. подкачка: держим ~1.5 с звука и немного кадров впереди
    while (!eof && q.size() < MAXQ && (q.size() < 6 || (ai.audio && au->queued() < bytesPerSec * 3 / 2 && au->space() > 16384))) {
      if (readPos + 8 > ai.moviEnd) { eof = true; break; }
      f.seek(readPos);
      uint8_t c[8];
      if (f.read(c, 8) != 8) { eof = true; break; }
      uint32_t sz = rd32(c + 4);
      uint32_t next = readPos + 8 + sz + (sz & 1);
      if (!memcmp(c, "LIST", 4)) { next = readPos + 12; }   // вложенный LIST rec - заходим внутрь
      else if (c[2] == 'd' && (c[3] == 'c' || c[3] == 'b') && sz > 0 && sz < 512 * 1024) {
        uint8_t* d = (uint8_t*)ps_malloc(sz);
        if (d && f.read(d, sz) == (int)sz) q.push_back({d, sz, vidIdx});
        else if (d) free(d);
        vidIdx++;
      } else if (c[2] == 'w' && c[3] == 'b' && ai.audio && audBuf) {
        uint32_t left = sz;
        while (left) {
          size_t n = f.read(audBuf, min(left, (uint32_t)16384));
          if (!n) break;
          au->push(audBuf, n);
          left -= n;
        }
      }
      readPos = next;
    }
    if (eof && q.empty()) break;

    // 2. запуск часов, когда набран запас
    if (!started && !paused && !q.empty()) {
      clock0 = esp_timer_get_time() - (int64_t)q.front().idx * ai.usPerFrame + audioLatUs;
      started = true;
      if (ai.audio) au->paused = false;
    }

    // 3. кадр: показываем вовремя, опоздавшие пропускаем (звук главный)
    if (started && !paused && !q.empty()) {
      Frame fr = q.front();
      int64_t due = clock0 + (int64_t)fr.idx * ai.usPerFrame;
      int64_t now = esp_timer_get_time();
      if (now >= due) {
        q.pop_front();
        if (now - due > (int64_t)ai.usPerFrame * 2 && !q.empty()) dropped++;
        else { lcd.drawJpg(fr.d, fr.len, x0, y0); shown++; }
        free(fr.d);
        if (osdUntil && millis() < osdUntil) drawOsd();
        else if (osdUntil) { osdUntil = 0; lcd.fillRect(0, SCR_H - 22, SCR_W, 22, 0x0000); }
      } else if (due - now > 3000) delay(2);
    }

    // 4. управление
    input.update();
    if (input.pressed(B_B) || input.pressed(B_MENU)) { quit = true; break; }
    if (input.pressed(B_A)) {
      paused = !paused;
      if (ai.audio) au->paused = paused;
      if (paused) { pausedAt = esp_timer_get_time(); drawOsd(); }
      else { clock0 += esp_timer_get_time() - pausedAt; lcd.fillRect(0, SCR_H - 22, SCR_W, 22, 0x0000); }
    }
    if (input.pressed(B_RIGHT)) seekTo(curSec() + 10);
    if (input.pressed(B_LEFT)) seekTo((int32_t)curSec() - 10);
    if (input.pressed(B_UP)) { settings.volume = min(20, settings.volume + 1); osdUntil = millis() + 1500; if (paused) drawOsd(); }
    if (input.pressed(B_DOWN)) { settings.volume = max(0, settings.volume - 1); osdUntil = millis() + 1500; if (paused) drawOsd(); }
    if (paused) delay(20);
  }
  // дать доиграть звук последних кадров
  if (!quit && eof && ai.audio) { uint32_t t0 = millis(); while (au->queued() && millis() - t0 < 3000) delay(20); }
  clearQ();
  au->end();
  delete au;
  if (audBuf) free(audBuf);
  f.close();
  Serial.printf("[Видео] показано %u, пропущено %u\n", (unsigned)shown, (unsigned)dropped);
  if (settings.volume != volAtStart) settings.save();
  input.waitRelease();
  // файл кончился раньше, чем обещает заголовок AVI - почти всегда оборванная загрузка
  if (!quit && ai.frames > 0 && vidIdx + 15 < ai.frames) {
    uint32_t got = (uint32_t)((uint64_t)vidIdx * ai.usPerFrame / 1000000), all = (uint32_t)((uint64_t)ai.frames * ai.usPerFrame / 1000000);
    messageBox("Видео обрезано", "В файле только " + mmss(got) + " из " + mmss(all) +
               ". Скорее всего, передача на приставку оборвалась. Отправьте видео заново.");
  }
}
