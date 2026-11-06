// ============================================================================
//  CrokOS - железо CrokPocket: дисплей, кнопки, SD, батарея
//  Пины взяты из конфига Retro-Go (targets/crokpocket/config.h)
// ============================================================================
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LovyanGFX.hpp>

// ---------------------------- Пины ------------------------------------------
#define PIN_LCD_MOSI 12
#define PIN_LCD_SCLK 48
#define PIN_LCD_CS   14
#define PIN_LCD_DC   47
#define PIN_LCD_RST  3
#define PIN_LCD_BL   39   // у вашего модуля подсветка запитана напрямую

#define PIN_SD_MISO  9
#define PIN_SD_MOSI  11
#define PIN_SD_CLK   13
#define PIN_SD_CS    10

#define PIN_BATTERY  4    // делитель 1:2 (ADC1_CH3)
#define PIN_LED      38

// Микрофон INMP441 (I2S): L/R -> GND, VDD -> 3.3 В
#define PIN_MIC_SCK  41
#define PIN_MIC_WS   42
#define PIN_MIC_SD   40

// ---------------------------- Дисплей ---------------------------------------
// Контроллер ST7789, родной драйвер. Физически панель 240x320 (портрет),
// альбомная ориентация, инверсия и порядок цветов выставляются из настроек (settings.h).
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = false;     // меняется в настройках (applyDisplaySettings)
      cfg.rgb_order = false;  // false = BGR; меняется в настройках
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

extern LGFX lcd;
extern LGFX_Sprite canvas;   // буфер кадра 320x240 в PSRAM
inline void displayPresent() { canvas.pushSprite(0, 0); }

// ---------------------------- Кнопки ----------------------------------------
enum Btn : uint8_t { B_UP, B_RIGHT, B_DOWN, B_LEFT, B_SELECT, B_START, B_MENU, B_OPTION, B_A, B_B, B_COUNT };
static const uint8_t BTN_PINS[B_COUNT] = {7, 6, 46, 45, 16, 17, 18, 8, 15, 5};

struct Input {
  uint16_t cur = 0, prev = 0, evt = 0;
  uint32_t downAt[B_COUNT] = {0};
  uint32_t nextRep[B_COUNT] = {0};

  void begin() {
    for (int i = 0; i < B_COUNT; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
    // не реагировать на кнопку, зажатую при старте
    delay(20);
    update(); update();
    evt = 0;
  }
  void update() {
    prev = cur;
    uint16_t s = 0;
    for (int i = 0; i < B_COUNT; i++) if (digitalRead(BTN_PINS[i]) == LOW) s |= (1 << i);
    cur = s;
    evt = 0;
    uint32_t now = millis();
    for (int i = 0; i < B_COUNT; i++) {
      bool d = cur & (1 << i), pd = prev & (1 << i);
      if (d && !pd) { evt |= (1 << i); downAt[i] = now; nextRep[i] = now + 380; }
      else if (d && i <= B_LEFT && now >= nextRep[i]) { evt |= (1 << i); nextRep[i] = now + 70; }  // автоповтор крестовины
    }
  }
  bool pressed(Btn b) const { return evt & (1 << b); }
  bool held(Btn b) const { return cur & (1 << b); }
  bool released(Btn b) const { return !(cur & (1 << b)) && (prev & (1 << b)); }
  uint32_t heldMs(Btn b) const { return held(b) ? millis() - downAt[b] : 0; }
  bool any() const { return evt != 0; }
  // дождаться, пока все кнопки отпущены (чтобы нажатие не "проваливалось" в следующий экран)
  void waitRelease() { while (true) { update(); if (!cur) break; delay(10); } evt = 0; }
};
extern Input input;

// ---------------------------- SD и батарея ----------------------------------
extern bool sdOk;

// «Хлебные крошки» в памяти RTC: переживают перезагрузку, чтобы показать, где случился сбой
struct Crumb { uint32_t magic; char where[64]; };
extern Crumb crumb;
inline void mark(const char* w) {
  crumb.magic = 0xC0DE5EED;
  strncpy(crumb.where, w, sizeof(crumb.where) - 1);
  crumb.where[sizeof(crumb.where) - 1] = 0;
}
extern SPIClass sdSPI;

// Частота SD снижена до 10 МГц: на макетке с длинными проводами 20 МГц давали сбои чтения
// sdHz можно временно снизить (приём файлов после сбоя записи), затем вернуть SD_HZ_NORMAL
#define SD_HZ_NORMAL 10000000
#define SD_HZ_SAFE    4000000
inline uint32_t sdHz = SD_HZ_NORMAL;
inline bool sdMount() {
  if (sdOk) return true;
  sdSPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = SD.begin(PIN_SD_CS, sdSPI, sdHz);
  if (!sdOk) sdOk = SD.begin(PIN_SD_CS, sdSPI, 4000000);
  return sdOk;
}

// Переподключить карту после сбоя чтения
inline bool sdRemount() {
  SD.end();
  sdOk = false;
  delay(30);
  for (int i = 0; i < 3 && !sdMount(); i++) delay(100);
  return sdOk;
}

// Открыть файл для чтения: несколько попыток, при неудаче - переподключение карты
inline File sdOpenRead(const String& path) {
  for (int attempt = 0; attempt < 3; attempt++) {
    File f = SD.open(path, FILE_READ);
    if (f) return f;
    delay(20);
  }
  sdRemount();
  return SD.open(path, FILE_READ);
}

inline int batteryPercent() {
  static uint32_t last = 0;
  static int pct = -1;
  if (pct < 0 || millis() - last > 5000) {
    last = millis();
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BATTERY);
    mv = mv / 8 * 2;  // делитель 1:2 (10к + 10к)
    // Разряд Li-ion нелинейный: от 4.2 до 3.9 В уходит треть заряда, а основная часть - в районе 3.7-3.8 В.
    // Прямая 3.15-4.10 В завышала заряд в 2-3 раза на половине батареи, поэтому считаем по типовой кривой.
    static const uint16_t V[] = {3300, 3500, 3610, 3690, 3730, 3770, 3810, 3860, 3920, 4000, 4100, 4180};
    static const uint8_t  P[] = {   0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100};
    int p;
    if (mv <= V[0]) p = 0;
    else if (mv >= V[11]) p = 100;
    else {
      int i = 1;
      while (mv > V[i]) i++;
      p = P[i - 1] + (int)((mv - V[i - 1]) * (P[i] - P[i - 1]) / (V[i] - V[i - 1]));
    }
    pct = constrain(p, 0, 100);
  }
  return pct;
}
