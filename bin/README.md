# Готовая прошивка

`crokos_0x400000.bin` — CrokOS версии 2026.10.18, собрана с параметрами из основного README.

Файл рассчитан на прошивку по адресу `0x400000` рядом с Retro-Go (раздел `CROKPOCKET`):

```bash
esptool.py --chip esp32s3 write_flash 0x400000 crokos_0x400000.bin
```

Если вы ставите только CrokOS без Retro-Go, проще собрать прошивку из исходников и загрузить её из Arduino IDE.
