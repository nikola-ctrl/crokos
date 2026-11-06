// ============================================================================
//  CrokOS - галерея: JPG / PNG / BMP с SD, автоматическое вписывание в экран
// ============================================================================
#pragma once
#include "ui.h"
#include "settings.h"
#include <WebPDec.h>   // libwebp (только декодер)

inline bool isImageExt(const String& e) { return e == "jpg" || e == "jpeg" || e == "png" || e == "bmp" || e == "webp"; }

// Формат определяем по содержимому: телефоны часто сохраняют WebP с расширением .jpg
enum ImgKind { IMG_UNKNOWN, IMG_JPG, IMG_PNG, IMG_BMP, IMG_WEBP, IMG_GIF, IMG_HEIC };
inline ImgKind imageKind(const String& path) {
  uint8_t b[16] = {0};
  for (int attempt = 0; attempt < 2; attempt++) {
    File f = sdOpenRead(path);
    if (!f) return IMG_UNKNOWN;
    memset(b, 0, sizeof b);
    size_t fs = f.size();
    int n = f.read(b, 16);
    f.close();
    if (n == 16 || (n >= 0 && (size_t)n == fs)) break;
    sdRemount();   // прочиталось не всё - сбой карты, пробуем ещё раз
  }
  if (b[0] == 0xFF && b[1] == 0xD8) return IMG_JPG;
  if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return IMG_PNG;
  if (b[0] == 'B' && b[1] == 'M') return IMG_BMP;
  if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return IMG_WEBP;
  if (!memcmp(b, "GIF8", 4)) return IMG_GIF;
  if (!memcmp(b + 4, "ftyp", 4)) return IMG_HEIC;
  return IMG_UNKNOWN;
}

// WebP: декодируем сразу в нужный размер (масштабирование внутри libwebp) в RGB565
inline bool drawWebp(const String& path, String& err) {
  File f = sdOpenRead(path);
  if (!f) { err = "Не удалось открыть файл"; return false; }
  size_t n = f.size();
  uint8_t* data = (uint8_t*)ps_malloc(n);
  if (!data) { f.close(); err = "Не хватает памяти"; return false; }
  size_t rd = f.read(data, n);
  f.close();
  if (rd != n) {   // сбой чтения - переподключаем карту и читаем заново
    sdRemount();
    f = sdOpenRead(path);
    rd = f ? f.read(data, n) : 0;
    if (f) f.close();
    if (rd != n) { free(data); err = "Ошибка чтения SD-карты"; return false; }
  }
  WebPDecoderConfig cfg;
  WebPInitDecoderConfig(&cfg);
  if (WebPGetFeatures(data, n, &cfg.input) != VP8_STATUS_OK) { free(data); err = "Повреждённый WebP"; return false; }
  if (cfg.input.has_animation) { free(data); err = "Анимированный WebP (стикер/GIF) не поддерживается"; return false; }
  int w = cfg.input.width, h = cfg.input.height;
  float sc = min((float)SCR_W / w, (float)SCR_H / h);
  if (sc > 4.f) sc = 4.f;
  int sw = constrain((int)(w * sc + 0.5f), 1, SCR_W), sh = constrain((int)(h * sc + 0.5f), 1, SCR_H);
  uint8_t* out = (uint8_t*)ps_malloc(sw * sh * 2);
  if (!out) { free(data); err = "Не хватает памяти"; return false; }
  cfg.options.use_scaling = 1;
  cfg.options.scaled_width = sw;
  cfg.options.scaled_height = sh;
  cfg.output.colorspace = MODE_RGB_565;
  cfg.output.is_external_memory = 1;
  cfg.output.u.RGBA.rgba = out;
  cfg.output.u.RGBA.stride = sw * 2;
  cfg.output.u.RGBA.size = sw * sh * 2;
  VP8StatusCode st = WebPDecode(data, n, &cfg);
  free(data);
  if (st != VP8_STATUS_OK) { free(out); err = "Ошибка декодирования WebP (" + String((int)st) + ")"; return false; }
  canvas.pushImage((SCR_W - sw) / 2, (SCR_H - sh) / 2, sw, sh, (const lgfx::swap565_t*)out);
  WebPFreeDecBuffer(&cfg.output);
  free(out);
  return true;
}

// Размер картинки из заголовка файла
inline bool imageSize(const String& path, const String& e, int& w, int& h, bool& progressive) {
  progressive = false;
  File f = sdOpenRead(path);
  if (!f) return false;
  uint8_t b[32];
  bool ok = false;
  if (e == "png") {
    if (f.read(b, 24) == 24 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
      w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
      h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
      ok = true;
    }
  } else if (e == "bmp") {
    if (f.read(b, 26) == 26 && b[0] == 'B' && b[1] == 'M') {
      w = (int32_t)(b[18] | (b[19] << 8) | (b[20] << 16) | (b[21] << 24));
      h = (int32_t)(b[22] | (b[23] << 8) | (b[24] << 16) | (b[25] << 24));
      h = abs(h);
      ok = true;
    }
  } else {  // JPEG: ищем маркер SOFn
    if (f.read(b, 2) == 2 && b[0] == 0xFF && b[1] == 0xD8) {
      while (f.available()) {
        int m;
        do { m = f.read(); } while (m != 0xFF && m >= 0);
        do { m = f.read(); } while (m == 0xFF);
        if (m < 0) break;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        uint8_t l[2];
        if (f.read(l, 2) != 2) break;
        int len = (l[0] << 8) | l[1];
        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
          uint8_t s[5];
          if (f.read(s, 5) != 5) break;
          h = (s[1] << 8) | s[2];
          w = (s[3] << 8) | s[4];
          progressive = (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE);
          ok = true;
          break;
        }
        f.seek(f.position() + len - 2);
      }
    }
  }
  f.close();
  return ok && w > 0 && h > 0;
}

// Показ одной картинки. Возвращает false, если не удалось.
inline bool showImageOnce(const String& path, String& err) {
  canvas.fillScreen(0x0000);
  ImgKind k = imageKind(path);
  if (k == IMG_WEBP) return drawWebp(path, err);
  if (k == IMG_GIF) { err = "GIF не поддерживается"; return false; }
  if (k == IMG_HEIC) { err = "HEIC (фото iPhone/Samsung) не поддерживается. Сохраните как JPG."; return false; }
  if (k == IMG_UNKNOWN) { err = "Неизвестный формат файла"; return false; }
  int w = 0, h = 0; bool prog = false;
  String realExt = k == IMG_PNG ? "png" : k == IMG_BMP ? "bmp" : "jpg";
  if (!imageSize(path, realExt, w, h, prog)) { err = "Не удалось прочитать заголовок файла"; return false; }
  if (prog) { err = "Прогрессивный JPEG не поддерживается. Пересохраните картинку как обычный (baseline) JPEG."; return false; }
  float sc = min((float)SCR_W / w, (float)SCR_H / h);
  if (sc > 4.f) sc = 4.f;
  bool ok;
  if (k == IMG_PNG) ok = canvas.drawPngFile(SD, path.c_str(), 0, 0, SCR_W, SCR_H, 0, 0, sc, sc, middle_center);
  else if (k == IMG_BMP) ok = canvas.drawBmpFile(SD, path.c_str(), 0, 0, SCR_W, SCR_H, 0, 0, sc, sc, middle_center);
  else ok = canvas.drawJpgFile(SD, path.c_str(), 0, 0, SCR_W, SCR_H, 0, 0, sc, sc, middle_center);
  if (!ok) { err = "Ошибка декодирования (" + String(w) + "×" + String(h) + ")"; return false; }
  return true;
}

// Сбои чтения SD выглядят как «битый файл» - при ошибке переподключаем карту и пробуем ещё раз
inline bool showImage(const String& path, String& err) {
  if (showImageOnce(path, err)) return true;
  if (err.indexOf("не поддерживается") >= 0 || err.indexOf("памяти") >= 0) return false;
  sdRemount();
  err = "";
  return showImageOnce(path, err);
}

// Просмотр списка картинок начиная с index
inline void galleryView(const String& dir, const std::vector<String>& names, int index) {
  if (names.empty()) return;
  bool info = true;
  input.waitRelease();
  bool reload = true, dirty = true, imgOk = false, gammaChanged = false;
  uint32_t gammaShownAt = 0;
  bool gammaHint = false;
  LGFX_Sprite* img = nullptr;
  while (true) {
    String path = pathJoin(dir, names[index]);
    if (reload) {
      canvas.fillScreen(0x0000);
      toast("Загрузка...");
      String err;
      imgOk = showImage(path, err);
      if (!imgOk) {
        canvas.fillScreen(C_BG);
        canvas.setFont(FONT);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(C_ERR);
        auto lines = wrapWords(err, 280);
        for (size_t i = 0; i < lines.size(); i++) canvas.drawString(lines[i], SCR_W / 2, 110 + i * 18);
      }
      // запоминаем исходный кадр без подписи и без гаммы
      if (!img) { img = new LGFX_Sprite(&canvas); img->setPsram(true); img->setColorDepth(16); img->createSprite(SCR_W, SCR_H); }
      canvas.pushSprite(img, 0, 0);
      reload = false; dirty = true;
    }
    if (dirty) {
      img->pushSprite(&canvas, 0, 0);
      if (imgOk) applyPhotoGamma(canvas, settings.gammaValue());
      bool showGamma = gammaShownAt && millis() - gammaShownAt < 1500;
      gammaHint = showGamma;
      if (info || showGamma) {
        canvas.fillRect(0, 0, SCR_W, 18, 0x0000);
        canvas.setFont(FONT_S);
        canvas.setTextDatum(middle_left);
        canvas.setTextColor(0xE71C);
        if (showGamma) canvas.drawString("Гамма: " + gammaText(settings.photoGamma) + "  (↑ светлее / ↓ темнее)", 4, 9);
        else canvas.drawString(fitText(names[index], 250), 4, 9);
        canvas.setTextDatum(middle_right);
        canvas.drawString(String(index + 1) + "/" + String(names.size()), SCR_W - 4, 9);
      }
      if (info) {
        canvas.fillRect(0, SCR_H - 16, SCR_W, 16, 0x0000);
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(0x8C51);
        canvas.drawString("←→: листать  ↑↓: гамма  A: подпись  B: назад", SCR_W / 2, SCR_H - 8);
      }
      present();
      dirty = false;
    }
    input.update();
    if (input.pressed(B_B) || input.pressed(B_MENU)) break;
    if (input.pressed(B_RIGHT)) { index = (index + 1) % names.size(); reload = true; }
    if (input.pressed(B_LEFT)) { index = (index - 1 + names.size()) % names.size(); reload = true; }
    if (input.pressed(B_UP) || input.pressed(B_DOWN)) {
      int d = input.pressed(B_UP) ? -1 : 1;   // меньше гамма = светлее
      settings.photoGamma = constrain(settings.photoGamma + d, 0, PHOTO_GAMMA_COUNT - 1);
      gammaChanged = true; gammaShownAt = millis(); dirty = true;
    }
    if (input.pressed(B_A)) { info = !info; dirty = true; }
    // убрать подсказку гаммы через 1,5 с
    if (gammaHint && millis() - gammaShownAt >= 1500) dirty = true;
    delay(10);
  }
  if (gammaChanged) settings.save();
  if (img) { img->deleteSprite(); delete img; }
  input.waitRelease();
}
