// ============================================================================
//  CrokOS - TXT-редактор (UTF-8) с экранной клавиатурой
// ============================================================================
#pragma once
#include "ui.h"
#include <unordered_map>

#define EDIT_MAX_BYTES (512 * 1024)

class TextEditor {
  std::vector<uint32_t> t;          // текст в кодовых точках
  struct VL { uint32_t s, n; };     // визуальная строка: начало и длина
  std::vector<VL> vl;
  size_t cur = 0;                   // курсор (индекс символа)
  int top = 0;                      // первая видимая строка
  bool modified = false, crlf = false, bom = false;
  bool editMode = false;            // показана клавиатура
  bool cursorMode = false;          // крестовина двигает курсор, а не клавиатуру
  String path;
  Keyboard kb;
  int8_t wcache[0x500];
  std::unordered_map<uint32_t, uint8_t> wmap;
  static const int LINE_H = 18;
  static const int TEXT_X = 4;
  static const int TEXT_W = SCR_W - 12;

  int cpw(uint32_t c) {
    if (c == '\t') return cpw(' ') * 4;
    if (c < 0x20) return 0;
    if (c < 0x500) {
      if (wcache[c] < 0) { String s; utf8Append(s, c); wcache[c] = canvas.textWidth(s); }
      return wcache[c];
    }
    auto it = wmap.find(c);
    if (it != wmap.end()) return it->second;
    String s; utf8Append(s, c);
    uint8_t w = canvas.textWidth(s);
    wmap[c] = w;
    return w;
  }

  void wrap() {
    canvas.setFont(FONT);
    vl.clear();
    uint32_t N = t.size(), i = 0, ls = 0;
    int w = 0;
    int64_t lastSpace = -1;
    while (i < N) {
      uint32_t c = t[i];
      if (c == '\n') { vl.push_back({ls, i - ls}); i++; ls = i; w = 0; lastSpace = -1; continue; }
      int cw = cpw(c);
      if (w + cw > TEXT_W && i > ls) {
        if (lastSpace >= (int64_t)ls) { vl.push_back({ls, (uint32_t)(lastSpace + 1 - ls)}); ls = lastSpace + 1; }
        else { vl.push_back({ls, i - ls}); ls = i; }
        w = 0; lastSpace = -1;
        for (uint32_t k = ls; k < i; k++) { w += cpw(t[k]); if (t[k] == ' ') lastSpace = k; }
        continue;
      }
      if (c == ' ') lastSpace = i;
      w += cw;
      i++;
    }
    vl.push_back({ls, N - ls});
  }

  int lineOf(size_t pos) const {
    int lo = 0, hi = vl.size() - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (vl[mid].s <= pos) lo = mid; else hi = mid - 1; }
    return lo;
  }
  int xOf(size_t pos) {
    int l = lineOf(pos), x = 0;
    for (size_t k = vl[l].s; k < pos && k < vl[l].s + vl[l].n; k++) x += cpw(t[k]);
    return x;
  }
  size_t posAtX(int line, int x) {
    size_t p = vl[line].s, end = vl[line].s + vl[line].n;
    int acc = 0;
    while (p < end) {
      int w = cpw(t[p]);
      if (acc + w / 2 >= x) break;
      acc += w; p++;
    }
    // не ставить курсор после пробела переноса (он визуально в конце строки)
    if (p == end && line + 1 < (int)vl.size() && vl[line + 1].s == end && p > vl[line].s) p--;
    return p;
  }
  int textRows() const {
    int area = SCR_H - HEADER_H - (editMode ? Keyboard::H + 14 : FOOTER_H) - 4;
    return area / LINE_H;
  }
  void ensureVisible() {
    int l = lineOf(cur), r = textRows();
    if (l < top) top = l;
    if (l >= top + r) top = l - r + 1;
    if (top < 0) top = 0;
  }

  bool load() {
    File f = sdOpenRead(path);
    if (!f) { messageBox("Ошибка", "Не удалось открыть файл"); return false; }
    size_t sz = f.size();
    if (sz > EDIT_MAX_BYTES) { f.close(); messageBox("Слишком большой", "Файл больше 512 КБ, редактор его не откроет."); return false; }
    char* buf = (char*)ps_malloc(sz + 1);
    if (!buf) buf = (char*)malloc(sz + 1);
    if (!buf) { f.close(); messageBox("Ошибка", "Не хватает памяти"); return false; }
    size_t rd = f.read((uint8_t*)buf, sz);
    f.close();
    if (rd != sz) {   // сбой чтения SD - переподключаем карту и читаем заново
      sdRemount();
      File g = sdOpenRead(path);
      rd = g ? g.read((uint8_t*)buf, sz) : 0;
      if (g) g.close();
      if (rd != sz) { free(buf); messageBox("Ошибка", "Не удалось прочитать файл с SD-карты."); return false; }
    }
    size_t off = 0;
    if (rd >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF) { bom = true; off = 3; }
    std::vector<uint32_t> raw;
    size_t bad = utf8Decode(buf + off, rd - off, raw);
    free(buf);
    t.clear();
    t.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
      if (raw[i] == '\r') { if (i + 1 < raw.size() && raw[i + 1] == '\n') { crlf = true; continue; } t.push_back('\n'); continue; }
      t.push_back(raw[i]);
    }
    if (bad) messageBox("Внимание", "Файл не в UTF-8 (" + String(bad) + " ошибочных байт). Текст может отображаться неверно, при сохранении он будет перезаписан в UTF-8.");
    return true;
  }

  bool save() {
    toast("Сохранение...");
    String tmp = path + ".tmp";
    File f = SD.open(tmp, FILE_WRITE);
    if (!f) { messageBox("Ошибка", "Не удалось записать файл"); return false; }
    if (bom) f.write((const uint8_t*)"\xEF\xBB\xBF", 3);
    String chunk;
    for (size_t i = 0; i < t.size(); i++) {
      if (t[i] == '\n' && crlf) chunk += '\r';
      utf8Append(chunk, t[i]);
      if (chunk.length() > 2048) { f.write((const uint8_t*)chunk.c_str(), chunk.length()); chunk = ""; }
    }
    if (chunk.length()) f.write((const uint8_t*)chunk.c_str(), chunk.length());
    f.close();
    SD.remove(path);
    if (!SD.rename(tmp, path)) { messageBox("Ошибка", "Не удалось переименовать временный файл"); return false; }
    modified = false;
    return true;
  }

  void draw() {
    canvas.fillScreen(C_BG);
    drawHeader((modified ? "* " : "") + pathName(path));
    canvas.setFont(FONT);
    canvas.setTextDatum(top_left);
    int rows = textRows();
    int y0 = HEADER_H + 3;
    int cl = lineOf(cur);
    for (int r = 0; r < rows; r++) {
      int l = top + r;
      if (l >= (int)vl.size()) break;
      int y = y0 + r * LINE_H;
      canvas.setTextColor(C_TEXT);
      // табуляции рисуем как пробелы
      String s;
      for (uint32_t k = vl[l].s; k < vl[l].s + vl[l].n; k++) { uint32_t c = t[k]; if (c == '\t') s += "    "; else if (c >= 0x20) utf8Append(s, c); }
      canvas.drawString(s, TEXT_X, y);
      if (editMode && l == cl) {
        int cx = TEXT_X + xOf(cur);
        canvas.fillRect(cx, y, 2, LINE_H - 2, cursorMode ? C_OK : C_ACCENT);
      }
    }
    // прокрутка
    int total = vl.size();
    if (total > rows) {
      int h = rows * LINE_H;
      int th = max(8, h * rows / total);
      int ty = y0 + (h - th) * top / max(1, total - rows);
      canvas.fillRect(SCR_W - 4, y0, 3, h, C_HEADER);
      canvas.fillRect(SCR_W - 4, ty, 3, th, C_DIM);
    }
    if (editMode) {
      int hy = SCR_H - Keyboard::H - 14;
      canvas.fillRect(0, hy, SCR_W, 14, C_HEADER);
      canvas.setFont(FONT_S);
      canvas.setTextDatum(middle_center);
      canvas.setTextColor(C_DIM);
      canvas.drawString(voiceHook() ? "B:← SELECT:язык OPTION:курсор, держать:голос" : "B:← SELECT:язык OPTION:курсор START:сохр.", SCR_W / 2, hy + 7);
      kb.draw(SCR_H - Keyboard::H);
      if (cursorMode) {
        canvas.fillRect(0, SCR_H - Keyboard::H, SCR_W, Keyboard::H, C_DISPLAY);
        canvas.setFont(FONT);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_OK);
        canvas.drawString("Режим курсора", SCR_W / 2, SCR_H - Keyboard::H + 20);
        canvas.setFont(FONT_S);
        canvas.setTextColor(C_DIM);
        canvas.drawString("Крестовина: двигать курсор", SCR_W / 2, SCR_H - Keyboard::H + 46);
        canvas.drawString("A / OPTION: вернуться к клавиатуре   B: стереть", SCR_W / 2, SCR_H - Keyboard::H + 64);
        canvas.drawString("START: сохранить   MENU: выход", SCR_W / 2, SCR_H - Keyboard::H + 82);
      }
    } else {
      String pos = String(min(top + rows, total)) + "/" + String(total);
      drawFooter((voiceHook() ? "A:правка OPTION(держать):голос  " : "A:правка  ↑↓ ←→:листать  B:выход  ") + pos);
    }
    present();
  }

  void insert(uint32_t c) {
    t.insert(t.begin() + cur, c);
    cur++;
    modified = true;
    wrap();
    ensureVisible();
  }
  // Вставить надиктованный текст в позицию курсора
  void insertText(const String& s) {
    auto v = utf8Split(s);
    if (v.empty()) return;
    if (cur > 0 && t[cur - 1] != ' ' && t[cur - 1] != '\n') v.insert(v.begin(), (uint32_t)' ');
    t.insert(t.begin() + cur, v.begin(), v.end());
    cur += v.size();
    modified = true;
    wrap();
    ensureVisible();
  }
  void dictate() {
    String s;
    if (voiceHook() && voiceHook()(s)) insertText(s);
  }
  void backspace() {
    if (cur == 0) return;
    t.erase(t.begin() + cur - 1);
    cur--;
    modified = true;
    wrap();
    ensureVisible();
  }
  void moveVert(int d) {
    int l = lineOf(cur), nl = l + d;
    if (nl < 0 || nl >= (int)vl.size()) return;
    int x = xOf(cur);
    cur = posAtX(nl, x);
    ensureVisible();
  }

  bool askExit() {
    if (!modified) return true;
    int r = popupMenu("Сохранить изменения?", {"Сохранить и выйти", "Выйти без сохранения", "Отмена"});
    if (r == 0) return save();
    return r == 1;
  }

public:
  void run(const String& filePath) {
    memset(wcache, -1, sizeof wcache);
    kb.optionIsShift = false;
    path = filePath;
    toast("Открытие...");
    if (!load()) return;
    wrap();
    input.waitRelease();
    bool dirty = true;
    while (true) {
      if (dirty) { draw(); dirty = false; }
      input.update();
      if (!input.any()) { delay(10); continue; }
      dirty = true;
      if (input.pressed(B_START)) { if (modified) { save(); } else { toast("Изменений нет"); delay(400); } continue; }
      if (input.pressed(B_MENU)) { if (askExit()) break; continue; }

      if (!editMode) {
        int rows = textRows();
        if (input.pressed(B_DOWN)) top = min(top + 1, max(0, (int)vl.size() - rows));
        if (input.pressed(B_UP)) top = max(0, top - 1);
        if (input.pressed(B_RIGHT)) top = min(top + rows, max(0, (int)vl.size() - rows));
        if (input.pressed(B_LEFT)) top = max(0, top - rows);
        if (input.pressed(B_B)) { if (askExit()) break; }
        if (input.pressed(B_OPTION) && voiceHook() && longPress(B_OPTION)) { dictate(); continue; }
        if (input.pressed(B_A)) {
          editMode = true;
          // курсор в конец первой видимой строки, если он за экраном
          int l = lineOf(cur);
          if (l < top || l >= top + rows) cur = vl[min(top, (int)vl.size() - 1)].s;
          ensureVisible();
        }
        continue;
      }

      if (cursorMode) {
        if (input.pressed(B_LEFT) && cur > 0) { cur--; ensureVisible(); }
        if (input.pressed(B_RIGHT) && cur < t.size()) { cur++; ensureVisible(); }
        if (input.pressed(B_UP)) moveVert(-1);
        if (input.pressed(B_DOWN)) moveVert(1);
        if (input.pressed(B_B)) backspace();
        if (input.pressed(B_A) || input.pressed(B_OPTION)) cursorMode = false;
        continue;
      }

      // режим клавиатуры: OPTION переключает на перемещение курсора
      if (input.pressed(B_OPTION)) {
        // короткое нажатие - режим курсора, долгое - голосовой ввод в позицию курсора
        if (voiceHook() && longPress(B_OPTION)) dictate(); else cursorMode = true;
        continue;
      }
      int32_t k = kb.handle();
      if (k == K_BS) backspace();
      else if (k > 0) { insert((uint32_t)k); if (kb.shift && k != ' ' && k != '\n') kb.shift = false; }
    }
    input.waitRelease();
  }
};

inline void openTextEditor(const String& path) {
  TextEditor* ed = new TextEditor();
  ed->run(path);
  delete ed;
}
