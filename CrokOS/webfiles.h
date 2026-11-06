// ============================================================================
//  CrokOS - загрузка файлов по Wi-Fi: консоль раздаёт открытую сеть CrokPocket
// ============================================================================
#pragma once
#include "ui.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#ifndef CROKOS_VERSION
#define CROKOS_VERSION "2026.10.18"
#endif

static const char WEB_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CrokPocket</title>
<style>
body{font-family:sans-serif;background:#111;color:#ddd;margin:0}
header{background:#222;border-bottom:2px solid #f90;padding:10px 12px;font-size:18px}
main{padding:10px 12px}
#path{color:#f90;word-break:break-all;margin:6px 0}
.row{display:flex;align-items:center;gap:8px;padding:9px 6px;border-bottom:1px solid #2a2a2a}
.row a{color:#ddd;text-decoration:none;flex:1;word-break:break-all}
.dir a{color:#9cf}
.sz{color:#888;font-size:12px;white-space:nowrap}
button,.btn{background:#333;color:#eee;border:1px solid #555;border-radius:6px;padding:7px 10px;font-size:14px}
.del{background:#522;border-color:#844}
.bar{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0}
#prog{height:6px;background:#333;border-radius:3px;margin:6px 0;display:none}
#prog div{height:100%;width:0;background:#f90;border-radius:3px}
#msg{color:#8c8;min-height:18px}
</style></head><body>
<header>CrokPocket — файлы на SD</header>
<main>
<div id="path"></div>
<div class="bar">
<button onclick="up()">↑ Наверх</button>
<label class="btn">Загрузить файлы<input id="f" type="file" multiple style="display:none" onchange="upload()"></label>
<button onclick="mk()">Новая папка</button>
</div>
<div id="prog"><div></div></div><div id="msg"></div>
<div id="list"></div>
</main>
<script>
let dir='/';
const $=id=>document.getElementById(id);
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function join(a,b){return (a.endsWith('/')?a:a+'/')+b}
function hs(b){if(b<1024)return b+' Б';if(b<1048576)return (b/1024).toFixed(1)+' КБ';return (b/1048576).toFixed(1)+' МБ'}
async function load(){
 $('path').textContent=dir;
 const r=await fetch('/list?dir='+encodeURIComponent(dir));
 const j=await r.json();
 j.sort((a,b)=>(b.d-a.d)||a.n.localeCompare(b.n));
 $('list').innerHTML=j.map(e=>{const p=join(dir,e.n);
  return e.d?`<div class="row dir"><a href="#" onclick='cd(${JSON.stringify(p)});return false'>📁 ${esc(e.n)}</a><button class="del" onclick='del(${JSON.stringify(p)},1)'>✕</button></div>`
  :`<div class="row"><a href="/dl?path=${encodeURIComponent(p)}">${esc(e.n)}</a><span class="sz">${hs(e.s)}</span><button class="del" onclick='del(${JSON.stringify(p)},0)'>✕</button></div>`}).join('')||'<p style="color:#777">Папка пуста</p>';
}
function cd(p){dir=p;load()}
function up(){if(dir=='/')return;dir=dir.substring(0,dir.lastIndexOf('/'))||'/';load()}
async function del(p,d){if(!confirm((d?'Удалить папку со всем содержимым?\n':'Удалить файл?\n')+p))return;await fetch('/del?path='+encodeURIComponent(p));load()}
async function mk(){const n=prompt('Имя папки');if(!n)return;await fetch('/mkdir?path='+encodeURIComponent(join(dir,n)));load()}
function upload(){
 const files=[...$('f').files];if(!files.length)return;
 let i=0;const pr=$('prog'),bar=pr.firstChild;pr.style.display='block';
 const next=()=>{
  if(i>=files.length){pr.style.display='none';$('msg').textContent='Загружено файлов: '+files.length;$('f').value='';load();return}
  const fd=new FormData();fd.append('file',files[i],files[i].name);
  const x=new XMLHttpRequest();x.open('POST','/upload?dir='+encodeURIComponent(dir));
  x.upload.onprogress=e=>{bar.style.width=(e.loaded/e.total*100)+'%';$('msg').textContent=`${i+1}/${files.length}: ${files[i].name}`};
  x.onload=()=>{i++;next()};x.onerror=()=>{$('msg').textContent='Ошибка загрузки';pr.style.display='none'};
  x.send(fd);
 };next();
}
load();
</script></body></html>)HTML";

class WebFiles {
  WebServer server{80};
  DNSServer dns;
  File upFile;
  String lastAction = "";
  uint32_t lastActionAt = 0;
  uint32_t uploadedBytes = 0;
  // Загрузка идёт во временный «имя.part» и переименовывается только после успешного конца:
  // оборванная передача (сбой питания, выход из экрана, разрыв Wi-Fi) не оставляет «целый на вид» обрезок
  String upTmp, upFinal;
  bool upOk = false;
  String upErr;
  // Защита от сбоев записи на SD (длинные провода, просадки питания):
  // последние RING байт держим в PSRAM, каждые SYNC_EVERY байт сбрасываем файл на карту.
  // При ошибке записи карта переподключается на пониженной частоте, файл дописывается
  // с последней надёжной точки - передача не обрывается.
  static constexpr uint32_t RING = 256 * 1024, SYNC_EVERY = 64 * 1024;
  uint8_t* ring = nullptr;
  uint32_t lastSynced = 0;
  int sdFixes = 0;

  void ringPut(const uint8_t* b, size_t n, uint32_t pos) {
    if (!ring) return;
    while (n) {
      uint32_t i = pos % RING;
      size_t k = min((size_t)(RING - i), n);
      memcpy(ring + i, b, k);
      b += k; n -= k; pos += k;
    }
  }
  bool ringWrite(File& f, uint32_t from, uint32_t to) {
    while (from < to) {
      uint32_t i = from % RING;
      size_t k = min((size_t)(RING - i), (size_t)(to - from));
      if (f.write(ring + i, k) != k) return false;
      from += k;
    }
    return true;
  }
  // текущий кусок buf/n не записался: переподключаем карту и дописываем
  bool recoverWrite(const uint8_t* buf, size_t n) {
    uint32_t ringFill = ring ? min(uploadedBytes, RING) : 0;
    for (int a = 0; a < 4; a++) {
      if (upFile) upFile.close();
      delay(50 + a * 150);
      sdHz = SD_HZ_SAFE;
      if (!sdRemount()) continue;
      File f = SD.open(upTmp, "r+");
      if (!f) continue;
      uint32_t have = f.size();
      uint32_t resume = min(have, lastSynced);
      if (resume + ringFill < uploadedBytes) { f.close(); return false; }   // потерянный кусок уже не в памяти
      bool ok = f.seek(resume) && ringWrite(f, resume, uploadedBytes) && f.write(buf, n) == n;
      if (ok) { f.flush(); upFile = f; lastSynced = uploadedBytes + n; sdFixes++; return true; }
      f.close();
    }
    return false;
  }

  static String jsonEsc(const String& s) {
    String o;
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      if (c == '"' || c == '\\') { o += '\\'; o += c; }
      else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
      else o += c;
    }
    return o;
  }
  void note(const String& s) { lastAction = s; lastActionAt = millis(); }

  static bool removeRecursive(const String& path) {
    File f = SD.open(path);
    if (!f) return false;
    if (!f.isDirectory()) { f.close(); return SD.remove(path); }
    std::vector<String> children;
    while (File e = f.openNextFile()) { children.push_back(pathJoin(path, e.name())); e.close(); }
    f.close();
    for (auto& c : children) removeRecursive(c);
    return SD.rmdir(path);
  }

  void handleList() {
    String dir = server.hasArg("dir") ? server.arg("dir") : "/";
    if (hiddenPath(dir)) { server.send(403, "application/json", "[]"); return; }
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { server.send(404, "application/json", "[]"); return; }
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json; charset=utf-8", "");
    server.sendContent("[");
    bool first = true;
    while (File e = d.openNextFile()) {
      String n = e.name();
      if (n.startsWith(".") || n == "System Volume Information") { e.close(); continue; }
      String j = String(first ? "" : ",") + "{\"n\":\"" + jsonEsc(n) + "\",\"d\":" + (e.isDirectory() ? "1" : "0") + ",\"s\":" + String((uint32_t)e.size()) + "}";
      server.sendContent(j);
      first = false;
      e.close();
    }
    server.sendContent("]");
    server.sendContent("");
    d.close();
  }
  // скрытые файлы (настройки с паролями и ключами) через веб не отдаём и не удаляем
  static bool hiddenPath(const String& p) { return p.indexOf("/.") >= 0 || p.startsWith("."); }

  void handleDownload() {
    String p = server.arg("path");
    // crokos.ini содержит пароль Wi-Fi и ключ API: загрузить новый можно, скачать - нет (сеть открытая)
    if (hiddenPath(p) || strLower(pathName(p)) == "crokos.ini") { server.send(403, "text/plain", "Forbidden"); return; }
    File f = SD.open(p, FILE_READ);
    if (!f || f.isDirectory()) { server.send(404, "text/plain", "Not found"); return; }
    String name = pathName(p);
    // имя в UTF-8 для заголовка
    String enc;
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < name.length(); i++) {
      uint8_t c = name[i];
      if (isalnum(c) || c == '.' || c == '-' || c == '_') enc += (char)c;
      else { enc += '%'; enc += hex[c >> 4]; enc += hex[c & 15]; }
    }
    server.sendHeader("Content-Disposition", "attachment; filename*=UTF-8''" + enc);
    server.streamFile(f, "application/octet-stream");
    f.close();
    note("Скачан: " + name);
  }
  void handleDelete() {
    String p = server.arg("path");
    if (hiddenPath(p)) { server.send(403, "text/plain", "Forbidden"); return; }
    if (p.length() < 2) { server.send(400, "text/plain", "bad"); return; }
    bool ok = removeRecursive(p);
    note(String(ok ? "Удалено: " : "Не удалено: ") + pathName(p));
    server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "ERR");
  }
  // Для приложения CrokPocket: кто мы и сколько места на карте
  void handleInfo() {
    uint64_t total = SD.totalBytes(), used = SD.usedBytes();
    String j = String("{\"app\":\"CrokOS\",\"ver\":\"") + CROKOS_VERSION + "\",\"total\":" + String((double)total, 0) +
               ",\"free\":" + String((double)(total > used ? total - used : 0), 0) + "}";
    server.send(200, "application/json", j);
  }
  void handleRename() {
    String a = server.arg("from"), b = server.arg("to");
    if (hiddenPath(a) || hiddenPath(b) || a.length() < 2 || b.length() < 2) { server.send(403, "text/plain", "Forbidden"); return; }
    if (SD.exists(b)) { server.send(409, "text/plain", "exists"); return; }
    bool ok = SD.rename(a, b);
    note(String(ok ? "Переименовано: " : "Не переименовано: ") + pathName(b));
    server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "ERR");
  }
  void handleMkdir() {
    String p = server.arg("path");
    bool ok = SD.mkdir(p);
    note("Папка: " + pathName(p));
    server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "ERR");
  }
  void handleUpload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
      String dir = server.hasArg("dir") ? server.arg("dir") : "/";
      String name = up.filename;
      name.replace("/", "_"); name.replace("\\", "_");
      if (name.startsWith(".") || hiddenPath(dir)) name = "_" + name;   // не даём перезаписать скрытые файлы
      String p = pathJoin(dir, name);
      upFinal = p;
      upTmp = p + ".part";
      if (SD.exists(upTmp)) SD.remove(upTmp);
      upFile = SD.open(upTmp, FILE_WRITE);
      upOk = (bool)upFile;
      upErr = upOk ? "" : "Не удалось создать файл на SD-карте";
      uploadedBytes = 0;
      lastSynced = 0;
      note("Загрузка: " + name);
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (upFile && upOk) {
        size_t w = upFile.write(up.buf, up.currentSize);
        if (w != up.currentSize) {
          note("Сбой записи SD, восстанавливаю...");
          if (!recoverWrite(up.buf, up.currentSize)) {
            upOk = false;
            upErr = "Ошибка записи на SD-карту после " + humanSize(uploadedBytes) + ". Проверьте карту и провода к ней";
          }
        }
        ringPut(up.buf, up.currentSize, uploadedBytes);
      }
      uploadedBytes += up.currentSize;
      if (upFile && upOk && uploadedBytes - lastSynced >= SYNC_EVERY) { upFile.flush(); lastSynced = uploadedBytes; }
      if (millis() - lastActionAt > 300) note("Загрузка: " + up.filename + " " + humanSize(uploadedBytes));
    } else if (up.status == UPLOAD_FILE_END) {
      if (upFile) upFile.close();
      if (upOk) {
        File chk = SD.open(upTmp, FILE_READ);
        uint32_t got = chk ? (uint32_t)chk.size() : 0;
        if (chk) chk.close();
        if (got != up.totalSize) { upOk = false; upErr = "На карту записано " + humanSize(got) + " из " + humanSize(up.totalSize); }
      }
      if (upOk) {
        if (SD.exists(upFinal)) SD.remove(upFinal);
        if (!SD.rename(upTmp, upFinal)) { upOk = false; upErr = "Не удалось переименовать файл"; }
      }
      if (!upOk) SD.remove(upTmp);
      note(upOk ? "Получен: " + up.filename + " (" + humanSize(up.totalSize) + ")" : "Ошибка: " + upErr);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      if (upFile) upFile.close();
      if (upTmp.length()) SD.remove(upTmp);
      upOk = false;
      upErr = "Передача прервана";
      note("Загрузка прервана");
    }
  }

  void drawScreen() {
    canvas.fillScreen(C_BG);
    drawHeader("Wi-Fi загрузка файлов");
    canvas.setFont(FONT);
    canvas.setTextDatum(top_left);
    int y = HEADER_H + 10;
    canvas.setTextColor(C_DIM);   canvas.drawString("1. Подключите телефон к сети:", 10, y); y += 20;
    canvas.setTextColor(C_ACCENT_TEXT); canvas.setFont(FONT_L); canvas.drawString("CrokPocket", 26, y); y += 30;
    canvas.setFont(FONT);
    canvas.setTextColor(C_DIM);   canvas.drawString("   (без пароля)", 10, y); y += 24;
    canvas.drawString("2. Откройте в браузере:", 10, y); y += 20;
    canvas.setTextColor(C_ACCENT_TEXT); canvas.setFont(FONT_L); canvas.drawString("192.168.4.1", 26, y); y += 32;
    canvas.setFont(FONT);
    canvas.setTextColor(C_TEXT);
    canvas.drawString("Подключено устройств: " + String(WiFi.softAPgetStationNum()), 10, y); y += 22;
    if (sdFixes) { canvas.setTextColor(C_DIM); canvas.drawString("Сбоев SD исправлено: " + String(sdFixes), 10, y); y += 22; }
    canvas.setTextColor(C_OK);
    canvas.drawString(fitText(lastAction, SCR_W - 20), 10, y);
    drawFooter("B: выключить Wi-Fi и выйти");
    present();
  }

public:
  void run() {
    if (!sdMount()) { messageBox("Нет SD-карты", "Вставьте карту памяти и попробуйте снова."); return; }
    toast("Запуск Wi-Fi...");
    ring = (uint8_t*)ps_malloc(RING);
    sdFixes = 0;
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP("CrokPocket");   // открытая сеть
    dns.start(53, "*", WiFi.softAPIP());
    server.on("/", HTTP_GET, [this]() { server.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
    server.on("/list", HTTP_GET, [this]() { handleList(); });
    server.on("/dl", HTTP_GET, [this]() { handleDownload(); });
    server.on("/del", HTTP_GET, [this]() { handleDelete(); });
    server.on("/mkdir", HTTP_GET, [this]() { handleMkdir(); });
    server.on("/info", HTTP_GET, [this]() { handleInfo(); });
    server.on("/rename", HTTP_GET, [this]() { handleRename(); });
    server.on("/upload", HTTP_POST, [this]() {
      if (upOk) server.send(200, "text/plain", "OK " + String(uploadedBytes));
      else server.send(500, "text/plain; charset=utf-8", upErr.length() ? upErr : String("upload failed"));
    }, [this]() { handleUpload(); });
    server.onNotFound([this]() {   // перенаправление для captive portal
      server.sendHeader("Location", "http://192.168.4.1/", true);
      server.send(302, "text/plain", "");
    });
    server.begin();
    note("Ожидание подключения...");
    input.waitRelease();
    uint32_t lastDraw = 0;
    String shown;
    int shownSta = -1;
    while (true) {
      dns.processNextRequest();
      server.handleClient();
      input.update();
      if (input.pressed(B_B) || input.pressed(B_MENU)) break;
      int sta = WiFi.softAPgetStationNum();
      if (millis() - lastDraw > 500 && (shown != lastAction || sta != shownSta)) {
        drawScreen(); shown = lastAction; shownSta = sta; lastDraw = millis();
      }
      delay(2);
    }
    if (upFile) { upFile.close(); if (upTmp.length()) SD.remove(upTmp); }   // вышли посреди загрузки
    if (ring) { free(ring); ring = nullptr; }
    if (sdHz != SD_HZ_NORMAL) { sdHz = SD_HZ_NORMAL; sdRemount(); }   // для видео нужна обычная скорость
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    input.waitRelease();
  }
};

inline void wifiApp() {
  WebFiles* w = new WebFiles();
  w->run();
  delete w;
}
