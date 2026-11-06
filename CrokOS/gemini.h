// ============================================================================
//  CrokOS - чат с Gemini через Wi-Fi (Cloudflare Worker-прокси, т.к. из РФ Google API недоступен)
// ============================================================================
#pragma once
#include "ui.h"
#include "settings.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include "root_cas.h"

#define GEMINI_DIRECT "https://generativelanguage.googleapis.com"
#define GEMINI_HISTORY_DIR "/Gemini"
#define GEMINI_MAX_TURNS 20   // сколько последних сообщений отправлять как контекст
#define RELAY_DEFAULT_PORT 8443

// Адрес своего сервера из настроек ("host:port" или "host") -> IP и порт
inline bool relayTarget(IPAddress& ip, uint16_t& port) {
  String r = settings.gemRelay;
  r.trim();
  if (!r.length()) return false;
  if (r.startsWith("https://")) r = r.substring(8); else if (r.startsWith("http://")) r = r.substring(7);
  int sl = r.indexOf('/');
  if (sl >= 0) r = r.substring(0, sl);
  int c = r.lastIndexOf(':');
  port = RELAY_DEFAULT_PORT;
  if (c > 0) { int p = r.substring(c + 1).toInt(); if (p > 0 && p < 65536) port = p; r = r.substring(0, c); }
  if (ip.fromString(r)) return true;
  return Network.hostByName(r.c_str(), ip) == 1;
}

// HTTPS-клиент нейросети. В режиме «свой сервер» TCP-соединение открывается на сервер (IP:порт),
// а TLS устанавливается с настоящим сервисом: SNI и проверка сертификата - по его имени.
// Сервер видит только зашифрованный поток и пересылает его по SNI, ключ API ему не виден.
class AiClient : public NetworkClientSecure {
public:
  bool viaRelay = false;
  int connect(const char* host, uint16_t port, int32_t timeout) override {
    _timeout = timeout;
    IPAddress rip;
    uint16_t rport;
    viaRelay = relayTarget(rip, rport);
    if (viaRelay) {
      Serial.printf("[AI] %s:%u через сервер %s:%u\n", host, port, rip.toString().c_str(), rport);
      return NetworkClientSecure::connect(rip, rport, host, _CA_cert, _cert, _private_key);
    }
    return NetworkClientSecure::connect(host, port, timeout);
  }
};

static const char GEMINI_SYSTEM[] =
  "Ты встроенный помощник карманной консоли CrokPocket с маленьким экраном 320x240. "
  "Отвечай кратко и по делу, на языке вопроса. Пиши обычным текстом без Markdown, "
  "без таблиц, без эмодзи и без форматирования. Списки оформляй простыми строками.";

// ---------------------------------------------------------------------------
// Какие символы есть в шрифте (см. tools/make_vlw.py); остальное заменяем
inline bool glyphOk(uint32_t c) {
  if (c == '\n' || (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF) || (c >= 0x400 && c <= 0x45F)) return true;
  static const uint16_t extra[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x2116, 0x2190, 0x2191,
                                   0x2192, 0x2193, 0x221A, 0x221E, 0x2260, 0x2264, 0x2265, 0x03C0, 0x20AC, 0x2122, 0x2212, 0x25B2, 0x25BC, 0x25CF};
  for (auto e : extra) if (c == e) return true;
  return false;
}

// Упрощение ответа для маленького экрана: убираем Markdown, неподдерживаемые символы
inline String cleanReply(const String& in) {
  String s = in;
  s.replace("**", ""); s.replace("__", ""); s.replace("`", "");
  s.replace("\r", "");
  String out;
  int start = 0;
  while (start <= (int)s.length()) {
    int nl = s.indexOf('\n', start);
    String line = s.substring(start, nl < 0 ? s.length() : nl);
    // заголовки "# ..." и маркеры списков "* " / "- "
    int k = 0;
    while (k < (int)line.length() && line[k] == '#') k++;
    if (k > 0 && k < (int)line.length() && line[k] == ' ') line = line.substring(k + 1);
    if (line.startsWith("* ") || line.startsWith("- ")) line = "• " + line.substring(2);
    else if (line.startsWith("  * ") || line.startsWith("  - ")) line = "  • " + line.substring(4);
    out += line;
    if (nl < 0) break;
    out += '\n';
    start = nl + 1;
  }
  // схлопываем больше двух пустых строк и заменяем неизвестные символы
  auto v = utf8Split(out);
  std::vector<uint32_t> r;
  int nls = 0;
  for (auto c : v) {
    if (c == '\n') { if (++nls > 2) continue; }
    else nls = 0;
    if (c == '\t') c = ' ';
    // эмодзи и служебные символы просто убираем, прочие незнакомые - '?'
    if (c > 0xFFFF || (c >= 0x2600 && c <= 0x27BF) || c == 0xFE0F || c == 0x200D || c == 0x200B) continue;
    r.push_back(glyphOk(c) ? c : (uint32_t)'?');
  }
  std::vector<uint32_t> r2;
  for (auto c : r) { if (c == ' ' && !r2.empty() && r2.back() == ' ') continue; r2.push_back(c); }
  r.swap(r2);
  while (!r.empty() && (r.back() == '\n' || r.back() == ' ')) r.pop_back();
  return utf8Encode(r);
}

// Перенос по словам с разрывом слишком длинных слов (ссылки и т.п.)
inline void wrapInto(const String& text, int maxw, std::vector<String>& out) {
  int start = 0;
  while (start <= (int)text.length()) {
    int nl = text.indexOf('\n', start);
    String para = text.substring(start, nl < 0 ? text.length() : nl);
    auto v = utf8Split(para);
    if (v.empty()) out.push_back("");
    size_t i = 0;
    while (i < v.size()) {
      // набираем строку посимвольно, запоминая последний пробел
      size_t j = i, lastSp = (size_t)-1;
      String line;
      while (j < v.size()) {
        String cand = line; utf8Append(cand, v[j]);
        if (canvas.textWidth(cand) > maxw && j > i) break;
        line = cand;
        if (v[j] == ' ') lastSp = j;
        j++;
      }
      if (j < v.size() && lastSp != (size_t)-1 && lastSp > i) {
        out.push_back(utf8Encode(v, i, lastSp - i));
        i = lastSp + 1;
      } else {
        out.push_back(line);
        i = j;
      }
    }
    if (nl < 0) break;
    start = nl + 1;
  }
}

// ---------------------------------------------------------------------------
// Wi-Fi: подключение и выбор сети
// Свой DNS поверх DHCP (повторяем перед каждым запросом - продление аренды DHCP может его сбросить)
static IPAddress routerDns1, routerDns2;
static bool customDnsSet = false;
inline void applyDns() {
  if (!settings.gemDns.length()) {
    // DNS не задан - возвращаем DNS от роутера, если меняли
    if (customDnsSet && (uint32_t)routerDns1 != 0) WiFi.setDNS(routerDns1, routerDns2);
    customDnsSet = false;
    return;
  }
  String s = settings.gemDns;
  int c = s.indexOf(',');
  IPAddress d1, d2((uint32_t)0);
  if (!d1.fromString(c < 0 ? s : s.substring(0, c))) return;
  if (c >= 0) d2.fromString(s.substring(c + 1));
  WiFi.setDNS(d1, d2);
  customDnsSet = true;
}

inline bool wifiConnect(String& err) {
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == settings.wifiSsid) return true;
  if (!settings.wifiSsid.length()) { err = "Не выбрана сеть Wi-Fi"; return false; }
  mark("Wi-Fi: подключение");
  WiFi.mode(WIFI_STA);
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
  uint32_t t0 = millis();
  int dots = 0;
  input.waitRelease();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 20000) { err = "Не удалось подключиться к «" + settings.wifiSsid + "». Проверьте пароль и что сеть рядом."; WiFi.disconnect(true); return false; }
    input.update();
    if (input.pressed(B_B) || input.pressed(B_MENU)) { err = "Подключение отменено"; WiFi.disconnect(true); return false; }
    String d = "Подключение к Wi-Fi";
    for (int i = 0; i < dots % 4; i++) d += ".";
    toast(d);
    dots++;
    delay(400);
  }
  routerDns1 = WiFi.dnsIP(0);
  routerDns2 = WiFi.dnsIP(1);
  customDnsSet = false;
  applyDns();
  return true;
}

inline void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Сканирование и выбор сети, затем ввод пароля
inline void wifiPick() {
  toast("Поиск сетей Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  int n = WiFi.scanNetworks();
  if (n <= 0) { WiFi.mode(WIFI_OFF); messageBox("Wi-Fi", "Сети не найдены"); return; }
  struct Net { String ssid; int rssi; bool open; };
  std::vector<Net> nets;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (auto& x : nets) if (x.ssid == s) { dup = true; if (WiFi.RSSI(i) > x.rssi) x.rssi = WiFi.RSSI(i); }
    if (!dup) nets.push_back({s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  ListView lv;
  input.waitRelease();
  bool dirty = true;
  while (true) {
    if (dirty) {
      canvas.fillScreen(C_BG);
      drawHeader("Выберите сеть Wi-Fi");
      lv.draw(nets.size(), [&](int i, int x, int y, int w, bool s) {
        canvas.setFont(FONT);
        canvas.setTextDatum(middle_left);
        canvas.setTextColor(s ? C_SELTEXT : C_TEXT);
        canvas.drawString(fitText(nets[i].ssid, w - 70), x + 2, y + lv.rowH / 2);
        // уровень сигнала: 1-4 палочки
        int bars = nets[i].rssi > -55 ? 4 : nets[i].rssi > -67 ? 3 : nets[i].rssi > -78 ? 2 : 1;
        for (int b = 0; b < 4; b++) {
          int bh = 3 + b * 3;
          canvas.fillRect(x + w - 26 + b * 5, y + 16 - bh, 3, bh, b < bars ? (s ? C_SELTEXT : C_TEXT) : C_DIM);
        }
        canvas.setFont(FONT_S);
        canvas.setTextDatum(middle_right);
        canvas.setTextColor(s ? C_SELTEXT : C_DIM);
        canvas.drawString(nets[i].open ? "откр." : "", x + w - 32, y + lv.rowH / 2);
      });
      drawFooter("A: выбрать  B: назад");
      present();
      dirty = false;
    }
    input.update();
    if (!input.any()) { delay(10); continue; }
    dirty = true;
    if (lv.handle(nets.size())) continue;
    if (input.pressed(B_B) || input.pressed(B_MENU)) break;
    if (input.pressed(B_A)) {
      Net& nt = nets[lv.sel];
      String pass = (nt.ssid == settings.wifiSsid) ? settings.wifiPass : "";
      if (!nt.open) {
        if (!textInput("Пароль для «" + nt.ssid + "»", pass, 0)) continue;
      } else pass = "";
      settings.wifiSsid = nt.ssid;
      settings.wifiPass = pass;
      if (!settings.save() || !settings.writeIni()) messageBox("Настройки", "Не удалось записать настройки на SD-карту.");
      break;
    }
  }
  WiFi.mode(WIFI_OFF);
  input.waitRelease();
}

// ---------------------------------------------------------------------------
// Запрос к Gemini
// sig/model - «подпись мыслей» ответа модели Gemini 3 и имя модели, которая ответила.
// Подпись возвращается серверу вместе с историей: без неё многоходовой диалог у Gemini 3 работает хуже.
struct GemMsg { bool user; String text; String sig; String model; };

#define OPENROUTER_URL "https://openrouter.ai/api/v1/chat/completions"
inline bool isOR() { return settings.aiProvider == 1; }
inline bool isAPI() { return settings.aiProvider == 2; }
inline bool isOAI() { return settings.aiProvider != 0; }   // формат запросов OpenAI
// имя сервиса по адресу: https://api.unorouter.com/v1 -> UnoRouter
inline String apiHost() {
  String u = settings.apiUrl;
  int p = u.indexOf("://");
  if (p >= 0) u = u.substring(p + 3);
  int sl = u.indexOf('/');
  if (sl >= 0) u = u.substring(0, sl);
  if (u.startsWith("api.")) u = u.substring(4);
  return u;
}
inline String aiName() {
  if (isOR()) return "OpenRouter";
  if (isAPI()) { String h = apiHost(); return h.indexOf("unorouter") >= 0 ? String("UnoRouter") : h; }
  return "Gemini";
}
inline const String& aiKey() { return isOR() ? settings.orKey : isAPI() ? settings.apiKey : settings.gemKey; }
inline const String& aiModel() { return isOR() ? settings.orModel : isAPI() ? settings.apiModel : settings.gemModel; }
inline const String& aiFallback() { return isOR() ? settings.orFallback : isAPI() ? settings.apiFallback : settings.gemFallback; }
inline String apiEndpoint(const char* what) {
  String u = settings.apiUrl;
  u.trim();
  if (!u.startsWith("http://") && !u.startsWith("https://")) u = "https://" + u;
  while (u.endsWith("/")) u = u.substring(0, u.length() - 1);
  if (u.endsWith("/chat/completions")) u = u.substring(0, u.length() - 17);
  return u + what;
}
// короткое имя модели для заголовка: google/gemma-4-31b-it:free -> gemma-4-31b-it
inline String shortModel(const String& m) {
  String s = m;
  int sl = s.lastIndexOf('/');
  if (sl >= 0) s = s.substring(sl + 1);
  if (s.endsWith(":free")) s = s.substring(0, s.length() - 5);
  if (m == "openrouter/free") s = "авто-выбор";
  return s;
}

inline String geminiBaseUrl() {
  String u = settings.gemProxy;
  u.trim();
  if (!u.length()) return GEMINI_DIRECT;
  if (!u.startsWith("http://") && !u.startsWith("https://")) u = "https://" + u;
  while (u.endsWith("/")) u = u.substring(0, u.length() - 1);
  return u;
}

inline String buildRequest(const std::vector<GemMsg>& msgs, bool withThinking, const String& model, bool withSig) {
  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] = GEMINI_SYSTEM;
  JsonArray contents = doc["contents"].to<JsonArray>();
  size_t from = msgs.size() > GEMINI_MAX_TURNS ? msgs.size() - GEMINI_MAX_TURNS : 0;
  if (from < msgs.size() && !msgs[from].user) from++;   // диалог должен начинаться с пользователя
  for (size_t i = from; i < msgs.size(); i++) {
    JsonObject c = contents.add<JsonObject>();
    c["role"] = msgs[i].user ? "user" : "model";
    c["parts"][0]["text"] = msgs[i].text;
    if (withSig && !msgs[i].user && msgs[i].sig.length() && msgs[i].model == model)
      c["parts"][0]["thoughtSignature"] = msgs[i].sig;
  }
  if (withThinking) doc["generationConfig"]["thinkingConfig"]["thinkingLevel"] = "low";
  String out;
  serializeJson(doc, out);
  return out;
}

// Запрос в формате OpenAI (OpenRouter)
inline String buildRequestOR(const std::vector<GemMsg>& msgs, const String& model) {
  JsonDocument doc;
  doc["model"] = model;
  JsonArray arr = doc["messages"].to<JsonArray>();
  JsonObject sys = arr.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = GEMINI_SYSTEM;
  size_t from = msgs.size() > GEMINI_MAX_TURNS ? msgs.size() - GEMINI_MAX_TURNS : 0;
  if (from < msgs.size() && !msgs[from].user) from++;
  for (size_t i = from; i < msgs.size(); i++) {
    JsonObject c = arr.add<JsonObject>();
    c["role"] = msgs[i].user ? "user" : "assistant";
    c["content"] = msgs[i].text;
  }
  doc["max_tokens"] = 2048;
  if (isOR()) {
    doc["reasoning"]["effort"] = "low";     // думающие модели - покороче
    doc["reasoning"]["exclude"] = true;     // рассуждения нам не нужны
  }
  String out;
  serializeJson(doc, out);
  return out;
}

inline bool parseReplyOR(const String& body, String& text, String& err, String* actual = nullptr) {
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) { err = ""; return false; }
  if (!doc["error"].isNull()) {
    err = doc["error"]["message"] | "";
    const char* raw = doc["error"]["metadata"]["raw"] | "";
    if (*raw && err.indexOf(raw) < 0) err += String(" / ") + raw;
    return false;
  }
  JsonVariant ch = doc["choices"][0];
  if (ch.isNull()) { err = "Пустой ответ"; return false; }
  if (!ch["error"].isNull()) { err = ch["error"]["message"] | "Ошибка модели"; return false; }
  if (actual) { const char* am = doc["model"] | ""; if (*am) *actual = am; }
  text = ch["message"]["content"] | "";
  text.trim();
  if (!text.length()) {
    const char* fr = ch["finish_reason"] | "";
    err = strlen(fr) ? String("Ответ не получен (") + fr + ")" : "Пустой ответ";
    return false;
  }
  return true;
}

// Разбор ответа. true = получен текст ответа
inline bool parseReply(const String& body, String& text, String& err, String* sig = nullptr) {
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) { err = ""; return false; }
  if (!doc["error"].isNull()) {
    err = doc["error"]["message"] | "";
    return false;
  }
  JsonVariant cand = doc["candidates"][0];
  if (cand.isNull()) {
    const char* block = doc["promptFeedback"]["blockReason"] | "";
    err = strlen(block) ? String("Запрос заблокирован фильтром (") + block + ")" : "Пустой ответ";
    return false;
  }
  text = "";
  JsonArray parts = cand["content"]["parts"];
  for (JsonVariant p : parts) {
    const char* s = p["thoughtSignature"] | "";
    if (sig && *s) *sig = s;
    if (p["thought"] | false) continue;
    const char* t = p["text"] | "";
    text += t;
  }
  if (!text.length()) {
    const char* fr = cand["finishReason"] | "";
    err = strlen(fr) ? String("Ответ не получен (") + fr + ")" : "Пустой ответ";
    return false;
  }
  return true;
}

// Подробности ошибки лимита (429): через сколько повторить, дневной ли лимит
struct GemErrInfo { int retryS = 0; bool perDay = false; bool zeroLimit = false; };
inline GemErrInfo parseErrInfo(const String& body) {
  GemErrInfo e;
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) return e;
  for (JsonVariant d : doc["error"]["details"].as<JsonArray>()) {
    const char* rd = d["retryDelay"] | "";
    if (*rd) e.retryS = atoi(rd) + 1;
    for (JsonVariant v : d["violations"].as<JsonArray>()) {
      const char* q = v["quotaId"] | "";
      if (strstr(q, "PerDay")) e.perDay = true;
      long lim = v["quotaValue"].is<const char*>() ? atol(v["quotaValue"] | "1") : (long)(v["quotaValue"] | 1L);
      if (lim == 0) e.zeroLimit = true;
    }
  }
  String m = doc["error"]["message"] | "";
  if (m.indexOf("limit: 0") >= 0) e.zeroLimit = true;
  if (m.indexOf("PerDay") >= 0 || m.indexOf("per-day") >= 0 || m.indexOf("per day") >= 0) e.perDay = true;
  return e;
}

inline String shortMsg(const String& m) {
  String s = m;
  s.replace("\n", " ");
  if (s.length() > 140) s = s.substring(0, 137) + "...";
  return s;
}

// Понятные сообщения для частых ошибок
inline String explainError(int code, const String& msg, const String& model, const GemErrInfo& info) {
  String m = msg;
  if (m.indexOf("location is not supported") >= 0)
    return "Google не обслуживает этот регион: запрос дошёл до Google с российского адреса. Укажите в настройках «Подключение» свой сервер (VPN) или прокси.";
  if (code == 400 && m.indexOf("API key") >= 0) return "Неверный ключ API. Проверьте ключ в настройках.";
  if (code == 401) return "Ключ API не принят (401). Проверьте ключ в настройках или crokos.ini.";
  if (code == 402) return "На счёте OpenRouter нет денег для этой модели (402). Выберите бесплатную модель - с окончанием :free.";
  if (m.indexOf("data policy") >= 0 || m.indexOf("privacy") >= 0)
    return "OpenRouter не пускает к бесплатным моделям из-за настроек приватности. На openrouter.ai/settings/privacy включите разрешения для бесплатных моделей (free endpoints).";
  if (code == 403) {
    if (m.indexOf("1009") >= 0 || m.indexOf("country") >= 0 || m.indexOf("region") >= 0 || m.indexOf("location") >= 0)
      return aiName() + " не пускает запросы из России (403): " + shortMsg(m) + (isOR() && model.indexOf(":free") < 0 && model != "openrouter/free" ? String("") : String(" Попробуйте другую модель или другой сервис."));
    if (m.indexOf("flagged") >= 0 || m.indexOf("moderation") >= 0) return "Вопрос отклонён фильтром модели (403). Переформулируйте или выберите другую модель.";
    return "Доступ запрещён (403). " + (m.length() ? shortMsg(m) : String("Проверьте ключ API."));
  }
  if (code == 404) return "модель «" + model + "» не найдена (404). Проверьте название модели." + (m.length() ? " " + shortMsg(m) : String(""));
  if (code == 429) {
    if (info.zeroLimit) return "модель недоступна на бесплатном тарифе (429).";
    if (info.perDay && isOR()) return "дневной лимит бесплатных запросов исчерпан (429). Он сбрасывается в 03:00 по Москве.";
    if (info.perDay && !isOAI()) return "дневной лимит запросов исчерпан (429). Он сбрасывается около 10:00 по Москве.";
    if (info.perDay || m.indexOf("daily") >= 0) return "лимит запросов на сегодня исчерпан (429).";
    if (isOAI() && m.indexOf("upstream") >= 0) return "модель сейчас перегружена у поставщика (429).";
    if (isAPI()) return "слишком много запросов (429). Подождите немного." + (m.length() ? " " + shortMsg(m) : String(""));
    return "лимит запросов в минуту (429). Подождите минуту.";
  }
  if (code >= 500) return "сервер перегружен (" + String(code) + "), повторы не помогли." + (m.length() ? " " + shortMsg(m) : String(""));
  return "ошибка " + String(code) + (m.length() ? ": " + shortMsg(m) : String(""));
}

// Текст последней ошибки TLS (код mbedTLS) - для сообщения пользователю и лога
static String lastTlsErr;
inline void noteTlsErr(NetworkClientSecure& c) {
  char buf[96] = {0};
  int e = c.lastError(buf, sizeof(buf));
  lastTlsErr = e ? String("TLS -0x") + String(-e, HEX) + " " + buf : String("");
  if (e) Serial.printf("[AI] %s\n", lastTlsErr.c_str());
}

// Один HTTPS-запрос. Возвращает HTTP-код (<=0 - нет связи), тело ответа в resp
// HTTPS POST тела из памяти (текст или большой буфер в PSRAM).
// geminiKey: заголовок x-goog-api-key с ключом Gemini, иначе Bearer ключом выбранного сервиса.
inline int aiPostRaw(const String& url, const uint8_t* body, size_t len, bool geminiKey, String& resp) {
  mark("Gemini: сборка запроса");
  Serial.printf("[Gemini] POST %s (%u B), heap %u, psram %u, stack free %u\n", url.c_str(), (unsigned)len,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
  // Порядок важен: client объявлен раньше http, значит http уничтожится первым.
  // (Деструктор HTTPClient обращается к client - раньше client удалялся первым, отсюда panic.)
  AiClient client;
  client.setCACert(ROOT_CAS);
  int code;
  {
    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(60000);   // макс. 65 с (uint16_t)
    http.setConnectTimeout(15000);
    if (!http.begin(client, url)) return -1000;
    http.addHeader("Content-Type", "application/json");
    if (!geminiKey) {
      http.addHeader("Authorization", "Bearer " + aiKey());
      if (isOR()) {
        http.addHeader("HTTP-Referer", "https://crokpocket.local");
        http.addHeader("X-Title", "CrokPocket");
      }
    } else {
      http.addHeader("x-goog-api-key", settings.gemKey);
    }
    mark("Gemini: HTTPS-запрос");
    code = http.POST((uint8_t*)body, len);
    if (code <= 0) noteTlsErr(client); else lastTlsErr = "";
    mark("Gemini: чтение ответа");
    resp = code > 0 ? http.getString() : "";
    http.end();
  }
  client.stop();
  Serial.printf("[Gemini] HTTP %d, %u B, heap %u, stack free %u\n", code, (unsigned)resp.length(), (unsigned)ESP.getFreeHeap(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
  if (code != 200) Serial.printf("[Gemini] %s\n", resp.c_str());
  mark("Gemini: разбор ответа");
  return code;
}

inline int geminiPostOnce(const String& url, const String& body, String& resp) {
  return aiPostRaw(url, (const uint8_t*)body.c_str(), body.length(), !isOAI(), resp);
}

// Запрос с повторами: соединение (TCP или TLS) иногда срывается на первой попытке, особенно
// через свой сервер - повтор через полсекунды почти всегда проходит. Повторяем только ошибку
// соединения: запрос тогда точно не дошёл до сервиса, лишнего расхода лимита нет.
inline int geminiPost(const String& url, const String& body, String& resp) {
  int code = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (attempt) { Serial.printf("[AI] повтор соединения %d\n", attempt); delay(400 * attempt); }
    code = geminiPostOnce(url, body, resp);
    if (code != HTTPC_ERROR_CONNECTION_REFUSED) break;
  }
  return code;
}

// Список моделей сервиса (OpenAI-формат: GET /models). Бесплатные (":free") - первыми.
inline bool fetchModels(std::vector<String>& out, String& err) {
  out.clear();
  String url = isOR() ? String("https://openrouter.ai/api/v1/models") : apiEndpoint("/models");
  String resp;
  int code = 0;
  for (int attempt = 0; attempt < 4; attempt++) {
    if (attempt) delay(400 * attempt);
    AiClient client;
    client.setCACert(ROOT_CAS);
    {
      HTTPClient http;
      http.setReuse(false);
      http.setTimeout(30000);
      http.setConnectTimeout(15000);
      if (!http.begin(client, url)) { err = "Неверный адрес: " + url; return false; }
      http.addHeader("Authorization", "Bearer " + aiKey());
      mark("Модели: запрос");
      code = http.GET();
      if (code <= 0) noteTlsErr(client);
      resp = code > 0 ? http.getString() : "";
      http.end();
    }
    client.stop();
    if (code != HTTPC_ERROR_CONNECTION_REFUSED) break;
  }
  if (code <= 0) { err = "Нет связи с сервером (" + HTTPClient::errorToString(code) + ")." + (lastTlsErr.length() ? " " + lastTlsErr : String("")); return false; }
  if (code != 200) { String t, e; parseReplyOR(resp, t, e); err = "Ошибка " + String(code) + (e.length() ? ": " + shortMsg(e) : String("")); return false; }
  JsonDocument filter;
  filter["data"][0]["id"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, resp.c_str(), resp.length(), DeserializationOption::Filter(filter))) { err = "Непонятный ответ сервера"; return false; }
  resp = String();
  std::vector<String> paid;
  for (JsonVariant m : doc["data"].as<JsonArray>()) {
    const char* id = m["id"] | "";
    if (!*id) continue;
    String s = id;
    if (s.endsWith(":free") || s == "openrouter/free") out.push_back(s);
    else paid.push_back(s);
  }
  std::sort(out.begin(), out.end());
  if (out.empty()) { std::sort(paid.begin(), paid.end()); out = paid; }
  if (out.size() > 60) out.resize(60);
  if (out.empty()) { err = "Сервер не вернул ни одной модели."; return false; }
  return true;
}

// Сервер перегружен / лимит - имеет смысл повторить или спросить другую модель
inline bool retryable(int code) { return code == 429 || code == 500 || code == 502 || code == 503 || code == 504; }

// Показ статуса во время повторов (задаётся экраном чата)
static void (*geminiStatusCb)(const String&) = nullptr;

// Ожидание с обратным отсчётом. false = пользователь нажал B (отмена)
inline bool geminiWait(int seconds, const String& what) {
  for (int s = seconds; s > 0; s--) {
    if (geminiStatusCb) geminiStatusCb(what + " через " + String(s) + " с (B - отмена)");
    for (int i = 0; i < 20; i++) {
      input.update();
      if (input.pressed(B_B)) { input.waitRelease(); return false; }
      delay(50);
    }
  }
  if (geminiStatusCb) geminiStatusCb("Повторный запрос...");
  return true;
}

#define GEM_CANCELLED -2000
static String lastModel;   // какая модель реально ответила (OpenRouter авто-выбор)

inline bool geminiAskModel(const String& model, const std::vector<GemMsg>& msgs, String& reply, String& sig, String& err, int& lastCode) {
  String url = isOR() ? String(OPENROUTER_URL) : isAPI() ? apiEndpoint("/chat/completions") : geminiBaseUrl() + "/v1beta/models/" + model + ":generateContent";
  bool thinking = true, withSig = true, waited429 = false;
  int tries5xx = 0;
  const int MAX_5XX = 3;
  for (int guard = 0; guard < 8; guard++) {
    String resp;
    int code = geminiPost(url, isOAI() ? buildRequestOR(msgs, model) : buildRequest(msgs, thinking, model, withSig), resp);
    lastCode = code;
    if (code == -1000) { err = "Неверный адрес: " + url; return false; }
    if (code <= 0) {
      err = "Нет связи с сервером (" + HTTPClient::errorToString(code) + ")." + (lastTlsErr.length() ? " " + lastTlsErr + "." : String("")) + (settings.gemRelay.length() ? String(" Проверьте, что сервер ") + settings.gemRelay + " включён и порт открыт." : String(" Проверьте интернет и «Подключение» в настройках."));
      return false;
    }
    String perr;
    sig = "";
    String actual;
    bool okReply = isOAI() ? parseReplyOR(resp, reply, perr, &actual) : parseReply(resp, reply, perr, &sig);
    if (code == 200 && okReply) { if (actual.length()) lastModel = actual; return true; }
    if (code == 200) {
      // OpenRouter иногда присылает ошибку поставщика с кодом 200
      JsonDocument d;
      if (isOAI() && !deserializeJson(d, resp.c_str(), resp.length()) && !d["error"].isNull()) code = d["error"]["code"] | 502;
      else { err = perr.length() ? perr : String("Непонятный ответ сервера"); return false; }
      lastCode = code;
    }
    // модель не принимает thinkingLevel или подпись - повторяем без них
    if (code == 400 && thinking && perr.indexOf("hinking") >= 0) { thinking = false; continue; }
    if (code == 400 && withSig && perr.indexOf("ignature") >= 0) { withSig = false; continue; }
    // ответ не JSON (например, страница блокировки Cloudflare) - показываем кусок текста
    if (!perr.length() && resp.length()) {
      String s;
      bool tag = false;
      for (size_t i = 0; i < resp.length() && s.length() < 400; i++) {
        char c = resp[i];
        if (c == '<') { tag = true; continue; }
        if (c == '>') { tag = false; s += ' '; continue; }
        if (!tag) s += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
      }
      while (s.indexOf("  ") >= 0) s.replace("  ", " ");
      s.trim();
      perr = s;
    }
    GemErrInfo info = parseErrInfo(resp);
    // OpenRouter: модель перегружена у поставщика - сразу к запасной, ждать бессмысленно
    if (isOAI() && code == 429 && perr.indexOf("upstream") >= 0) { err = explainError(code, perr, model, info); return false; }
    if (code == 429 && !waited429 && !info.perDay && !info.zeroLimit) {
      // лимит в минуту: ждём столько, сколько просит Google (обычно до минуты)
      waited429 = true;
      int w = info.retryS > 0 ? info.retryS : 30;
      if (w <= 65) {
        if (!geminiWait(w, "Лимит в минуту, повтор")) { err = "Отменено."; lastCode = GEM_CANCELLED; return false; }
        continue;
      }
    }
    if (code >= 500 && code != 501 && ++tries5xx < MAX_5XX) {
      if (!geminiWait(tries5xx * 4, "Сервер занят (" + String(code) + "), повтор " + String(tries5xx + 1) + "/" + String(MAX_5XX))) {
        err = "Отменено."; lastCode = GEM_CANCELLED; return false;
      }
      continue;
    }
    err = explainError(code, perr, model, info);
    return false;
  }
  err = "Не удалось получить ответ";
  return false;
}

// Ответ: reply, модель, которая ответила (usedModel), её подпись (sig)
inline bool geminiAsk(const std::vector<GemMsg>& msgs, String& reply, String& sig, String& usedModel, String& err) {
  applyDns();
  int code = 0;
  String err1;
  const String main = aiModel(), fb = aiFallback();
  usedModel = main;
  lastModel = "";
  if (geminiAskModel(main, msgs, reply, sig, err1, code)) { if (lastModel.length()) usedModel = lastModel; return true; }
  // у OpenRouter дневной лимит общий для всех бесплатных моделей - запасная не поможет
  if (isOR() && code == 429 && err1.indexOf("дневной") >= 0) { err = "OpenRouter: " + err1; return false; }
  // запасная модель: при перегрузке/лимите, а у OpenRouter ещё и если модель пропала из списка (404)
  bool tryFb = (retryable(code) || (isOAI() && code == 404 && err1.indexOf("data policy") < 0 && err1.indexOf("приватност") < 0)) && fb.length() && fb != main;
  if (!tryFb) { err = (code == 429 || code >= 500 || code == 404) ? shortModel(main) + ": " + err1 : err1; return false; }
  if (geminiStatusCb) geminiStatusCb("Спрашиваю запасную модель " + shortModel(fb) + "...");
  String err2;
  usedModel = fb;
  if (geminiAskModel(fb, msgs, reply, sig, err2, code)) {
    if (lastModel.length()) usedModel = lastModel;
    return true;
  }
  if (code == GEM_CANCELLED) { err = err2; return false; }
  err = "Не ответила ни одна модель.\n" + shortModel(main) + ": " + err1 + "\n" + shortModel(fb) + ": " + err2;
  return false;
}

// ---------------------------------------------------------------------------
// История диалога на SD
struct ChatLog {
  String path;
  void append(const String& who, const String& text) {
    if (!sdOk) return;
    if (!path.length()) {
      SD.mkdir(GEMINI_HISTORY_DIR);
      for (int i = 1; i < 1000; i++) {
        char b[48];
        snprintf(b, sizeof b, GEMINI_HISTORY_DIR "/chat_%03d.txt", i);
        if (!SD.exists(b)) { path = b; break; }
      }
      if (!path.length()) return;
    }
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    String s = who + ":\n" + text + "\n\n";
    f.write((const uint8_t*)s.c_str(), s.length());
    f.close();
  }
};

// ---------------------------------------------------------------------------
// Настройки Gemini
inline String maskKey(const String& k) {
  if (!k.length()) return "не задан";
  if (k.length() <= 8) return "••••";
  return k.substring(0, 4) + "…" + k.substring(k.length() - 4);
}

inline void saveOrWarn() {
  if (!settings.save()) { messageBox("Настройки", sdOk ? "Не удалось записать настройки на SD-карту." : "Нет SD-карты: настройки действуют до выключения."); return; }
  if (!settings.writeIni()) messageBox("Настройки", "Настройки сохранены, но crokos.ini обновить не удалось.");
}

inline void geminiSetup() {
  int sel = 0;
  input.waitRelease();
  bool dirty = true;
  while (true) {
    const int N = 7;
    const int ROW = 28;
    if (dirty) {
      canvas.fillScreen(C_BG);
      drawHeader("Нейросеть: настройки");
      const char* names[N] = {"Нейросеть", "Сеть Wi-Fi", "Пароль Wi-Fi", "Ключ API", "Модель", "Подключение", isAPI() ? "Адрес API" : "Адрес прокси"};
      String vals[N] = {
        isOR() ? String("OpenRouter (openrouter.ai)") : isAPI() ? aiName() + String(" (свой сервис)") : String("Gemini (Google)"),
        settings.wifiSsid.length() ? settings.wifiSsid : "не выбрана",
        settings.wifiPass.length() ? "••••••" : "нет",
        maskKey(aiKey()),
        shortModel(aiModel()) + (aiFallback().length() ? "  (запасная: " + shortModel(aiFallback()) + ")" : String("")),
        settings.gemRelay.length() ? "через свой сервер " + settings.gemRelay : settings.gemDns.length() ? "напрямую, DNS " + settings.gemDns : String("напрямую"),
        isAPI() ? settings.apiUrl : isOR() ? String("только для Gemini") : settings.gemProxy.length() ? settings.gemProxy : String("не используется (напрямую)"),
      };
      for (int i = 0; i < N; i++) {
        int y = HEADER_H + 3 + i * ROW;
        bool s = i == sel;
        if (s) canvas.fillRoundRect(4, y, SCR_W - 8, ROW - 1, 4, C_SEL);
        canvas.setFont(FONT);
        canvas.setTextDatum(top_left);
        canvas.setTextColor(s ? C_SELTEXT : C_TEXT);
        canvas.drawString(names[i], 12, y);
        canvas.setFont(FONT_S);
        canvas.setTextColor(s ? C_SELTEXT : C_ACCENT_TEXT);
        canvas.drawString(fitText(vals[i], SCR_W - 28), 12, y + 15);
      }
      drawFooter("A: изменить  B: назад");
      present();
      dirty = false;
    }
    input.update();
    if (!input.any()) { delay(10); continue; }
    dirty = true;
    if (input.pressed(B_B) || input.pressed(B_MENU)) break;
    if (input.pressed(B_UP)) sel = (sel + N - 1) % N;
    if (input.pressed(B_DOWN)) sel = (sel + 1) % N;
    if (input.pressed(B_A)) {
      String v;
      switch (sel) {
        case 0: {
          int r = popupMenu("Нейросеть", {"Gemini (Google)", "OpenRouter (бесплатные модели)", "Свой сервис (UnoRouter и др.)"});
          if (r >= 0 && r != settings.aiProvider) { settings.aiProvider = r; saveOrWarn(); }
          break;
        }
        case 1: wifiPick(); break;
        case 2: v = settings.wifiPass; if (textInput("Пароль Wi-Fi", v, 0)) { settings.wifiPass = v; saveOrWarn(); } break;
        case 3: {
          const char* kf = isOR() ? "/openrouter_key.txt" : isAPI() ? "/api_key.txt" : "/gemini_key.txt";
          int r = popupMenu("Ключ " + aiName(), {"Из файла на SD-карте", "Ввести с клавиатуры"});
          String& key = isOR() ? settings.orKey : isAPI() ? settings.apiKey : settings.gemKey;
          if (r == 0) {
            // файл закидывается через Wi-Fi загрузчик в корень карты; после чтения удаляется
            File f = SD.open(kf, FILE_READ);
            if (!f) { messageBox("Ключ API", String("Положите файл ") + (kf + 1) + " с ключом в корень SD-карты (через «Wi-Fi» или картридер)."); break; }
            String k;
            int c;
            while ((c = f.read()) >= 0 && c != '\n' && k.length() < 512) if (c != '\r' && c != ' ' && c != '\t' && c != 0xEF && c != 0xBB && c != 0xBF) k += (char)c;
            f.close();
            if (k.length() < 20) { messageBox("Ключ API", "В файле нет ключа или он слишком короткий."); break; }
            key = k;
            saveOrWarn();
            SD.remove(kf);
            messageBox("Ключ API", "Ключ загружен (" + maskKey(k) + "). Файл удалён с карты.");
          } else if (r == 1) {
            v = key;
            if (textInput("Ключ API", v, 0)) { v.trim(); key = v; saveOrWarn(); }
          }
          break;
        }
        case 4: {
          String& m = isOR() ? settings.orModel : isAPI() ? settings.apiModel : settings.gemModel;
          String& fbm = isOR() ? settings.orFallback : isAPI() ? settings.apiFallback : settings.gemFallback;
          int which = popupMenu("Модель", {"Основная: " + shortModel(m), "Запасная: " + (fbm.length() ? shortModel(fbm) : String("нет"))});
          if (which < 0) break;
          String& target = which == 0 ? m : fbm;
          std::vector<String> opts;
          if (isOR()) opts = {"google/gemma-4-31b-it:free", "qwen/qwen3.8-27b:free", "openrouter/free"};
          else if (isAPI()) opts = {};
          else opts = {"gemini-3.8-flash", "gemini-3.5-flash", "gemini-3.5-flash-lite"};
          if (isOAI()) opts.push_back("Список моделей с сервера");
          if (which == 1) opts.push_back("Без запасной модели");
          opts.push_back("Ввести вручную");
          int r = popupMenu(which == 0 ? "Основная модель" : "Запасная модель", opts);
          if (r < 0) break;
          if (opts[r] == "Без запасной модели") { target = ""; saveOrWarn(); break; }
          if (opts[r] == "Список моделей с сервера") {
            String e;
            if (!aiKey().length()) { messageBox("Модели", "Сначала укажите ключ API."); break; }
            if (!wifiConnect(e)) { messageBox("Wi-Fi", e); break; }
            applyDns();
            toast("Загружаю список моделей...");
            std::vector<String> list;
            bool ok = fetchModels(list, e);
            if (!ok) { messageBox("Модели", e); break; }
            int k = popupMenu("Модели (" + String((int)list.size()) + ")", list);
            if (k >= 0) { target = list[k]; saveOrWarn(); }
            break;
          }
          if (opts[r] == "Ввести вручную") {
            String& m = target;
            v = m;
            if (!textInput("Модель", v, 0)) break;
            v.trim();
            if (!v.length()) break;
            m = v;
          } else target = opts[r];
          saveOrWarn();
          break;
        }
        case 5: {
          int r = popupMenu("Подключение", {"Напрямую", "Через свой сервер (VPN)", "Напрямую, свой DNS-сервер"});
          if (r == 0) { settings.gemDns = ""; settings.gemRelay = ""; }
          else if (r == 1) {
            v = settings.gemRelay;
            if (!textInput("Адрес сервера:порт (например 1.2.3.4:8443)", v, 0)) break;
            v.trim(); v.replace(" ", "");
            if (!v.length()) { settings.gemRelay = ""; saveOrWarn(); break; }
            if (v.indexOf(':') < 0) v += ":" + String(RELAY_DEFAULT_PORT);
            settings.gemRelay = v; settings.gemDns = ""; settings.gemProxy = "";
          }
          else if (r == 2) {
            v = settings.gemDns;
            if (!textInput("IP DNS (через запятую - два)", v, 0)) break;
            v.trim(); v.replace(" ", "");
            IPAddress t;
            int c = v.indexOf(',');
            if (v.length() && !t.fromString(c < 0 ? v : v.substring(0, c))) { messageBox("DNS", "Неверный IP-адрес"); break; }
            settings.gemDns = v;
            settings.gemRelay = "";
          }
          if (r >= 0) saveOrWarn();
          break;
        }
        case 6:
          if (isAPI()) {
            v = settings.apiUrl;
            if (textInput("Адрес API (например https://api.unorouter.com/v1)", v, 0)) { v.trim(); if (v.length()) { settings.apiUrl = v; saveOrWarn(); } }
            break;
          }
          if (isOR()) { messageBox("Прокси", "Прокси используется только для Gemini."); break; }
          v = settings.gemProxy.length() ? settings.gemProxy : "https://"; if (textInput("Адрес прокси (пусто = напрямую)", v, 0)) { v.trim(); if (v == "https://" || v == "http://") v = ""; settings.gemProxy = v; if (v.length()) settings.gemRelay = ""; saveOrWarn(); } break;
      }
    }
  }
  input.waitRelease();
}

// ---------------------------------------------------------------------------
// Экран чата
inline void geminiApp() {
  storageReady();
  if (settings.importIni()) saveOrWarn();   // вдруг crokos.ini обновили через Wi-Fi
  if (!aiKey().length() || !settings.wifiSsid.length()) {
    messageBox(aiName(), isOAI() ? "Сначала укажите сеть Wi-Fi и ключ " + aiName() + "."
                                : String("Сначала укажите сеть Wi-Fi и ключ API. Из России Gemini работает через свой сервер (VPN) или прокси: настройки, пункт «Подключение»."));
    geminiSetup();
    if (!aiKey().length() || !settings.wifiSsid.length()) return;
  }
  std::vector<GemMsg> msgs;
  ChatLog log;
  struct Line { String text; uint16_t color; bool label; };
  std::vector<Line> lines;
  std::vector<int> msgStart;   // первая строка каждого сообщения
  int top = 0;
  const int LINE_H = 18;
  const int AREA_Y = HEADER_H + 2;
  const int ROWS = (SCR_H - HEADER_H - FOOTER_H - 4) / LINE_H;
  String status = "";

  auto relayout = [&]() {
    lines.clear();
    msgStart.clear();
    canvas.setFont(FONT);
    for (auto& m : msgs) {
      msgStart.push_back(lines.size());
      lines.push_back({m.user ? String("Вы") : (m.model.length() ? shortModel(m.model) : aiName()), m.user ? C_ACCENT_TEXT : C_LINK, true});
      std::vector<String> w;
      wrapInto(m.text, SCR_W - 16, w);
      for (auto& s : w) lines.push_back({s, C_TEXT, false});
      lines.push_back({"", C_TEXT, false});
    }
  };
  auto maxTop = [&]() { return max(0, (int)lines.size() - ROWS); };

  auto draw = [&]() {
    canvas.fillScreen(C_BG);
    drawHeader(aiName() + "  ·  " + shortModel(aiModel()));
    canvas.setFont(FONT);
    canvas.setTextDatum(top_left);
    if (lines.empty()) {
      canvas.setTextColor(C_DIM);
      canvas.setTextDatum(middle_center);
      canvas.drawString("Нажмите A и задайте вопрос", SCR_W / 2, SCR_H / 2 - 10);
      canvas.setFont(FONT_S);
      canvas.drawString("START: настройки   SELECT: новый чат", SCR_W / 2, SCR_H / 2 + 12);
    }
    for (int r = 0; r < ROWS; r++) {
      int i = top + r;
      if (i >= (int)lines.size()) break;
      int y = AREA_Y + r * LINE_H;
      if (lines[i].label) {
        canvas.setFont(FONT_S);
        canvas.setTextColor(lines[i].color);
        canvas.drawString(lines[i].text, 6, y + 3);
        canvas.drawFastHLine(6 + canvas.textWidth(lines[i].text) + 6, y + 10, SCR_W - 30 - canvas.textWidth(lines[i].text), C_HEADER);
        canvas.setFont(FONT);
      } else {
        canvas.setTextColor(lines[i].color);
        canvas.drawString(lines[i].text, 8, y);
      }
    }
    if ((int)lines.size() > ROWS) {
      int h = ROWS * LINE_H;
      int th = max(8, h * ROWS / (int)lines.size());
      int ty = AREA_Y + (h - th) * top / max(1, maxTop());
      canvas.fillRect(SCR_W - 4, AREA_Y, 3, h, C_HEADER);
      canvas.fillRect(SCR_W - 4, ty, 3, th, C_DIM);
    }
    if (status.length()) {
      canvas.fillRect(0, SCR_H - FOOTER_H - 20, SCR_W, 20, C_PANEL);
      canvas.setFont(FONT_S);
      canvas.setTextDatum(middle_center);
      canvas.setTextColor(C_ACCENT_TEXT);
      canvas.drawString(status, SCR_W / 2, SCR_H - FOOTER_H - 10);
    }
    drawFooter(voiceHook() ? "A:вопрос OPTION(держать):голос SELECT:новый" : "A:вопрос ↑↓:листать SELECT:новый START:настр.");
    present();
  };

  input.waitRelease();
  bool dirty = true;
  while (true) {
    if (dirty) { draw(); dirty = false; }
    input.update();
    if (!input.any()) { delay(10); continue; }
    dirty = true;
    if (input.pressed(B_B) || input.pressed(B_MENU)) {
      if (msgs.empty() || confirmBox("Выход", "Закрыть чат? Диалог сохранён в папке /Gemini.", true)) break;
      continue;
    }
    if (input.pressed(B_UP)) top = max(0, top - 1);
    if (input.pressed(B_DOWN)) top = min(maxTop(), top + 1);
    if (input.pressed(B_LEFT)) top = max(0, top - (ROWS - 1));
    if (input.pressed(B_RIGHT)) top = min(maxTop(), top + (ROWS - 1));
    if (input.pressed(B_START)) { geminiSetup(); continue; }
    if (input.pressed(B_SELECT)) {
      if (!msgs.empty() && confirmBox("Новый чат", "Начать новый диалог? Текущий уже сохранён.", true)) {
        msgs.clear(); lines.clear(); msgStart.clear(); top = 0; log = ChatLog();
      }
      continue;
    }
    bool voice = input.pressed(B_OPTION) && voiceHook();
    if (input.pressed(B_A) || voice) {
      String q;
      // голос: держите OPTION и говорите; распознанный текст можно поправить перед отправкой
      if (voice && !voiceHook()(q)) continue;
      if (!textInput(voice ? String("Проверьте вопрос (START - отправить)") : String("Вопрос для Gemini"), q, 1)) continue;
      q.trim();
      if (!q.length()) continue;
      msgs.push_back({true, q});
      log.append("Вы", q);   // сохраняем вопрос сразу - даже если дальше что-то пойдёт не так
      relayout();
      top = maxTop();
      status = "Подключение...";
      draw();
      String err;
      if (!wifiConnect(err)) {
        msgs.pop_back(); relayout(); top = maxTop(); status = "";
        messageBox("Wi-Fi", err);
        continue;
      }
      status = aiName() + " думает...";
      draw();
      geminiStatusCb = [](const String& s) { toast(s); };
      String reply, sig, usedModel;
      bool ok = geminiAsk(msgs, reply, sig, usedModel, err);
      status = "";
      if (!ok) {
        log.append("Ошибка", err);
        msgs.pop_back();
        relayout(); top = maxTop();
        draw();
        messageBox(aiName(), err);
        continue;
      }
      mark("Gemini: вывод ответа");
      reply = cleanReply(reply);
      msgs.push_back({false, reply, sig, usedModel});
      log.append(shortModel(usedModel), reply);
      relayout();
      // показываем начало ответа
      int start = msgStart.back();
      top = min(maxTop(), max(0, start - 1));
    }
  }
  wifiOff();
  mark("");
  input.waitRelease();
}
