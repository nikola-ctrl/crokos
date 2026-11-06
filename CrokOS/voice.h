// Голосовой ввод: микрофон INMP441 (I2S) -> WAV в PSRAM -> Gemini распознаёт речь в текст.
// Нужен только ключ Gemini (тот же, что для чата). Работает при любой выбранной нейросети
// и через «свой сервер» (AiClient).
#pragma once
#include "hw.h"
#include "ui.h"
#include "settings.h"
#include "gemini.h"
#include <driver/i2s_std.h>
#include <mbedtls/base64.h>

#define VOICE_RATE      16000
#define VOICE_MAX_SEC   30
#define VOICE_MIN_MS    500

static i2s_chan_handle_t micRx = nullptr;

inline bool micBegin() {
  if (micRx) return true;
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 512;   // 8 x 512 кадров = 256 мс запаса, пока рисуется экран
  if (i2s_new_channel(&cc, nullptr, &micRx) != ESP_OK) { micRx = nullptr; return false; }
  i2s_std_config_t sc = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)PIN_MIC_SCK,
      .ws = (gpio_num_t)PIN_MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)PIN_MIC_SD,
      .invert_flags = {false, false, false},
    },
  };
  sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   // L/R микрофона на GND = левый канал
  if (i2s_channel_init_std_mode(micRx, &sc) != ESP_OK || i2s_channel_enable(micRx) != ESP_OK) {
    i2s_del_channel(micRx);
    micRx = nullptr;
    return false;
  }
  return true;
}

inline void micEnd() {
  if (!micRx) return;
  i2s_channel_disable(micRx);
  i2s_del_channel(micRx);
  micRx = nullptr;
}

// Запись WAV (16 кГц, 16 бит, моно) в буфер PSRAM. Пишем, пока зажата holdBtn
// (или до A/B, если кнопку уже отпустили). Буфер освобождает вызывающий (free()).
inline bool voiceRecord(Btn holdBtn, uint8_t*& wav, size_t& wavLen, String& err) {
  wav = nullptr; wavLen = 0;
  const size_t maxN = (size_t)VOICE_RATE * VOICE_MAX_SEC;
  uint8_t* buf = (uint8_t*)ps_malloc(44 + maxN * 4);
  if (!buf) { err = "Не хватает памяти для записи."; return false; }
  int32_t* smp = (int32_t*)(buf + 44);
  if (!micBegin()) { free(buf); err = "Не удалось запустить микрофон (I2S)."; return false; }

  // первые ~60 мс после запуска INMP441 выдаёт мусор - пропускаем
  static int32_t chunk[512];
  size_t got = 0;
  for (int i = 0; i < 4; i++) i2s_channel_read(micRx, chunk, sizeof(chunk), &got, 50);

  size_t n = 0;
  uint32_t t0 = millis(), lastDraw = 0;
  float level = 0;
  bool holding = true;
  while (n < maxN) {
    if (i2s_channel_read(micRx, chunk, sizeof(chunk), &got, 100) == ESP_OK) {
      size_t k = got / 4;
      int64_t acc = 0;
      for (size_t i = 0; i < k && n < maxN; i++) {
        int32_t v = chunk[i] >> 8;   // 24 бита данных в старших разрядах 32-битного слота
        smp[n++] = v;
        acc += (int64_t)(v >> 8) * (v >> 8);
      }
      if (k) level = level * 0.6f + 0.4f * sqrtf((float)(acc / (int64_t)k));
    }
    input.update();
    if (holding && !input.held(holdBtn)) holding = false;
    if (!holding && holdBtn != B_COUNT) break;                       // отпустили - стоп
    if (input.pressed(B_B) || input.pressed(B_MENU)) { n = 0; break; } // отмена
    if (input.pressed(B_A)) break;
    uint32_t now = millis();
    if (now - lastDraw > 120) {
      lastDraw = now;
      canvas.fillScreen(C_BG);
      drawHeader("Голосовой ввод");
      canvas.setFont(FONT);
      canvas.setTextDatum(middle_center);
      canvas.setTextColor(C_TEXT);
      canvas.drawString("Говорите...", SCR_W / 2, 80);
      int sec = (now - t0) / 1000;
      canvas.setTextColor(C_ACCENT_TEXT);
      canvas.drawString(String(sec) + " / " + String(VOICE_MAX_SEC) + " с", SCR_W / 2, 108);
      // индикатор громкости
      int w = SCR_W - 60;
      int lv = constrain((int)(level / 2500.0f * w), 0, w);
      canvas.fillRoundRect(30, 132, w, 14, 4, C_PANEL);
      canvas.fillRoundRect(30, 132, max(lv, 4), 14, 4, lv > w * 0.85 ? C_ERR : C_OK);
      canvas.setFont(FONT_S);
      canvas.setTextColor(C_DIM);
      canvas.drawString("Отпустите кнопку, когда закончите", SCR_W / 2, 168);
      canvas.drawString("B: отмена", SCR_W / 2, 186);
      present();
    }
  }
  micEnd();
  uint32_t ms = (uint32_t)((uint64_t)n * 1000 / VOICE_RATE);
  input.waitRelease();
  if (n == 0) { free(buf); err = ""; return false; }   // отменено
  if (ms < VOICE_MIN_MS) { free(buf); err = "Слишком короткая запись. Держите кнопку, пока говорите."; return false; }

  // Убираем постоянную составляющую и нормируем громкость (тихий микрофон -> до -3 дБ, усиление до 64x)
  int64_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += smp[i];
  int32_t dc = (int32_t)(sum / (int64_t)n);
  int32_t peak = 0;
  for (size_t i = 0; i < n; i++) { int32_t a = abs(smp[i] - dc); if (a > peak) peak = a; }
  if (peak < 64) {
    free(buf);
    err = "Микрофон молчит. Проверьте подключение INMP441: SCK - GPIO41, WS - GPIO42, SD - GPIO40, L/R - GND, VDD - 3.3 В.";
    return false;
  }
  // 24-битный отсчёт -> 16 бит: делим на 256, затем усиливаем до пика ~23000
  float gain = 23000.0f * 256.0f / (float)peak;
  if (gain > 64.0f) gain = 64.0f;
  int16_t* out = (int16_t*)(buf + 44);   // пишем поверх: out[i] (2 байта) всегда не дальше smp[i] (4 байта)
  for (size_t i = 0; i < n; i++) {
    float v = (float)(smp[i] - dc) * gain / 256.0f;
    out[i] = (int16_t)constrain((int)v, -32767, 32767);
  }
  // заголовок WAV
  uint32_t dataLen = n * 2;
  auto w32 = [&](int off, uint32_t v) { buf[off] = v; buf[off + 1] = v >> 8; buf[off + 2] = v >> 16; buf[off + 3] = v >> 24; };
  auto w16 = [&](int off, uint16_t v) { buf[off] = v; buf[off + 1] = v >> 8; };
  memcpy(buf, "RIFF", 4); w32(4, 36 + dataLen); memcpy(buf + 8, "WAVEfmt ", 8);
  w32(16, 16); w16(20, 1); w16(22, 1); w32(24, VOICE_RATE); w32(28, VOICE_RATE * 2); w16(32, 2); w16(34, 16);
  memcpy(buf + 36, "data", 4); w32(40, dataLen);
  wav = buf;
  wavLen = 44 + dataLen;
  Serial.printf("[Voice] %u мс, пик %d, усиление %.1f, WAV %u Б\n", (unsigned)ms, (int)peak, gain, (unsigned)wavLen);
  return true;
}

static const char VOICE_PROMPT[] =
  "Перепиши дословно речь из этой аудиозаписи на том языке, на котором говорят. "
  "Расставь знаки препинания и заглавные буквы. Выведи только сам текст, без кавычек, "
  "пояснений и пометок. Если речи не слышно, выведи пустую строку.";

// Распознавание WAV через Gemini (основная модель, при перегрузке - запасная)
inline bool voiceTranscribe(const uint8_t* wav, size_t wavLen, String& text, String& err) {
  size_t b64Len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Len, wav, wavLen);
  const String pre = "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
  const String post = String("\"}},{\"text\":\"") + VOICE_PROMPT + "\"}]}],\"generationConfig\":{\"temperature\":0}}";
  size_t cap = pre.length() + b64Len + post.length() + 1;
  char* body = (char*)ps_malloc(cap);
  if (!body) { err = "Не хватает памяти для отправки записи."; return false; }
  memcpy(body, pre.c_str(), pre.length());
  size_t olen = 0;
  mbedtls_base64_encode((unsigned char*)body + pre.length(), b64Len, &olen, wav, wavLen);
  memcpy(body + pre.length() + olen, post.c_str(), post.length());
  size_t len = pre.length() + olen + post.length();
  body[len] = 0;

  std::vector<String> models = {settings.gemModel};
  if (settings.gemFallback.length() && settings.gemFallback != settings.gemModel) models.push_back(settings.gemFallback);
  bool ok = false;
  for (size_t mi = 0; mi < models.size() && !ok; mi++) {
    String url = geminiBaseUrl() + "/v1beta/models/" + models[mi] + ":generateContent";
    String resp;
    int code = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
      if (attempt) delay(400 * attempt);
      code = aiPostRaw(url, (const uint8_t*)body, len, true, resp);
      if (code != HTTPC_ERROR_CONNECTION_REFUSED) break;
    }
    if (code <= 0) {
      err = "Нет связи с сервером (" + HTTPClient::errorToString(code) + ")." + (lastTlsErr.length() ? " " + lastTlsErr : String(""));
      break;
    }
    String perr;
    if (code == 200 && parseReply(resp, text, perr)) { ok = true; break; }
    if (code == 200) { text = ""; ok = true; break; }   // пустой ответ = речи не было
    if ((code == 429 || code == 500 || code == 503) && mi + 1 < models.size()) { toast("Пробую запасную модель..."); continue; }
    if (code == 400 && perr.indexOf("API key") >= 0) err = "Ключ Gemini не подходит (400). Голосовому вводу нужен ключ Gemini из aistudio.google.com.";
    else if (code == 403 || perr.indexOf("location") >= 0) err = "Google не пускает запрос из России. В настройках нейросети выберите «Подключение» -> «Через свой сервер».";
    else if (code == 429) err = "Лимит бесплатного ключа Gemini исчерпан (429). Попробуйте позже.";
    else err = "Ошибка Gemini " + String(code) + (perr.length() ? ": " + shortMsg(perr) : String(""));
  }
  free(body);
  if (ok) { text.trim(); text = cleanReply(text); text.trim(); }
  return ok;
}

// Полный цикл: запись (пока зажата holdBtn) -> Wi-Fi -> распознавание. true и текст в out.
inline bool voiceDictate(Btn holdBtn, String& out) {
  out = "";
  if (!settings.gemKey.length()) {
    messageBox("Голосовой ввод", "Для распознавания речи нужен ключ Gemini (бесплатный, aistudio.google.com). Укажите его: Нейросеть -> START -> Нейросеть: Gemini -> Ключ API.");
    return false;
  }
  uint8_t* wav = nullptr;
  size_t wavLen = 0;
  String err;
  if (!voiceRecord(holdBtn, wav, wavLen, err)) {
    if (err.length()) messageBox("Голосовой ввод", err);
    return false;
  }
  toast("Подключение...");
  if (!wifiConnect(err)) { free(wav); messageBox("Wi-Fi", err); return false; }
  applyDns();
  toast("Распознаю речь...");
  String text;
  bool ok = voiceTranscribe(wav, wavLen, text, err);
  free(wav);
  if (!ok) { messageBox("Голосовой ввод", err); return false; }
  if (!text.length()) { messageBox("Голосовой ввод", "Речь не распознана. Говорите ближе к микрофону."); return false; }
  out = text;
  return true;
}

// Регистрируем обработчик для textInput и редактора (ui.h не знает про сеть)
static bool voiceHookImpl(String& out) { return voiceDictate(B_OPTION, out); }
static struct VoiceReg { VoiceReg() { voiceHook() = voiceHookImpl; } } voiceReg_;
