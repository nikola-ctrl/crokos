// ============================================================================
//  CrokOS - файловый менеджер (он же выбор файлов для редактора и галереи)
// ============================================================================
#pragma once
#include "ui.h"
#include "editor.h"
#include "gallery.h"
#include "media.h"
#include <algorithm>

enum BrowseMode { BR_ALL, BR_TEXT, BR_IMAGES, BR_MEDIA };

inline bool isTextExt(const String& e) {
  static const char* ex[] = {"txt", "md", "log", "ini", "cfg", "conf", "csv", "json", "xml", "htm", "html", "c", "h", "cpp", "hpp", "ino", "py", "js", "css", "sh", "bat", "yml", "yaml", nullptr};
  for (int i = 0; ex[i]; i++) if (e == ex[i]) return true;
  return false;
}

struct FEntry { String name; bool dir; uint32_t size; };

class FileBrowser {
  String dir = "/";
  std::vector<FEntry> items;
  ListView lv;
  BrowseMode mode;

  bool visible(const FEntry& e) const {
    if (e.dir) return true;
    String x = fileExt(e.name);
    if (mode == BR_TEXT) return isTextExt(x);
    if (mode == BR_IMAGES) return isImageExt(x);
    if (mode == BR_MEDIA) return isMediaExt(x);
    return true;
  }
  void scan() {
    items.clear();
    File d = SD.open(dir);
    if (!d) { sdRemount(); d = SD.open(dir); }
    if (!d || !d.isDirectory()) { dir = "/"; d = SD.open(dir); }
    while (File e = d.openNextFile()) {
      FEntry fe{e.name(), e.isDirectory(), (uint32_t)e.size()};
      e.close();
      if (fe.name.startsWith(".") || fe.name == "System Volume Information") continue;
      if (visible(fe)) items.push_back(fe);
    }
    d.close();
    std::sort(items.begin(), items.end(), [](const FEntry& a, const FEntry& b) {
      if (a.dir != b.dir) return a.dir;
      return strLower(a.name) < strLower(b.name);
    });
    lv.clamp(items.size());
  }
  void selectName(const String& n) {
    for (size_t i = 0; i < items.size(); i++) if (items[i].name == n) { lv.sel = i; break; }
    lv.clamp(items.size());
  }

  void drawIcon(int x, int y, const FEntry& e) {
    if (e.dir) {
      canvas.fillRect(x, y + 3, 7, 3, 0xFE60);
      canvas.fillRoundRect(x, y + 5, 14, 9, 1, 0xFE60);
      return;
    }
    String ex = fileExt(e.name);
    uint16_t col = isImageExt(ex) ? 0x4E7F : isMediaExt(ex) ? C_ACCENT : isTextExt(ex) ? C_TEXT : C_DIM;
    canvas.drawRect(x + 2, y + 2, 10, 13, col);
    if (isImageExt(ex)) { canvas.fillTriangle(x + 4, y + 13, x + 7, y + 8, x + 10, y + 13, col); }
    else if (isMediaExt(ex)) { canvas.fillTriangle(x + 5, y + 5, x + 5, y + 12, x + 10, y + 8, col); }
    else for (int k = 0; k < 3; k++) canvas.drawFastHLine(x + 4, y + 5 + k * 3, 6, col);
  }

  void draw() {
    canvas.fillScreen(C_BG);
    const char* t = mode == BR_TEXT ? "Текст: " : mode == BR_IMAGES ? "Галерея: " : "Файлы: ";
    drawHeader(String(t) + dir);
    if (items.empty()) {
      canvas.setFont(FONT);
      canvas.setTextDatum(middle_center);
      canvas.setTextColor(C_DIM);
      canvas.drawString(mode == BR_ALL ? "Папка пуста" : mode == BR_TEXT ? "Текстовых файлов нет" : "Картинок нет", SCR_W / 2, SCR_H / 2);
    }
    canvas.setFont(FONT);
    lv.draw(items.size(), [&](int i, int x, int y, int w, bool s) {
      const FEntry& e = items[i];
      drawIcon(x, y + 1, e);
      canvas.setFont(FONT);
      canvas.setTextDatum(middle_left);
      canvas.setTextColor(s ? C_SELTEXT : (e.dir ? C_LINK : C_TEXT));
      String sz = e.dir ? "" : humanSize(e.size);
      canvas.setFont(FONT_S);
      int szw = sz.length() ? canvas.textWidth(sz) + 8 : 0;
      canvas.setFont(FONT);
      canvas.drawString(fitText(e.name, w - 22 - szw), x + 20, y + lv.rowH / 2);
      if (szw) {
        canvas.setFont(FONT_S);
        canvas.setTextDatum(middle_right);
        canvas.setTextColor(s ? C_SELTEXT : C_DIM);
        canvas.drawString(sz, x + w, y + lv.rowH / 2);
      }
    });
    drawFooter("A:открыть  B:назад  START:действия  MENU:домой");
    present();
  }

  void open(const FEntry& e) {
    String p = pathJoin(dir, e.name);
    if (e.dir) { dir = p; lv.sel = lv.top = 0; scan(); return; }
    String ex = fileExt(e.name);
    if (isImageExt(ex)) {
      std::vector<String> imgs;
      int idx = 0;
      for (auto& it : items) if (!it.dir && isImageExt(fileExt(it.name))) { if (it.name == e.name) idx = imgs.size(); imgs.push_back(it.name); }
      galleryView(dir, imgs, idx);
    } else if (isVideoExt(ex)) {
      videoPlay(p);
    } else if (isAudioExt(ex)) {
      std::vector<String> tracks;
      int idx = 0;
      for (auto& it : items) if (!it.dir && isAudioExt(fileExt(it.name))) { if (it.name == e.name) idx = tracks.size(); tracks.push_back(it.name); }
      musicPlay(dir, tracks, idx);
    } else if (isTextExt(ex) || mode == BR_TEXT) {
      openTextEditor(p);
      scan(); selectName(e.name);
    } else {
      if (confirmBox("Неизвестный тип", "Открыть «" + e.name + "» как текст?")) { openTextEditor(p); scan(); selectName(e.name); }
    }
  }

  bool removeRecursive(const String& path) {
    File f = SD.open(path);
    if (!f) return false;
    if (!f.isDirectory()) { f.close(); return SD.remove(path); }
    std::vector<String> ch;
    while (File e = f.openNextFile()) { ch.push_back(pathJoin(path, e.name())); e.close(); }
    f.close();
    for (auto& c : ch) removeRecursive(c);
    return SD.rmdir(path);
  }

  void actions() {
    bool has = !items.empty();
    std::vector<String> m = {"Новый текстовый файл", "Новая папка"};
    if (has) { m.push_back("Переименовать"); m.push_back("Удалить"); }
    int r = popupMenu(has ? items[lv.sel].name : "Действия", m);
    if (r == 0) {
      String n = "заметка.txt";
      if (!textInput("Имя нового файла", n) || !n.length()) return;
      if (fileExt(n) == "") n += ".txt";
      String p = pathJoin(dir, n);
      if (SD.exists(p)) { messageBox("Ошибка", "Такой файл уже есть"); return; }
      File f = SD.open(p, FILE_WRITE);
      if (!f) { messageBox("Ошибка", "Не удалось создать файл"); return; }
      f.close();
      scan(); selectName(n);
      openTextEditor(p);
      scan(); selectName(n);
    } else if (r == 1) {
      String n = "Новая папка";
      if (!textInput("Имя папки", n) || !n.length()) return;
      if (!SD.mkdir(pathJoin(dir, n))) messageBox("Ошибка", "Не удалось создать папку");
      scan(); selectName(n);
    } else if (r == 2) {
      FEntry e = items[lv.sel];
      String n = e.name;
      if (!textInput("Новое имя", n) || !n.length() || n == e.name) return;
      if (!SD.rename(pathJoin(dir, e.name), pathJoin(dir, n))) messageBox("Ошибка", "Не удалось переименовать");
      scan(); selectName(n);
    } else if (r == 3) {
      FEntry e = items[lv.sel];
      String q = e.dir ? "Удалить папку «" + e.name + "» со всем содержимым?" : "Удалить файл «" + e.name + "»?";
      if (!confirmBox("Удаление", q)) return;
      toast("Удаление...");
      if (!removeRecursive(pathJoin(dir, e.name))) messageBox("Ошибка", "Не удалось удалить");
      scan();
    }
  }

public:
  explicit FileBrowser(BrowseMode m, const String& start = "/") : dir(start), mode(m) {}
  String currentDir() const { return dir; }

  void run() {
    if (!sdMount()) { messageBox("Нет SD-карты", "Вставьте карту памяти и перезапустите приложение."); return; }
    scan();
    input.waitRelease();
    bool dirty = true;
    while (true) {
      if (dirty) { draw(); dirty = false; }
      input.update();
      if (!input.any()) { delay(10); continue; }
      dirty = true;
      if (lv.handle(items.size())) continue;
      if (input.pressed(B_MENU)) break;
      if (input.pressed(B_A) && !items.empty()) open(items[lv.sel]);
      else if (input.pressed(B_B)) {
        if (dir == "/") break;
        String prev = pathName(dir);
        dir = pathParent(dir);
        scan(); selectName(prev);
      } else if (input.pressed(B_START) || input.pressed(B_OPTION)) actions();
    }
    input.waitRelease();
  }
};

// Запоминаем последнюю папку для каждого режима
inline void browserApp(BrowseMode m) {
  static String last[4] = {"/", "/", "/", "/"};
  FileBrowser* b = new FileBrowser(m, last[m]);
  b->run();
  last[m] = b->currentDir();
  delete b;
}
