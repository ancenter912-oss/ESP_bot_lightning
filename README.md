# ESP_bot_lightning — ESP8266-пеленгаторы света (Коротич)

Отдельный чистовой репозиторий прошивки и Pull-OTA для датчиков наличия 220 В на базе **ESP8266** (Wemos D1 Mini / NodeMCU) для интеграции с ботом посёлка `@KorotychLightbot` (`https://korotych-power-bot.onrender.com/api/meter`) и опциональным личным Telegram-ботом владельца.

## Структура проекта

```text
esp8266/
├── korotych_esp8266_opto/
│   ├── korotych_esp8266_opto.ino   # Основной скетч
│   ├── secrets.h.example           # Шаблон токена и OTA URL (скопировать в secrets.h)
│   └── secrets.h                   # Локальные секреты (в .gitignore, не коммитить!)
├── version.txt                     # Текущая версия прошивки для Pull-OTA (например: 1.0.0)
└── firmware.bin                    # Скомпилированный бинарник для Pull-OTA (добавляется при релизе)
```

## Подключение железа

- **Питание ESP8266:** от ИБП роутера (5 В).
- **Модуль оптопары 220 В (PC817 / LTV-814):** вход 220 В — в розетку **до ИБП**.

| Сигнал | Пин ESP8266 | Маркировка D1 Mini / NodeMCU | Режим |
| :--- | :--- | :--- | :--- |
| Выход оптопары (220 В) | `GPIO14` | `D5` | `INPUT_PULLUP` (активный `LOW`) |
| Кнопка сброса / портала Wi-Fi | `GPIO12` | `D6` | `INPUT_PULLUP` (на `GND` ≥ 5 сек или при старте) |
| Индикатор 220 В / OTA | `LED_BUILTIN` | `D4` / `GPIO2` | Горит при 220 В, мигает при OTA |

## Сборка и прошивка (Arduino IDE)

1. Скопируйте `esp8266/korotych_esp8266_opto/secrets.h.example` → `esp8266/korotych_esp8266_opto/secrets.h` и впишите реальный `VILLAGE_TOKEN` (и проверьте `OTA_VERSION_URL` / `OTA_BIN_URL`).
2. Установите ядро **ESP8266 Boards (3.1.2+)** и библиотеки через Library Manager:
   - `WiFiManager` (by tzapu, `2.0.17+`)
   - `ArduinoJson` (by Benoit Blanchon, `7.x`)
3. Настройки платы в Arduino IDE:
   - **Board:** `LOLIN(WEMOS) D1 R2 & mini` (или `NodeMCU 1.0 (ESP-12E Module)`)
   - **Flash Size:** `4MB (FS:1MB OTA:~1019KB)` — обязательно для работы LittleFS + OTA.
4. Откройте `esp8266/korotych_esp8266_opto/korotych_esp8266_opto.ino` и прошейте по USB.

## Первичная настройка датчика

1. При первом запуске (или при удержании `D6` на `GND` 5 секунд / при подаче питания) ESP8266 поднимет точку доступа **`Korotych-Light-Setup`** (таймаут 300 сек).
2. Подключитесь с телефона, выберите Wi-Fi и заполните:
   - **1. Расположение датчика** (`sensor_label`, например `ул. Центральная`).
   - **2. Личный Telegram-бот** (`tg_bot_token`, `tg_chat_id`) — по желанию.
3. Настройки сохраняются в `LittleFS` (`/config.json`) и **не стираются при OTA-обновлениях**. `sensor_id` генерируется автоматически из `ESP.getChipId()` в формате `esp_%06x`.

## Выпуск Pull-OTA обновления

1. Увеличьте `FW_VERSION` в `korotych_esp8266_opto.ino` (например, `1.0.1`).
2. В Arduino IDE выполните **Sketch → Export Compiled Binary** и скопируйте `.bin` в `esp8266/firmware.bin`.
3. Обновите `esp8266/version.txt` до `1.0.1` и запушьте изменения в `main`.
4. Датчики проверят `esp8266/version.txt` (через 60 сек после старта и далее каждые 6 часов) и обновятся автоматически.
