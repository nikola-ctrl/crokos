// ============================================================================
//  CrokOS - мини-«телефон» для CrokPocket (ESP32-S3 N16R8)
//  Файлы, TXT-редактор, галерея, калькулятор, Wi-Fi загрузка, переход в Retro-Go.
//  Прошивается в раздел factory (0x400000) рядом с Retro-Go.
// ============================================================================
#include "hw.h"
#include "ui.h"
#include "editor.h"
#include "gallery.h"
#include "calc.h"
#include "webfiles.h"
#include "browser.h"
#include "settings.h"
#include "gemini.h"
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_partition.h>

// Основной задаче нужен большой стек: HTTPS (mbedTLS) + интерфейс не помещаются в стандартные 8 КБ
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

RTC_NOINIT_ATTR Crumb crumb;

LGFX lcd;
LGFX_Sprite canvas(&lcd);
Input input;
bool sdOk = false;
SPIClass sdSPI(HSPI);
UIFonts uiFonts;
Theme theme = THEME_DARK;
Settings settings;


// ---------------------------- Переход в Retro-Go ----------------------------
void launchRetroGo() {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "launcher");
  if (!p) { messageBox("Retro-Go не найден", "На флешке нет раздела launcher. Прошейте полный образ Retro-Go с разделом factory."); return; }
  toast("Запуск Retro-Go...");
  if (esp_ota_set_boot_partition(p) != ESP_OK) { messageBox("Ошибка", "Не удалось выбрать раздел загрузки"); return; }
  if (sdOk) SD.end();
  delay(300);
  esp_restart();
}

// ---------------------------- Иконки главного экрана ------------------------
void iconFiles(int cx, int cy, uint16_t c) {
  canvas.fillRoundRect(cx - 36, cy - 26, 30, 12, 3, c);
  canvas.fillRoundRect(cx - 36, cy - 18, 72, 48, 5, c);
  canvas.fillRoundRect(cx - 32, cy - 10, 64, 36, 3, C_BG);
  canvas.fillRoundRect(cx - 30, cy - 8, 60, 32, 3, c);
}
void iconText(int cx, int cy, uint16_t c) {
  canvas.fillRoundRect(cx - 28, cy - 36, 56, 72, 5, c);
  for (int i = 0; i < 6; i++) canvas.fillRect(cx - 20, cy - 26 + i * 10, i == 5 ? 24 : 40, 4, C_BG);
}
void iconGallery(int cx, int cy, uint16_t c) {
  canvas.fillRoundRect(cx - 40, cy - 30, 80, 60, 5, c);
  canvas.fillRect(cx - 34, cy - 24, 68, 48, C_BG);
  canvas.fillCircle(cx + 18, cy - 10, 7, c);
  canvas.fillTriangle(cx - 34, cy + 24, cx - 10, cy - 6, cx + 14, cy + 24, c);
  canvas.fillTriangle(cx, cy + 24, cx + 16, cy + 6, cx + 34, cy + 24, c);
}
void iconCalc(int cx, int cy, uint16_t c) {
  canvas.fillRoundRect(cx - 28, cy - 36, 56, 72, 6, c);
  canvas.fillRect(cx - 22, cy - 30, 44, 16, C_BG);
  for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) canvas.fillRoundRect(cx - 22 + k * 16, cy - 8 + r * 14, 12, 10, 2, C_BG);
}
void iconWifi(int cx, int cy, uint16_t c) {
  for (int i = 0; i < 3; i++) {
    int r = 14 + i * 14;
    canvas.fillArc(cx, cy + 22, r + 5, r, 225, 315, c);
  }
  canvas.fillCircle(cx, cy + 22, 6, c);
}
void iconGames(int cx, int cy, uint16_t c) {
  canvas.fillRoundRect(cx - 42, cy - 20, 84, 44, 20, c);
  canvas.fillRect(cx - 30, cy - 2, 18, 6, C_BG);
  canvas.fillRect(cx - 24, cy - 8, 6, 18, C_BG);
  canvas.fillCircle(cx + 18, cy - 4, 5, C_BG);
  canvas.fillCircle(cx + 28, cy + 6, 5, C_BG);
}

void iconSettings(int cx, int cy, uint16_t c) {
  for (int i = 0; i < 8; i++) {
    float an = i * 3.14159f / 4;
    int x = cx + cosf(an) * 30, y = cy + sinf(an) * 30;
    canvas.fillCircle(x, y, 8, c);
  }
  canvas.fillCircle(cx, cy, 30, c);
  canvas.fillCircle(cx, cy, 13, C_BG);
}

void iconGemini(int cx, int cy, uint16_t c) {
  // четырёхлучевая звезда
  canvas.fillTriangle(cx, cy - 38, cx - 11, cy, cx + 11, cy, c);
  canvas.fillTriangle(cx, cy + 38, cx - 11, cy, cx + 11, cy, c);
  canvas.fillTriangle(cx - 38, cy, cx, cy - 11, cx, cy + 11, c);
  canvas.fillTriangle(cx + 38, cy, cx, cy - 11, cx, cy + 11, c);
  canvas.fillCircle(cx, cy, 11, c);
}

void iconPlayer(int cx, int cy, uint16_t c) {
  canvas.fillRoundRect(cx - 40, cy - 30, 80, 60, 8, c);
  canvas.fillTriangle(cx - 10, cy - 16, cx - 10, cy + 16, cx + 16, cy, C_BG);
}

struct App { const char* name; const char* hint; void (*icon)(int, int, uint16_t); };
static const App APPS[] = {
  {"Файлы", "Файловый менеджер SD-карты", iconFiles},
  {"Текст", "Чтение и редактирование .txt", iconText},
  {"Галерея", "Просмотр JPG, PNG, BMP", iconGallery},
  {"Плеер", "Видео AVI, музыка MP3 и WAV", iconPlayer},
  {"Калькулятор", "Инженерный калькулятор", iconCalc},
  {"Нейросеть", "Gemini или OpenRouter: вопросы и ответы", iconGemini},
  {"Wi-Fi", "Загрузка файлов с телефона", iconWifi},
  {"Настройки", "Тема, гамма фото и экрана", iconSettings},
  {"Игры", "Перезагрузка в Retro-Go", iconGames},
};
static const int APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);

void drawHome(int sel) {
  canvas.fillScreen(C_BG);
  drawHeader(sdOk ? "CrokPocket" : "CrokPocket  (нет SD-карты)");
  int cx = SCR_W / 2, cy = 104;
  // соседние приложения по краям, приглушённо
  int prev = (sel - 1 + APP_COUNT) % APP_COUNT, next = (sel + 1) % APP_COUNT;
  APPS[prev].icon(cx - 125, cy, C_ICONDIM);
  APPS[next].icon(cx + 125, cy, C_ICONDIM);
  canvas.fillRoundRect(cx - 62, cy - 52, 124, 104, 12, C_HEADER);
  canvas.drawRoundRect(cx - 62, cy - 52, 124, 104, 12, C_ACCENT);
  APPS[sel].icon(cx, cy, C_TEXT);
  canvas.setTextDatum(middle_center);
  canvas.setFont(FONT);
  canvas.setTextColor(C_ACCENT_TEXT);
  canvas.drawString("←", cx - 74, cy);
  canvas.drawString("→", cx + 74, cy);
  canvas.setFont(FONT_L);
  canvas.setTextColor(C_TEXT);
  canvas.drawString(APPS[sel].name, cx, 178);
  canvas.setFont(FONT_S);
  canvas.setTextColor(C_DIM);
  canvas.drawString(APPS[sel].hint, cx, 199);
  for (int i = 0; i < APP_COUNT; i++) {
    int x = cx - (APP_COUNT - 1) * 7 + i * 14;
    if (i == sel) canvas.fillCircle(x, 212, 4, C_ACCENT);
    else canvas.fillCircle(x, 212, 3, C_ICONDIM);
  }
  drawFooter("←→: выбор   A: открыть");
  present();
}

// Если прошлый запуск закончился сбоем - показать причину (вместо «молча перезагрузилась»)
void reportLastReset() {
  esp_reset_reason_t r = esp_reset_reason();
  const char* why = nullptr;
  switch (r) {
    case ESP_RST_PANIC: why = "программная ошибка (panic)"; break;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: why = "зависание (сторожевой таймер)"; break;
    case ESP_RST_BROWNOUT: why = "просадка питания (brownout) - не хватает тока от аккумулятора/USB"; break;
    default: break;
  }
  bool haveCrumb = crumb.magic == 0xC0DE5EED && crumb.where[0];
  if (why) {
    String msg = String("Причина: ") + why + ".";
    if (haveCrumb) msg += String("\nГде: ") + crumb.where + ".";
    Serial.printf("[CrokOS] last reset: %d (%s) at %s\n", (int)r, why, haveCrumb ? crumb.where : "-");
    messageBox("Консоль перезагрузилась", msg);
  }
  crumb.magic = 0;
}

void runApp(int i) {
  switch (i) {
    case 0: browserApp(BR_ALL); break;
    case 1: browserApp(BR_TEXT); break;
    case 2: browserApp(BR_IMAGES); break;
    case 3: browserApp(BR_MEDIA); break;
    case 4: calculatorApp(); break;
    case 5: geminiApp(); break;
    case 6: wifiApp(); break;
    case 7: settingsApp(); break;
    case 8: launchRetroGo(); break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  lcd.init();
  lcd.setRotation(DISP_ROTATIONS[DISP_ROT_DEFAULT]);
  lcd.setBrightness(255);
  lcd.fillScreen(0x0000);
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCR_W, SCR_H)) {
    lcd.setTextColor(TFT_RED);
    lcd.drawString("PSRAM error", 10, 10);
    while (true) delay(1000);
  }
  uiFonts.load();
  analogReadResolution(12);
  input.begin();
  storageReady();
  // START + SELECT при включении - сброс параметров экрана (если выставили что-то нечитаемое)
  if (input.held(B_START) && input.held(B_SELECT)) { settings.resetDisplay(); settings.save(); }
  applyDisplaySettings();
  applyTheme(settings.lightTheme);
  applyPanelGamma(settings.panelGammaRG);
  reportLastReset();
}

void loop() {
  static int sel = 0;
  static bool dirty = true;
  static uint32_t lastBat = 0;
  if (dirty) { drawHome(sel); dirty = false; }
  input.update();
  if (input.pressed(B_LEFT) || input.pressed(B_UP)) { sel = (sel - 1 + APP_COUNT) % APP_COUNT; dirty = true; }
  if (input.pressed(B_RIGHT) || input.pressed(B_DOWN)) { sel = (sel + 1) % APP_COUNT; dirty = true; }
  if (input.pressed(B_A) || input.pressed(B_START)) {
    input.waitRelease();
    storageReady();   // если при включении карта не определилась - пробуем снова
    runApp(sel);
    dirty = true;
  }
  // обновление заряда раз в 10 с
  if (millis() - lastBat > 10000) { lastBat = millis(); dirty = true; }
  delay(10);
}
