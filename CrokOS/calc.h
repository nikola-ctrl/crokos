// ============================================================================
//  CrokOS - инженерный калькулятор
// ============================================================================
#pragma once
#include "ui.h"
#include <math.h>

// ---- вычисление выражения (рекурсивный спуск) ----
struct CalcParser {
  const char* p;
  bool err = false;
  bool deg = true;
  double ans = 0;

  void ws() { while (*p == ' ') p++; }
  bool isIdent(char c) { return (c >= 'a' && c <= 'z'); }

  double number() {
    char* end;
    double v = strtod(p, &end);
    if (end == p) { err = true; return 0; }
    p = end;
    return v;
  }
  double func(const String& name, double x) {
    double r = deg ? x * M_PI / 180.0 : x;
    if (name == "sin") return sin(r);
    if (name == "cos") return cos(r);
    if (name == "tan") { if (deg && fmod(fabs(x) - 90.0, 180.0) == 0.0) { err = true; return 0; } return tan(r); }
    if (name == "asin") { double v = asin(x); return deg ? v * 180.0 / M_PI : v; }
    if (name == "acos") { double v = acos(x); return deg ? v * 180.0 / M_PI : v; }
    if (name == "atan") { double v = atan(x); return deg ? v * 180.0 / M_PI : v; }
    if (name == "ln") return log(x);
    if (name == "log") return log10(x);
    if (name == "sqrt") return sqrt(x);
    if (name == "abs") return fabs(x);
    err = true;
    return 0;
  }
  // primary: число | константа | функция(...) | (expr)
  double primary() {
    ws();
    if (*p == '(') {
      p++;
      double v = expr();
      ws();
      if (*p == ')') p++;   // незакрытые скобки допускаем
      return v;
    }
    if ((*p >= '0' && *p <= '9') || *p == '.') return number();
    if (isIdent(*p)) {
      String id;
      while (isIdent(*p)) id += *p++;
      if (id == "pi") return M_PI;
      if (id == "e") return M_E;
      if (id == "ans") return ans;
      ws();
      if (*p == '(') { p++; double a = expr(); ws(); if (*p == ')') p++; return func(id, a); }
      // функция без скобок: sin 30
      return func(id, power());
    }
    err = true;
    return 0;
  }
  // постфикс: ! (факториал) и неявное умножение 2pi, 2(3), (1)(2)
  double postfix() {
    double v = primary();
    while (!err) {
      ws();
      if (*p == '!') {
        p++;
        if (v < 0 || v > 170 || v != floor(v)) { err = true; return 0; }
        double f = 1; for (int i = 2; i <= (int)v; i++) f *= i; v = f;
      } else break;
    }
    return v;
  }
  double power() {
    double b = postfix();
    ws();
    if (*p == '^') { p++; double e = unary(); return pow(b, e); }  // правоассоциативно
    return b;
  }
  double unary() {
    ws();
    if (*p == '-') { p++; return -unary(); }
    if (*p == '+') { p++; return unary(); }
    return power();
  }
  double term() {
    double v = unary();
    while (!err) {
      ws();
      if (*p == '*') { p++; v *= unary(); }
      else if (*p == '/') { p++; double d = unary(); if (d == 0) { err = true; return 0; } v /= d; }
      else if (*p == '%') { p++; v /= 100.0; }
      else if (*p == '(' || isIdent(*p) || (*p >= '0' && *p <= '9') || *p == '.') { v *= unary(); }  // неявное умножение
      else break;
    }
    return v;
  }
  double expr() {
    double v = term();
    while (!err) {
      ws();
      if (*p == '+') { p++; v += term(); }
      else if (*p == '-') { p++; v -= term(); }
      else break;
    }
    return v;
  }
  bool eval(const String& s, double& out) {
    p = s.c_str();
    err = false;
    out = expr();
    ws();
    if (*p != 0) err = true;
    if (isnan(out) || isinf(out)) err = true;
    return !err;
  }
};

inline String formatNumber(double v) {
  if (fabs(v) < 1e-12) v = 0;
  char buf[32];
  snprintf(buf, sizeof buf, "%.10g", v);
  String s = buf;
  s.replace("e+", "e");
  return s;
}

// ---- кнопки калькулятора ----
struct CalcKey { const char* label; const char* insert; };
static const CalcKey CALC_KEYS[6][5] = {
  {{"sin", "sin("}, {"cos", "cos("}, {"tan", "tan("}, {"ln", "ln("}, {"log", "log("}},
  {{"√", "sqrt("}, {"x²", "^2"}, {"x^y", "^"}, {"(", "("}, {")", ")"}},
  {{"7", "7"}, {"8", "8"}, {"9", "9"}, {"÷", "/"}, {"C", ""}},
  {{"4", "4"}, {"5", "5"}, {"6", "6"}, {"×", "*"}, {"←", ""}},
  {{"1", "1"}, {"2", "2"}, {"3", "3"}, {"−", "-"}, {"π", "pi"}},
  {{"0", "0"}, {".", "."}, {"Ans", "ans"}, {"+", "+"}, {"=", ""}},
};

inline void calculatorApp() {
  CalcParser cp;
  std::vector<String> tokens;   // чтобы "←" стирал "sin(" целиком
  String result = "0";
  bool showResult = false, resultErr = false;
  int r = 5, c = 0;
  auto exprString = [&]() { String s; for (auto& t : tokens) s += t; return s; };
  auto prettyString = [&]() {
    String s;
    for (auto& t : tokens) {
      if (t == "*") s += "×"; else if (t == "/") s += "÷"; else if (t == "sqrt(") s += "√(";
      else if (t == "pi") s += "π"; else if (t == "ans") s += "Ans"; else s += t;
    }
    return s;
  };
  auto doEval = [&]() {
    double v;
    if (tokens.empty()) return;
    if (cp.eval(exprString(), v)) { result = formatNumber(v); cp.ans = v; resultErr = false; }
    else { result = "Ошибка"; resultErr = true; }
    showResult = true;
  };
  input.waitRelease();
  bool dirty = true;
  while (true) {
    if (dirty) {
      canvas.fillScreen(C_BG);
      drawHeader(String("Калькулятор  [") + (cp.deg ? "DEG" : "RAD") + "]");
      // дисплей
      canvas.fillRoundRect(4, HEADER_H + 4, SCR_W - 8, 50, 4, C_DISPLAY);
      canvas.setFont(FONT);
      canvas.setTextDatum(top_right);
      canvas.setTextColor(C_DIM);
      String ex = prettyString();
      // хвост выражения, если длинное
      auto v = utf8Split(ex);
      while (canvas.textWidth(utf8Encode(v)) > SCR_W - 24 && !v.empty()) v.erase(v.begin());
      canvas.drawString(utf8Encode(v), SCR_W - 10, HEADER_H + 8);
      canvas.setFont(FONT_L);
      canvas.setTextColor(resultErr ? C_ERR : (showResult ? C_ACCENT_TEXT : C_TEXT));
      canvas.drawString(showResult ? "= " + result : result, SCR_W - 10, HEADER_H + 27);
      // кнопки
      int gy = HEADER_H + 60, kw = SCR_W / 5, kh = (SCR_H - gy - FOOTER_H) / 6;
      canvas.setFont(FONT);
      canvas.setTextDatum(middle_center);
      for (int i = 0; i < 6; i++) for (int j = 0; j < 5; j++) {
        bool s = (i == r && j == c);
        bool op = (j >= 3 && i >= 2) || i < 2;
        uint16_t bg = s ? C_ACCENT : (i == 5 && j == 4 ? C_SEL : (op ? C_PANEL : C_KEY));
        canvas.fillRoundRect(j * kw + 2, gy + i * kh + 1, kw - 4, kh - 2, 4, bg);
        canvas.setTextColor(s ? 0x0000 : C_TEXT);
        canvas.drawString(CALC_KEYS[i][j].label, j * kw + kw / 2, gy + i * kh + kh / 2);
      }
      drawFooter("A:ввод B:← START:= SELECT:DEG/RAD MENU:выход");
      present();
      dirty = false;
    }
    input.update();
    if (!input.any()) { delay(10); continue; }
    dirty = true;
    if (input.pressed(B_MENU)) break;
    if (input.pressed(B_UP)) r = (r + 5) % 6;
    if (input.pressed(B_DOWN)) r = (r + 1) % 6;
    if (input.pressed(B_LEFT)) c = (c + 4) % 5;
    if (input.pressed(B_RIGHT)) c = (c + 1) % 5;
    if (input.pressed(B_SELECT)) { cp.deg = !cp.deg; if (showResult) doEval(); }
    if (input.pressed(B_B)) { if (!tokens.empty()) tokens.pop_back(); showResult = false; resultErr = false; result = tokens.empty() ? "0" : ""; }
    if (input.pressed(B_START)) doEval();
    if (input.pressed(B_A)) {
      String lab = CALC_KEYS[r][c].label;
      if (lab == "=") doEval();
      else if (lab == "C") { tokens.clear(); result = "0"; showResult = false; resultErr = false; }
      else if (lab == "←") { if (!tokens.empty()) tokens.pop_back(); showResult = false; resultErr = false; result = tokens.empty() ? "0" : ""; }
      else {
        const char* ins = CALC_KEYS[r][c].insert;
        // после "=" новое число начинает новое выражение, оператор продолжает от Ans
        if (showResult) {
          bool isOp = !strcmp(ins, "+") || !strcmp(ins, "-") || !strcmp(ins, "*") || !strcmp(ins, "/") || ins[0] == '^';
          tokens.clear();
          if (isOp && !resultErr) tokens.push_back("ans");
          showResult = false; resultErr = false;
        }
        tokens.push_back(ins);
        result = "";
      }
    }
    delay(10);
  }
  input.waitRelease();
}
