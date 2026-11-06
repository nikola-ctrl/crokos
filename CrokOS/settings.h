// ============================================================================
//  CrokOS - настройки: тема, гамма фото, гамма и параметры экрана. Хранятся на SD в /.crokos.cfg
// ============================================================================
#pragma once
#include <vector>
#include "ui.h"
#include <math.h>

#define SETTINGS_FILE "/.crokos.cfg"
#define INI_FILE "/crokos.ini"

static const float PHOTO_GAMMAS[] = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.6f, 1.8f, 2.0f, 2.2f};
static const int PHOTO_GAMMA_COUNT = sizeof(PHOTO_GAMMAS) / sizeof(PHOTO_GAMMAS[0]);
static const int PHOTO_GAMMA_DEFAULT = 5;   // 1.0 = без изменений

// Альбомные ориентации LovyanGFX (с обменом осей). Вариант 2 (rotation 3) рассчитан
// по тому, как Retro-Go настраивает этот ST7789; остальные - на случай ошибки расчёта.
static const uint8_t DISP_ROTATIONS[] = {1, 3, 5, 7};
static const int DISP_ROT_DEFAULT = 1;

struct Settings {
  bool lightTheme = false;
  int photoGamma = PHOTO_GAMMA_DEFAULT;   // индекс в PHOTO_GAMMAS
  bool panelGammaRG = true;               // гамма-кривые экрана как в Retro-Go
  int dispRot = DISP_ROT_DEFAULT;         // индекс в DISP_ROTATIONS
  bool dispInvert = false;                // INVON
  bool dispBGR = true;                    // порядок цветов BGR
  int volume = 12;                        // громкость плеера 0..20
  // Wi-Fi (клиент) и Gemini. Хранятся открытым текстом на SD - файл скрыт от веб-загрузчика
  String wifiSsid, wifiPass;
  String gemKey, gemProxy;
  String gemModel = "gemini-3.8-flash";
  String gemDns;                          // пусто = DNS от роутера; иначе "ip1" или "ip1,ip2"
  // Свой сервер (VPN): "адрес:порт". TCP идёт на него, а TLS - сквозной до настоящего сервиса
  // (SNI и сертификат сервиса). Сервер пересылает соединение дальше по SNI (Xray dokodemo-door + sniffing).
  String gemRelay;
  String gemFallback = "gemini-3.5-flash-lite";   // запасная модель, если основная перегружена
  // Какая нейросеть: 0 = Gemini, 1 = OpenRouter, 2 = любой OpenAI-совместимый сервис (по умолчанию UnoRouter)
  int aiProvider = 0;
  String apiUrl = "https://api.unorouter.com/v1";
  String apiKey;
  String apiModel = "qwen3.8-27b:free";
  String apiFallback = "kimi-k3:free";
  String orKey;
  String orModel = "google/gemma-4-31b-it:free";
  String orFallback = "openrouter/free";           // «любая свободная бесплатная модель»

  void resetDisplay() { dispRot = DISP_ROT_DEFAULT; dispInvert = false; dispBGR = true; }

  void load() {
    if (!sdOk) return;
    loaded = true;   // карта доступна: дальше можно сохранять
    File f = SD.open(SETTINGS_FILE, FILE_READ);
    if (!f) return;
    while (f.available()) {
      String line;
      int c;
      while ((c = f.read()) >= 0 && c != '\n') if (c != '\r') line += (char)c;
      int eq = line.indexOf('=');
      if (eq <= 0) continue;
      String k = line.substring(0, eq), v = line.substring(eq + 1);
      int n = atoi(v.c_str());
      if (k == "lightTheme") lightTheme = n != 0;
      else if (k == "photoGamma") photoGamma = constrain(n, 0, PHOTO_GAMMA_COUNT - 1);
      else if (k == "panelGammaRG") panelGammaRG = n != 0;
      else if (k == "dispRot") dispRot = constrain(n, 0, 3);
      else if (k == "dispInvert") dispInvert = n != 0;
      else if (k == "dispBGR") dispBGR = n != 0;
      else if (k == "volume") volume = constrain(n, 0, 20);
      else if (k == "wifiSsid") wifiSsid = v;
      else if (k == "wifiPass") wifiPass = v;
      else if (k == "gemKey") gemKey = v;
      else if (k == "gemProxy") gemProxy = v;
      else if (k == "gemModel" && v.length()) gemModel = v;
      else if (k == "gemDns") gemDns = v;
      else if (k == "gemRelay") gemRelay = v;
      else if (k == "gemFallback") gemFallback = v;
      else if (k == "aiProvider") aiProvider = constrain(n, 0, 2);
      else if (k == "apiUrl" && v.length()) apiUrl = v;
      else if (k == "apiKey") apiKey = v;
      else if (k == "apiModel" && v.length()) apiModel = v;
      else if (k == "apiFallback") apiFallback = v;
      else if (k == "orKey") orKey = v;
      else if (k == "orModel" && v.length()) orModel = v;
      else if (k == "orFallback") orFallback = v;
    }
    f.close();
    dropDeadDns();
  }
  // XBox DNS закрыт (РКН) - старое значение из настроек убираем, иначе никакие запросы не пройдут
  void dropDeadDns() { if (gemDns == "111.88.96.54,111.88.96.55" || gemDns == "111.88.96.54" || gemDns == "111.88.96.55") gemDns = ""; }
  bool loaded = false;   // файл настроек прочитан (иначе сохранять нельзя - затрём значения по умолчанию)

  bool save() {
    if (!sdOk || !loaded) return false;
    String s = "lightTheme=" + String(lightTheme ? 1 : 0) + "\nphotoGamma=" + String(photoGamma) +
               "\npanelGammaRG=" + String(panelGammaRG ? 1 : 0) + "\ndispRot=" + String(dispRot) +
               "\ndispInvert=" + String(dispInvert ? 1 : 0) + "\ndispBGR=" + String(dispBGR ? 1 : 0) + "\nvolume=" + String(volume) +
               "\nwifiSsid=" + wifiSsid + "\nwifiPass=" + wifiPass + "\ngemKey=" + gemKey +
               "\ngemProxy=" + gemProxy + "\ngemModel=" + gemModel + "\ngemDns=" + gemDns + "\ngemRelay=" + gemRelay + "\ngemFallback=" + gemFallback +
               "\naiProvider=" + String(aiProvider) + "\norKey=" + orKey + "\norModel=" + orModel + "\norFallback=" + orFallback +
               "\napiUrl=" + apiUrl + "\napiKey=" + apiKey + "\napiModel=" + apiModel + "\napiFallback=" + apiFallback + "\n";
    File f = SD.open(SETTINGS_FILE ".tmp", FILE_WRITE);
    if (!f) return false;
    size_t w = f.write((const uint8_t*)s.c_str(), s.length());
    f.close();
    if (w != s.length()) { SD.remove(SETTINGS_FILE ".tmp"); return false; }
    SD.remove(SETTINGS_FILE);
    return SD.rename(SETTINGS_FILE ".tmp", SETTINGS_FILE);
  }

  // Файл crokos.ini, который пользователь заполняет сам (на телефоне или в «Тексте»).
  // Непустые значения из него перекрывают сохранённые. Файл не удаляется.
  bool importIni() {
    if (!sdOk) return false;
    File f = SD.open(INI_FILE, FILE_READ);
    if (!f) return false;
    String mode, dns, proxy, relay;
    bool any = false;
    while (f.available()) {
      String line;
      int c;
      while ((c = f.read()) >= 0 && c != '\n') if (c != '\r') line += (char)c;
      if (line.startsWith("\xEF\xBB\xBF")) line = line.substring(3);   // BOM из Блокнота
      line.trim();
      if (!line.length() || line.startsWith("#") || line.startsWith(";")) continue;
      int eq = line.indexOf('=');
      if (eq <= 0) continue;
      String k = line.substring(0, eq), v = line.substring(eq + 1);
      k.trim(); v.trim(); k.toLowerCase();
      if (!v.length()) continue;
      any = true;
      if (k == "wifi_ssid") wifiSsid = v;
      else if (k == "wifi_password") wifiPass = v;
      else if (k == "gemini_key") gemKey = v;
      else if (k == "model") gemModel = v;
      else if (k == "fallback_model") gemFallback = (v == "none" || v == "нет") ? String("") : v;
      else if (k == "provider") { v.toLowerCase(); if (v.startsWith("open")) aiProvider = 1; else if (v.startsWith("gem")) aiProvider = 0; else if (v == "api" || v == "unorouter" || v == "custom") aiProvider = 2; }
      else if (k == "api_url") apiUrl = v;
      else if (k == "api_key") apiKey = v;
      else if (k == "api_model") apiModel = v;
      else if (k == "api_fallback") apiFallback = (v == "none" || v == "нет") ? String("") : v;
      else if (k == "openrouter_key") orKey = v;
      else if (k == "openrouter_model") orModel = v;
      else if (k == "openrouter_fallback") orFallback = (v == "none" || v == "нет") ? String("") : v;
      else if (k == "mode") { v.toLowerCase(); mode = v; }
      else if (k == "dns") dns = v;
      else if (k == "proxy") proxy = v;
      else if (k == "relay" || k == "server") relay = v;
    }
    f.close();
    if (mode == "relay" || mode == "server" || mode == "vpn") { gemDns = ""; gemProxy = ""; if (relay.length()) gemRelay = relay; }
    else if (mode == "dns") { gemDns = dns; gemProxy = ""; gemRelay = ""; }
    else if (mode == "proxy") { gemDns = ""; gemRelay = ""; if (proxy.length()) gemProxy = proxy; }
    else if (mode == "direct") { gemDns = ""; gemProxy = ""; gemRelay = ""; }
    else { if (dns.length()) gemDns = dns; if (proxy.length()) gemProxy = proxy; if (relay.length()) gemRelay = relay; }
    dropDeadDns();
    return any;
  }
  // Записать текущие настройки нейросети обратно в crokos.ini (комментарии и порядок строк сохраняются).
  // Иначе при следующем открытии «Нейросети» значения из файла перекрыли бы изменения, сделанные в меню.
  bool writeIni() {
    if (!sdOk || !loaded || !SD.exists(INI_FILE)) return true;   // файла нет - нечего обновлять
    auto orNone = [](const String& v) { return v.length() ? v : String("none"); };
    String mode = gemRelay.length() ? "relay" : gemProxy.length() ? "proxy" : gemDns.length() ? "dns" : "direct";
    const char* prov = aiProvider == 1 ? "openrouter" : aiProvider == 2 ? "api" : "gemini";
    struct KV { const char* k; String v; bool done; };
    std::vector<KV> kv = {
      {"wifi_ssid", wifiSsid, false}, {"wifi_password", wifiPass, false},
      {"provider", prov, false},
      {"api_url", apiUrl, false}, {"api_key", apiKey, false}, {"api_model", apiModel, false}, {"api_fallback", orNone(apiFallback), false},
      {"openrouter_key", orKey, false}, {"openrouter_model", orModel, false}, {"openrouter_fallback", orNone(orFallback), false},
      {"gemini_key", gemKey, false}, {"mode", mode, false}, {"dns", gemDns, false}, {"proxy", gemProxy, false}, {"relay", gemRelay, false},
      {"model", gemModel, false}, {"fallback_model", orNone(gemFallback), false},
    };
    File f = SD.open(INI_FILE, FILE_READ);
    if (!f) return false;
    String out;
    bool firstLine = true;
    while (f.available()) {
      String line;
      int c;
      while ((c = f.read()) >= 0 && c != '\n') if (c != '\r') line += (char)c;
      if (firstLine && line.startsWith("\xEF\xBB\xBF")) line = line.substring(3);
      firstLine = false;
      String s = line;
      s.trim();
      int eq = s.indexOf('=');
      if (eq > 0 && !s.startsWith("#") && !s.startsWith(";")) {
        String k = s.substring(0, eq);
        k.trim();
        k.toLowerCase();
        for (auto& e : kv) {
          if (k == e.k) {
            // режим dns без своего списка серверов - оставляем строку dns как была
            if (k == "dns" && !e.v.length() && mode != "direct") break;
            if (k == "relay" && !e.v.length()) break;   // адрес сервера не стираем - пригодится при возврате
            if (k == "proxy" && !e.v.length() && mode != "direct" && mode != "dns") break;
            line = String(e.k) + "=" + e.v;
            e.done = true;
            break;
          }
        }
      }
      out += line;
      out += "\n";
    }
    f.close();
    for (auto& e : kv) if (!e.done && e.v.length() && strcmp(e.k, "dns") && strcmp(e.k, "proxy")) out += String(e.k) + "=" + e.v + "\n";
    while (out.endsWith("\n\n")) out = out.substring(0, out.length() - 1);
    File w = SD.open(INI_FILE ".tmp", FILE_WRITE);
    if (!w) return false;
    size_t n = w.write((const uint8_t*)out.c_str(), out.length());
    w.close();
    if (n != out.length()) { SD.remove(INI_FILE ".tmp"); return false; }
    SD.remove(INI_FILE);
    return SD.rename(INI_FILE ".tmp", INI_FILE);
  }
  float gammaValue() const { return PHOTO_GAMMAS[photoGamma]; }
};
extern Settings settings;

// Монтирует SD (с повторами) и при первом успехе читает настройки и crokos.ini
inline bool storageReady() {
  if (!sdOk) {
    for (int i = 0; i < 3 && !sdMount(); i++) delay(150);
  }
  if (sdOk && !settings.loaded) {
    settings.load();
    if (settings.importIni()) settings.save();
    applyTheme(settings.lightTheme);
  }
  return sdOk;
}

// ---- Гамма-кривые самого экрана (контроллер ST7789, 14 байт на кривую) ----
// Вариант Retro-Go взят из targets/crokpocket/config.h (команды 0xE0/0xE1).
// «Стандартный» - гамма драйвера ST7789 из LovyanGFX.
inline void applyPanelGamma(bool retroGo) {
  static const uint8_t RG_P[] = {0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19};
  static const uint8_t RG_N[] = {0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19};
  static const uint8_t LG_P[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17};
  static const uint8_t LG_N[] = {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E};
  const uint8_t* p = retroGo ? RG_P : LG_P;
  const uint8_t* n = retroGo ? RG_N : LG_N;
  int len = retroGo ? sizeof(RG_P) : sizeof(LG_P);
  lcd.startWrite();
  lcd.writeCommand(0xE0);
  for (int i = 0; i < len; i++) lcd.writeData(p[i]);
  lcd.writeCommand(0xE1);
  for (int i = 0; i < len; i++) lcd.writeData(n[i]);
  lcd.endWrite();
}

// ---- Ориентация, инверсия и порядок цветов экрана ----
inline void applyDisplaySettings() {
  auto* panel = lcd.getPanel();
  auto cfg = panel->config();
  cfg.rgb_order = !settings.dispBGR;   // rgb_order=false означает BGR
  panel->config(cfg);
  lcd.setRotation(DISP_ROTATIONS[settings.dispRot]);   // заново отправляет MADCTL с новым порядком цветов
  lcd.invertDisplay(settings.dispInvert);
}

// ---- Гамма-коррекция фото (программная, только в галерее) ----
struct PhotoGammaLUT {
  uint8_t l5[32], l6[64];
  float built = -1;
  void build(float g) {
    for (int i = 0; i < 32; i++) l5[i] = (uint8_t)lroundf(powf(i / 31.f, g) * 31.f);
    for (int i = 0; i < 64; i++) l6[i] = (uint8_t)lroundf(powf(i / 63.f, g) * 63.f);
    built = g;
  }
};

// Применяет гамму ко всему кадру canvas (буфер 16 бит, байты в порядке big-endian)
inline void applyPhotoGamma(LGFX_Sprite& spr, float g) {
  if (fabsf(g - 1.f) < 0.01f) return;
  static PhotoGammaLUT lut;
  if (lut.built != g) lut.build(g);
  uint16_t* buf = (uint16_t*)spr.getBuffer();
  if (!buf) return;
  size_t n = (size_t)spr.width() * spr.height();
  for (size_t i = 0; i < n; i++) {
    uint16_t v = buf[i];
    v = (v >> 8) | (v << 8);
    uint16_t r = lut.l5[v >> 11], gg = lut.l6[(v >> 5) & 63], b = lut.l5[v & 31];
    v = (r << 11) | (gg << 5) | b;
    buf[i] = (v >> 8) | (v << 8);
  }
}

// ---- Экран настроек ----
inline String gammaText(int idx) {
  char b[8];
  snprintf(b, sizeof b, "%.1f", PHOTO_GAMMAS[idx]);
  return b;
}

void volumeBeep();   // media.h: короткий сигнал для проверки громкости

inline void settingsApp() {
  int sel = 0;
  const int N = 7;
  const int ROW = 22;
  bool changed = false;
  input.waitRelease();
  bool dirty = true;
  while (true) {
    if (dirty) {
      canvas.fillScreen(C_BG);
      drawHeader("Настройки");
      const char* names[N] = {"Тема", "Гамма фото", "Гамма экрана", "Поворот экрана", "Инверсия цветов", "Порядок цветов", "Громкость"};
      String vals[N] = {
        settings.lightTheme ? "Светлая" : "Тёмная",
        gammaText(settings.photoGamma) + (settings.photoGamma == PHOTO_GAMMA_DEFAULT ? " (без изм.)" : settings.gammaValue() > 1 ? " (темнее)" : " (светлее)"),
        settings.panelGammaRG ? "Как в Retro-Go" : "Стандартная",
        "Вариант " + String(settings.dispRot + 1),
        settings.dispInvert ? "Вкл" : "Выкл",
        settings.dispBGR ? "BGR" : "RGB",
        settings.volume == 0 ? String("Без звука") : String(settings.volume * 5) + "%",
      };
      const char* hints[N] = {
        "Цвета интерфейса CrokOS",
        "Только для галереи, там же кнопками ↑↓",
        "Гамма-кривая дисплея. Если картинка стала хуже, переключите",
        "Основной - вариант 2. Если экран перевёрнут или отзеркален, перебирайте варианты",
        "Если тёмное стало светлым (негатив), переключите",
        "Если перепутаны красный и синий, переключите",
        "Плеер видео и музыки (динамик MAX98357A). При изменении звучит сигнал",
      };
      canvas.setFont(FONT);
      for (int i = 0; i < N; i++) {
        int y = HEADER_H + 6 + i * ROW;
        bool s = i == sel;
        if (s) canvas.fillRoundRect(4, y, SCR_W - 8, ROW - 2, 4, C_SEL);
        canvas.setTextDatum(middle_left);
        canvas.setTextColor(s ? C_SELTEXT : C_TEXT);
        canvas.drawString(names[i], 12, y + ROW / 2 - 1);
        canvas.setTextDatum(middle_right);
        canvas.setTextColor(s ? C_SELTEXT : C_ACCENT_TEXT);
        canvas.drawString(String(s ? "← " : "") + vals[i] + (s ? " →" : ""), SCR_W - 12, y + ROW / 2 - 1);
      }
      int hy = HEADER_H + 6 + N * ROW + 4;
      canvas.setFont(FONT_S);
      canvas.setTextColor(C_DIM);
      canvas.setTextDatum(top_left);
      auto lines = wrapWords(hints[sel], SCR_W - 24);
      for (size_t i = 0; i < lines.size() && i < 2; i++) canvas.drawString(lines[i], 12, hy + i * 15);
      if (sel == 1) {   // образец гаммы фото
        int gy = hy + 16;
        float g = settings.gammaValue();
        for (int x = 0; x < SCR_W - 24; x++) {
          uint8_t v = (uint8_t)(powf((float)x / (SCR_W - 25), g) * 255);
          canvas.drawFastVLine(12 + x, gy, 9, lcd.color565(v, v, v));
          canvas.drawFastVLine(12 + x, gy + 10, 9, lcd.color565(v, (uint8_t)(v * 0.55f), 0));
        }
      }
      drawFooter("↑↓: пункт  ←→: изменить  B: назад");
      present();
      dirty = false;
    }
    input.update();
    if (!input.any()) { delay(10); continue; }
    dirty = true;
    if (input.pressed(B_B) || input.pressed(B_MENU)) break;
    if (input.pressed(B_UP)) sel = (sel + N - 1) % N;
    if (input.pressed(B_DOWN)) sel = (sel + 1) % N;
    int d = input.pressed(B_RIGHT) || input.pressed(B_A) ? 1 : input.pressed(B_LEFT) ? -1 : 0;
    if (d) {
      changed = true;
      switch (sel) {
        case 0: settings.lightTheme = !settings.lightTheme; applyTheme(settings.lightTheme); break;
        case 1: settings.photoGamma = constrain(settings.photoGamma + d, 0, PHOTO_GAMMA_COUNT - 1); break;
        case 2: settings.panelGammaRG = !settings.panelGammaRG; applyPanelGamma(settings.panelGammaRG); break;
        case 3: settings.dispRot = (settings.dispRot + d + 4) % 4; applyDisplaySettings(); break;
        case 4: settings.dispInvert = !settings.dispInvert; applyDisplaySettings(); break;
        case 5: settings.dispBGR = !settings.dispBGR; applyDisplaySettings(); break;
        case 6: {
          int v = constrain(settings.volume + d, 0, 20);
          if (input.pressed(B_A)) v = settings.volume;   // A - просто проверить звук
          settings.volume = v;
          volumeBeep();
          break;
        }
      }
    }
  }
  if (changed && !settings.save()) messageBox("Настройки", sdOk ? "Не удалось записать настройки на SD-карту." : "Нет SD-карты: настройки действуют до выключения.");
  input.waitRelease();
}
