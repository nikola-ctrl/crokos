# CrokOS

Прошивка «карманного мини-компьютера» для самодельной консоли **CrokPocket** на **ESP32-S3 (N16R8)** с экраном **ST7789 320×240**.
Работает отдельно или рядом с эмулятором Retro-Go на той же флешке.

*English: CrokOS is a pocket "mini-PC" firmware for a DIY ESP32-S3 (N16R8) handheld with an ST7789 320×240 LCD: file manager, UTF-8 text editor with RU/EN on-screen keyboard, image gallery, scientific calculator, MP3/WAV/AVI player, AI chat (Gemini / OpenRouter / any OpenAI-compatible API) and Wi-Fi file upload. Comes with an Android companion app.*

> Схемы подключения, фотографии и 3D-модели корпуса будут добавлены позже в папку [`docs/`](docs/).

## Возможности

| Приложение | Что умеет |
|---|---|
| **Файлы** | Файловый менеджер SD-карты: новые файлы и папки, переименование, удаление, открытие файлов в нужном приложении |
| **Редактор** | Текстовые файлы в UTF-8 до 512 КБ, экранная клавиатура RU/EN |
| **Галерея** | JPG, PNG, BMP, WebP с подгонкой под экран |
| **Калькулятор** | Инженерный: тригонометрия (градусы и радианы), логарифмы, корни, степени, скобки |
| **Плеер** | MP3 и WAV, видео AVI (MJPEG + PCM) со звуком через I2S-усилитель |
| **Нейросеть** | Чат с Gemini, OpenRouter или любым OpenAI-совместимым API; история чатов на SD |
| **Wi-Fi** | Консоль раздаёт открытую сеть `CrokPocket`, файлы загружаются через браузер по адресу `192.168.4.1` |
| **Retro-Go** | Переход в эмулятор, если он прошит на ту же флешку |

Устойчивая запись на SD: при сбоях карта переподключается на пониженной частоте, а загрузка продолжается с места остановки. Файлы загружаются «атомарно», через временный `.part`.

## Железо

| Узел | Выводы ESP32-S3 |
|---|---|
| Экран ST7789 (SPI) | MOSI 12, SCLK 48, CS 14, DC 47, RST 3, BL 39 |
| SD-карта (SPI) | MISO 9, MOSI 11, CLK 13, CS 10 |
| Кнопки (на GND, внутренняя подтяжка) | UP 7, RIGHT 6, DOWN 46, LEFT 45, SELECT 16, START 17, MENU 18, OPTION 8, A 15, B 5 |
| Аккумулятор | GPIO 4 через делитель 1:2 |
| Светодиод | GPIO 38 |
| Звук: I2S-усилитель MAX98357A | BCLK 41, LRC 42, DIN 40 |
| Микрофон INMP441 (необязательно) | SCK 41, WS 42, SD 40, L/R на GND |

Модуль ESP32-S3 нужен с 16 МБ флеш-памяти и 8 МБ PSRAM (OPI), например ESP32-S3-DevKitC-1 N16R8.
Выводы меняются в [`CrokOS/hw.h`](CrokOS/hw.h).

Советы по SD-карте: провода до карты делайте покороче, а у её питания поставьте конденсаторы 10–100 мкФ и 0,1 мкФ.

## Сборка

Нужны [arduino-cli](https://arduino.github.io/arduino-cli/) или Arduino IDE, ядро **esp32 3.3.x** (проверено на 3.3.12) и библиотека **LovyanGFX**.
Библиотеки ArduinoJson, WebPDec и minimp3 лежат в папке [`libraries/`](libraries/).

```bash
arduino-cli core install esp32:esp32@3.3.12
arduino-cli lib install LovyanGFX
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" \
  --libraries libraries CrokOS
```

В Arduino IDE: плата **ESP32S3 Dev Module**, Flash Size **16MB**, PSRAM **OPI PSRAM**, Partition Scheme **16M Flash (3MB APP/9.9MB FATFS)**, USB CDC On Boot **Enabled**. Папку `libraries/` скопируйте в `~/Arduino/libraries`.

## Прошивка

**Только CrokOS:** соберите и загрузите обычным способом (`arduino-cli upload` или кнопка «Загрузить» в IDE).

**Вместе с Retro-Go:** готовый файл [`bin/crokos_0x400000.bin`](bin/) прошивается по адресу `0x400000` поверх образа Retro-Go, в котором есть раздел `CROKPOCKET` типа factory (сам такой образ Retro-Go в репозиторий пока не входит):

```bash
esptool.py --chip esp32s3 write_flash 0x400000 bin/crokos_0x400000.bin
```

Раскладка флешки в этом варианте:

| Адрес | Раздел |
|---|---|
| `0x010000` | launcher (Retro-Go) |
| `0x110000` | retro-core (NES, GB, SMS и др.) |
| `0x210000` | prboom-go (Doom) |
| `0x2E0000` | gwenesis (Mega Drive) |
| `0x400000` | CROKPOCKET — CrokOS |

## Настройка нейросети

Скопируйте [`crokos.ini.template`](crokos.ini.template) на SD-карту под именем `crokos.ini` и впишите свои значения: сеть Wi-Fi, провайдера, ключ API и способ подключения. Всё это можно задать и прямо на консоли: «Нейросеть» → START.

- Ключи и пароли хранятся только на карте памяти консоли. Веб-загрузчик не отдаёт `crokos.ini` и скрытые файлы.
- Провайдеры: `gemini`, `openrouter`, `api` (любой OpenAI-совместимый `/v1/chat/completions`).
- Если сервис недоступен напрямую, есть режимы `relay` (свой сервер) и `proxy` (Cloudflare Worker из [`proxy/worker.js`](proxy/worker.js), только для Gemini). Адрес своего воркера не публикуйте.

**Никогда не выкладывайте свой `crokos.ini` в открытый доступ.**

## Приложение для Android

[`app/CrokPocketApp`](app/CrokPocketApp) — компаньон для Android 10+. Он сам подключается к сети консоли, загружает и скачивает файлы, переводит видео, музыку и картинки в подходящие форматы (средствами Android, без ffmpeg), раскладывает игры по папкам Retro-Go и собирает `crokos.ini`. Подробнее — в [README приложения](app/CrokPocketApp/README.md).
Сборка: открыть папку в Android Studio → Build → Build APK(s). Сторонних библиотек нет.

## Управление

Подсказки по кнопкам выводятся внизу экрана в каждом приложении.

| Кнопка | Действие |
|---|---|
| Крестовина | Навигация |
| A / B | Выбрать / назад |
| START | Главное действие приложения: в «Файлах» — меню действий, в редакторе — сохранить, в калькуляторе — посчитать, в «Нейросети» — настройки |
| SELECT, OPTION | Дополнительные действия, свои в каждом приложении |
| MENU | Выход в главное меню |
| START + SELECT при включении | Сброс настроек экрана (поворот, инверсия, порядок цветов) |

## Лицензии

Код CrokOS и приложения распространяется по лицензии MIT, см. [`LICENSE`](LICENSE).
Сторонние библиотеки в `libraries/` — под своими лицензиями: ArduinoJson (MIT), libwebp/WebPDec (BSD), minimp3 (CC0). Шрифты DejaVu — под лицензией DejaVu Fonts (свободная).
