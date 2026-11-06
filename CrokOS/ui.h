// ============================================================================
//  CrokOS - интерфейс в стиле Retro-Go: тема, шапка, списки, диалоги, клавиатура
// ============================================================================
#pragma once
#include "hw.h"
#include "utf8.h"

// ---------------------------- Тема ------------------------------------------
// Цвета берутся из текущей темы (тёмная / светлая), переключается в «Настройках»
struct Theme {
  uint16_t bg, header, text, dim, sel, seltext, panel, key, display, iconDim, link, accentText;
};
static const Theme THEME_DARK  = {0x0841, 0x2104, 0xE71C, 0x8C51, 0x3A0B, 0xFFFF, 0x18C3, 0x3186, 0x0000, 0x39E7, 0x9E7F, 0xFD20};
static const Theme THEME_LIGHT = {0xEF7D, 0xD69A, 0x18E3, 0x6B4D, 0xAE5E, 0x0000, 0xFFDF, 0xCE59, 0xFFFF, 0xBDD7, 0x1AD7, 0xB2A0};
extern Theme theme;
inline void applyTheme(bool light) { theme = light ? THEME_LIGHT : THEME_DARK; }

#define C_BG       (theme.bg)
#define C_HEADER   (theme.header)
#define C_TEXT     (theme.text)
#define C_DIM      (theme.dim)
#define C_SEL      (theme.sel)
#define C_SELTEXT  (theme.seltext)
#define C_PANEL    (theme.panel)
#define C_KEY      (theme.key)
#define C_DISPLAY  (theme.display)
#define C_ICONDIM  (theme.iconDim)
#define C_LINK     (theme.link)
#define C_ACCENT_TEXT (theme.accentText)   // оранжевый для текста (в светлой теме темнее)
#define C_ACCENT   0xFD20   // оранжевый, как акценты Retro-Go
#define C_OK       0x4E68   // зелёный
#define C_ERR      0xF206   // красный

// Сглаженные шрифты DejaVu Sans с кириллицей (fonts_data.h), грузятся один раз
#include "fonts_data.h"
struct UIFonts {
  lgfx::PointerWrapper src[3];
  lgfx::VLWfont font[3];
  void load() {
    src[0].set(VLW_S, sizeof(VLW_S)); src[1].set(VLW_M, sizeof(VLW_M)); src[2].set(VLW_L, sizeof(VLW_L));
    for (int i = 0; i < 3; i++) font[i].loadFont(&src[i]);
  }
};
extern UIFonts uiFonts;
#define FONT_S     (&uiFonts.font[0])
#define FONT       (&uiFonts.font[1])
#define FONT_L     (&uiFonts.font[2])

#define HEADER_H 22
#define FOOTER_H 16
#define SCR_W 320
#define SCR_H 240

inline void present() { displayPresent(); }

// Обрезать строку по ширине с многоточием
inline String fitText(const String& s, int maxw) {
  if (canvas.textWidth(s) <= maxw) return s;
  auto v = utf8Split(s);
  int ell = canvas.textWidth("…");
  while (!v.empty()) {
    v.pop_back();
    String t = utf8Encode(v);
    if (canvas.textWidth(t) + ell <= maxw) return t + "…";
  }
  return "…";
}

inline void drawBattery(int x, int y) {
  int p = batteryPercent();
  canvas.setFont(FONT_S);
  canvas.setTextDatum(middle_right);
  canvas.setTextColor(C_TEXT);
  String t = String(p) + "%";
  canvas.drawString(t, x - 22, y + 11);
  int bx = x - 19, by = y + 6;
  canvas.drawRect(bx, by, 16, 10, C_TEXT);
  canvas.fillRect(bx + 16, by + 3, 2, 4, C_TEXT);
  int w = (p * 12 + 50) / 100;
  canvas.fillRect(bx + 2, by + 2, w, 6, p <= 15 ? C_ERR : C_OK);
}

inline void drawHeader(const String& title) {
  canvas.fillRect(0, 0, SCR_W, HEADER_H, C_HEADER);
  canvas.drawFastHLine(0, HEADER_H - 1, SCR_W, C_ACCENT);
  canvas.setFont(FONT);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(fitText(title, 240), 6, HEADER_H / 2);
  drawBattery(SCR_W - 4, 0);
}

inline void drawFooter(const String& hints) {
  canvas.fillRect(0, SCR_H - FOOTER_H, SCR_W, FOOTER_H, C_HEADER);
  canvas.setFont(FONT_S);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_DIM);
  canvas.drawString(hints, SCR_W / 2, SCR_H - FOOTER_H / 2);
}

// ---------------------------- Список ----------------------------------------
struct ListView {
  int sel = 0, top = 0;
  int rowH = 20;
  int y0 = HEADER_H + 2, h = SCR_H - HEADER_H - FOOTER_H - 4;
  int rows() const { return h / rowH; }

  // возвращает true, если выбор поменялся
  bool handle(int count) {
    if (count <= 0) return false;
    int old = sel;
    if (input.pressed(B_UP)) sel = (sel - 1 + count) % count;
    if (input.pressed(B_DOWN)) sel = (sel + 1) % count;
    if (input.pressed(B_LEFT)) sel = max(0, sel - rows());
    if (input.pressed(B_RIGHT)) sel = min(count - 1, sel + rows());
    clamp(count);
    return sel != old;
  }
  void clamp(int count) {
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + rows()) top = sel - rows() + 1;
    if (top < 0) top = 0;
  }
  // рисует строки; drawItem(i, x, y, w, selected)
  template <typename F>
  void draw(int count, F drawItem) {
    for (int r = 0; r < rows(); r++) {
      int i = top + r;
      if (i >= count) break;
      int y = y0 + r * rowH;
      bool s = (i == sel);
      if (s) canvas.fillRoundRect(2, y, SCR_W - 10, rowH - 1, 3, C_SEL);
      drawItem(i, 6, y, SCR_W - 16, s);
    }
    // полоса прокрутки
    if (count > rows()) {
      int th = max(10, h * rows() / count);
      int ty = y0 + (h - th) * top / max(1, count - rows());
      canvas.fillRect(SCR_W - 5, y0, 3, h, C_HEADER);
      canvas.fillRect(SCR_W - 5, ty, 3, th, C_DIM);
    }
  }
};

// ---------------------------- Диалоги ---------------------------------------
// Рисует рамку окна поверх текущего кадра
inline void drawPanel(int x, int y, int w, int h, const String& title) {
  canvas.fillRoundRect(x, y, w, h, 6, C_PANEL);
  canvas.drawRoundRect(x, y, w, h, 6, C_ACCENT);
  if (title.length()) {
    canvas.setFont(FONT);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(C_ACCENT_TEXT);
    canvas.drawString(fitText(title, w - 16), x + w / 2, y + 6);
  }
}

// Перенос текста по словам в пределах ширины
inline std::vector<String> wrapWords(const String& text, int maxw) {
  std::vector<String> lines;
  int start = 0;
  while (start <= (int)text.length()) {
    int nl = text.indexOf('\n', start);
    String para = text.substring(start, nl < 0 ? text.length() : nl);
    String line;
    int p = 0;
    while (p <= (int)para.length()) {
      int sp = para.indexOf(' ', p);
      String word = para.substring(p, sp < 0 ? para.length() : sp);
      String cand = line.length() ? line + " " + word : word;
      if (canvas.textWidth(cand) > maxw && line.length()) { lines.push_back(line); line = word; }
      else line = cand;
      if (sp < 0) break;
      p = sp + 1;
    }
    lines.push_back(line);
    if (nl < 0) break;
    start = nl + 1;
  }
  return lines;
}

// Сообщение, закрывается A или B
inline void messageBox(const String& title, const String& msg) {
  canvas.setFont(FONT);
  auto lines = wrapWords(msg, 260);
  int h = 40 + lines.size() * 18 + 22;
  h = min(h, 220);
  int y = (SCR_H - h) / 2;
  drawPanel(20, y, 280, h, title);
  canvas.setTextDatum(top_center);
  canvas.setTextColor(C_TEXT);
  for (size_t i = 0; i < lines.size() && 32 + i * 18 < (size_t)h - 22; i++)
    canvas.drawString(lines[i], SCR_W / 2, y + 30 + i * 18);
  canvas.setFont(FONT_S);
  canvas.setTextColor(C_DIM);
  canvas.drawString("A: OK", SCR_W / 2, y + h - 18);
  present();
  input.waitRelease();
  while (true) {
    input.update();
    if (input.pressed(B_A) || input.pressed(B_B) || input.pressed(B_START)) break;
    delay(10);
  }
  input.waitRelease();
}

// Короткое сообщение без ожидания
inline void toast(const String& msg) {
  canvas.setFont(FONT);
  int w = min(300, (int)canvas.textWidth(msg) + 30);
  drawPanel((SCR_W - w) / 2, 100, w, 40, "");
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(fitText(msg, w - 20), SCR_W / 2, 120);
  present();
}

// Да/Нет
inline bool confirmBox(const String& title, const String& msg, bool defYes = false) {
  bool yes = defYes;
  canvas.setFont(FONT);
  auto lines = wrapWords(msg, 260);
  int h = 40 + lines.size() * 18 + 34;
  int y = (SCR_H - h) / 2;
  LGFX_Sprite* bg = nullptr;  // перерисовываем только кнопки
  input.waitRelease();
  while (true) {
    drawPanel(20, y, 280, h, title);
    canvas.setFont(FONT);
    canvas.setTextDatum(top_center);
    canvas.setTextColor(C_TEXT);
    for (size_t i = 0; i < lines.size(); i++) canvas.drawString(lines[i], SCR_W / 2, y + 30 + i * 18);
    int by = y + h - 30;
    const char* lab[2] = {"Да", "Нет"};
    for (int k = 0; k < 2; k++) {
      int bx = 60 + k * 110;
      bool s = (k == 0) == yes;
      canvas.fillRoundRect(bx, by, 90, 22, 4, s ? C_SEL : C_HEADER);
      if (s) canvas.drawRoundRect(bx, by, 90, 22, 4, C_ACCENT);
      canvas.setTextDatum(middle_center);
      canvas.setTextColor(s ? C_SELTEXT : C_DIM);
      canvas.drawString(lab[k], bx + 45, by + 11);
    }
    present();
    while (true) {
      input.update();
      if (input.pressed(B_LEFT) || input.pressed(B_RIGHT)) { yes = !yes; break; }
      if (input.pressed(B_A)) { input.waitRelease(); return yes; }
      if (input.pressed(B_B)) { input.waitRelease(); return false; }
      delay(10);
    }
  }
  (void)bg;
}

// Всплывающее меню, возвращает индекс или -1
inline int popupMenu(const String& title, const std::vector<String>& items) {
  if (items.empty()) return -1;
  int sel = 0, top = 0;
  const int VIS = min((int)items.size(), 8);   // больше 8 пунктов - список прокручивается
  int h = 36 + VIS * 22 + 6;
  int y = (SCR_H - h) / 2;
  input.waitRelease();
  while (true) {
    if (sel < top) top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;
    drawPanel(36, y, 248, h, title);
    canvas.setFont(FONT);
    for (int r = 0; r < VIS; r++) {
      int i = top + r;
      int iy = y + 32 + r * 22;
      bool s = i == sel;
      if (s) canvas.fillRoundRect(42, iy, 236, 21, 3, C_SEL);
      canvas.setTextDatum(middle_left);
      canvas.setTextColor(s ? C_SELTEXT : C_TEXT);
      canvas.drawString(fitText(items[i], 220), 52, iy + 11);
    }
    if ((int)items.size() > VIS) {   // полоса прокрутки
      int th = max(8, VIS * 22 * VIS / (int)items.size());
      int ty = y + 32 + (VIS * 22 - th) * top / ((int)items.size() - VIS);
      canvas.fillRect(279, ty, 3, th, C_DIM);
    }
    present();
    while (true) {
      input.update();
      if (input.pressed(B_UP)) { sel = (sel - 1 + items.size()) % items.size(); break; }
      if (input.pressed(B_DOWN)) { sel = (sel + 1) % items.size(); break; }
      if (input.pressed(B_A)) { input.waitRelease(); return sel; }
      if (input.pressed(B_B) || input.pressed(B_MENU)) { input.waitRelease(); return -1; }
      delay(10);
    }
  }
}

// ---------------------------- Голосовой ввод (хук) --------------------------
// Реализация в voice.h (нужна сеть). Пусто - голосового ввода нет.
typedef bool (*VoiceHook)(String& out);
inline VoiceHook& voiceHook() { static VoiceHook h = nullptr; return h; }
// Кнопка только что нажата: держат ли её дольше порога (true = долгое нажатие).
inline bool longPress(Btn b, uint32_t ms = 450) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { input.update(); if (!input.held(b)) return false; delay(5); }
  return true;
}

// ---------------------------- Экранная клавиатура ---------------------------
enum : int32_t { K_SHIFT = -1, K_LANG = -2, K_BS = -3, K_DONE = -4, K_NONE = -100 };

struct Keyboard {
  int layout = 1;      // 0 EN, 1 RU, 2 символы
  bool shift = false;
  int row = 1, col = 0;
  static const int ROW_H = 20;
  static const int ROWS = 5;
  static const int H = ROWS * ROW_H + 2;
  bool enterIsDone = false;  // для однострочного ввода
  bool optionIsShift = true; // OPTION = регистр (в редакторе OPTION занят курсором)

  const char* rowStr(int r) const {
    static const char* L[3][4] = {
      {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?"},
      {"1234567890", "йцукенгшщзхъ", "фывапролджэё", "ячсмитьбю,.?"},
      {"!@#$%^&*()", "-_=+[]{}<>", ";:'\"`~\\|/", ",.?!№«»×÷…"},
    };
    return L[layout][r];
  }
  int rowCount(int r) const {
    if (r == 4) return 5;
    return utf8Split(rowStr(r)).size();
  }
  // ширины нижнего ряда в долях из 12
  static int bottomUnits(int c) { static const int u[5] = {2, 2, 4, 2, 2}; return u[c]; }
  void keyRect(int r, int c, int y0, int& x, int& w) const {
    if (r == 4) {
      int acc = 0;
      for (int k = 0; k < c; k++) acc += bottomUnits(k);
      x = acc * SCR_W / 12; w = bottomUnits(c) * SCR_W / 12;
    } else {
      int n = rowCount(r);
      x = c * SCR_W / n; w = (c + 1) * SCR_W / n - x;
    }
  }
  int32_t keyCode(int r, int c) const {
    if (r == 4) { static const int32_t k[5] = {K_SHIFT, K_LANG, ' ', K_BS, '\n'}; return k[c]; }
    auto v = utf8Split(rowStr(r));
    uint32_t cp = v[c];
    return shift ? cpUpper(cp) : cp;
  }
  String keyLabel(int r, int c) const {
    if (r == 4) {
      switch (c) {
        case 0: return shift ? "АБВ" : "абв";
        case 1: return layout == 0 ? "EN" : layout == 1 ? "RU" : "#!?";
        case 2: return "Пробел";
        case 3: return "←";
        default: return enterIsDone ? "OK" : "Ввод";
      }
    }
    String s; utf8Append(s, (uint32_t)keyCode(r, c)); return s;
  }
  void draw(int y0) {
    canvas.fillRect(0, y0, SCR_W, H, C_HEADER);
    canvas.setFont(FONT);
    canvas.setTextDatum(middle_center);
    for (int r = 0; r < ROWS; r++) {
      int n = rowCount(r);
      for (int c = 0; c < n; c++) {
        int x, w; keyRect(r, c, y0, x, w);
        int y = y0 + 2 + r * ROW_H;
        bool s = (r == row && c == col);
        uint16_t bg = s ? C_ACCENT : (r == 4 ? C_PANEL : C_KEY);
        canvas.fillRoundRect(x + 1, y, w - 2, ROW_H - 2, 3, bg);
        canvas.setTextColor(s ? 0x0000 : C_TEXT);
        canvas.drawString(keyLabel(r, c), x + w / 2, y + ROW_H / 2 - 1);
      }
    }
  }
  // Навигация. Возвращает код нажатой клавиши или K_NONE.
  int32_t handle() {
    if (input.pressed(B_LEFT)) { col = (col - 1 + rowCount(row)) % rowCount(row); }
    if (input.pressed(B_RIGHT)) { col = (col + 1) % rowCount(row); }
    if (input.pressed(B_UP) || input.pressed(B_DOWN)) {
      int x, w; keyRect(row, col, 0, x, w);
      int cx = x + w / 2;
      row = input.pressed(B_UP) ? (row - 1 + ROWS) % ROWS : (row + 1) % ROWS;
      int n = rowCount(row), best = 0, bd = 9999;
      for (int c = 0; c < n; c++) { int kx, kw; keyRect(row, c, 0, kx, kw); int d = abs(kx + kw / 2 - cx); if (d < bd) { bd = d; best = c; } }
      col = best;
    }
    if (input.pressed(B_SELECT)) { nextLayout(); }
    if (optionIsShift && input.pressed(B_OPTION)) { shift = !shift; }
    if (input.pressed(B_B)) return K_BS;
    if (input.pressed(B_A)) {
      int32_t k = keyCode(row, col);
      if (k == K_SHIFT) { shift = !shift; return K_NONE; }
      if (k == K_LANG) { nextLayout(); return K_NONE; }
      if (k == '\n' && enterIsDone) return K_DONE;
      return k;
    }
    return K_NONE;
  }
  void nextLayout() {
    layout = (layout + 1) % 3;
    if (col >= rowCount(row)) col = rowCount(row) - 1;
  }
};

// Ввод одной строки (имя файла и т.п.). true = подтверждено
inline bool textInput(const String& title, String& value, int layout = 1) {
  Keyboard kb;
  kb.layout = layout;   // 0 EN, 1 RU, 2 символы
  kb.enterIsDone = true;
  auto v = utf8Split(value);
  input.waitRelease();
  bool dirty = true;
  while (true) {
    if (dirty) {
      canvas.fillScreen(C_BG);
      drawHeader(title);
      canvas.fillRoundRect(8, 40, SCR_W - 16, 34, 4, C_PANEL);
      canvas.drawRoundRect(8, 40, SCR_W - 16, 34, 4, C_ACCENT);
      canvas.setFont(FONT);
      canvas.setTextDatum(middle_left);
      canvas.setTextColor(C_TEXT);
      String s = utf8Encode(v);
      // показываем хвост, если не влезает
      int maxw = SCR_W - 40;
      String shown = s;
      auto vv = v;
      while (canvas.textWidth(shown) > maxw && !vv.empty()) { vv.erase(vv.begin()); shown = utf8Encode(vv); }
      canvas.drawString(shown, 16, 57);
      int cx = 16 + canvas.textWidth(shown) + 1;
      canvas.fillRect(cx, 47, 2, 20, C_ACCENT);
      canvas.setFont(FONT_S);
      canvas.setTextDatum(top_center);
      canvas.setTextColor(C_DIM);
      canvas.drawString(voiceHook() ? "B: стереть  SELECT: язык  OPTION: регистр / держать: голос" : "B: стереть  SELECT: язык  OPTION: регистр", SCR_W / 2, 84);
      canvas.drawString("START: готово  MENU: отмена", SCR_W / 2, 100);
      kb.draw(SCR_H - Keyboard::H);
      present();
      dirty = false;
    }
    input.update();
    if (input.pressed(B_MENU)) { input.waitRelease(); return false; }
    if (input.pressed(B_START)) { value = utf8Encode(v); input.waitRelease(); return true; }
    if (input.pressed(B_OPTION)) {
      // короткое нажатие - регистр, долгое - голосовой ввод (текст добавляется в конец)
      if (voiceHook() && longPress(B_OPTION)) {
        String t;
        if (voiceHook()(t)) {
          if (!v.empty() && v.back() != ' ') v.push_back(' ');
          for (auto c : utf8Split(t)) if (c != '\n') v.push_back(c);
        }
      } else kb.shift = !kb.shift;
      dirty = true;
      continue;
    }
    if (input.any()) {
      int32_t k = kb.handle();
      if (k == K_DONE) { value = utf8Encode(v); input.waitRelease(); return true; }
      if (k == K_BS) { if (!v.empty()) v.pop_back(); }
      else if (k > 0 && k != '\n') { v.push_back(k); if (kb.shift) kb.shift = false; }
      dirty = true;
    }
    delay(10);
  }
}
