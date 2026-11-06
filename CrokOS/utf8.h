// ============================================================================
//  CrokOS - UTF-8 утилиты
// ============================================================================
#pragma once
#include <Arduino.h>
#include <vector>

// Декодирование UTF-8 в кодовые точки. Возвращает число ошибочных байт.
inline size_t utf8Decode(const char* s, size_t len, std::vector<uint32_t>& out) {
  size_t bad = 0, i = 0;
  out.reserve(out.size() + len);
  while (i < len) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp; int n;
    if (c < 0x80) { cp = c; n = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else { out.push_back('?'); bad++; i++; continue; }
    bool ok = true;
    for (int k = 1; k <= n; k++) {
      if (i + k >= len || (((uint8_t)s[i + k]) & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (((uint8_t)s[i + k]) & 0x3F);
    }
    if (!ok) { out.push_back('?'); bad++; i++; continue; }
    out.push_back(cp);
    i += n + 1;
  }
  return bad;
}

inline void utf8Append(String& s, uint32_t cp) {
  char b[5] = {0};
  if (cp < 0x80) { b[0] = cp; }
  else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); }
  else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); }
  else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); }
  s += b;
}

inline String utf8Encode(const std::vector<uint32_t>& v, size_t from = 0, size_t n = (size_t)-1) {
  String s;
  size_t end = (n == (size_t)-1) ? v.size() : min(v.size(), from + n);
  s.reserve((end - from) * 2 + 1);
  for (size_t i = from; i < end; i++) utf8Append(s, v[i]);
  return s;
}

inline std::vector<uint32_t> utf8Split(const String& s) {
  std::vector<uint32_t> v;
  utf8Decode(s.c_str(), s.length(), v);
  return v;
}

inline uint32_t cpUpper(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c >= 0x430 && c <= 0x44F) return c - 0x20;
  if (c == 0x451) return 0x401;
  return c;
}
inline uint32_t cpLower(uint32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 0x20;
  if (c == 0x401) return 0x451;
  return c;
}

inline String strLower(const String& s) {
  auto v = utf8Split(s);
  for (auto& c : v) c = cpLower(c);
  return utf8Encode(v);
}

inline String fileExt(const String& name) {
  int p = name.lastIndexOf('.');
  if (p < 0) return "";
  String e = name.substring(p + 1);
  e.toLowerCase();
  return e;
}

inline String humanSize(uint64_t b) {
  char buf[16];
  if (b < 1024) snprintf(buf, sizeof buf, "%u Б", (unsigned)b);
  else if (b < 1024 * 1024) snprintf(buf, sizeof buf, "%.1f КБ", b / 1024.0);
  else if (b < 1024ull * 1024 * 1024) snprintf(buf, sizeof buf, "%.1f МБ", b / 1048576.0);
  else snprintf(buf, sizeof buf, "%.1f ГБ", b / 1073741824.0);
  return buf;
}

inline String pathJoin(const String& dir, const String& name) {
  if (dir.endsWith("/")) return dir + name;
  return dir + "/" + name;
}
inline String pathParent(const String& p) {
  int i = p.lastIndexOf('/');
  if (i <= 0) return "/";
  return p.substring(0, i);
}
inline String pathName(const String& p) {
  int i = p.lastIndexOf('/');
  return i < 0 ? p : p.substring(i + 1);
}
