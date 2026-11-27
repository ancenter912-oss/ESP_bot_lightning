#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h! Copy secrets.h.example to secrets.h and set VILLAGE_TOKEN and OTA URLs."
#endif

// ============================================================================
// Firmware & Backend Constants (updated via OTA, hidden from end-user UI)
// ============================================================================
static const char* FW_VERSION = "1.0.0";
static const char* VILLAGE_URL = "https://korotych-power-bot.onrender.com/api/meter";
static const char* SENSOR_SOURCE = "ESP8266-Opto";

// ============================================================================
// Hardware Pins & Polarity
// ============================================================================
static const uint8_t PIN_OPTO = 14;          // D5 / GPIO14 (INPUT_PULLUP)
static const uint8_t OPTO_ACTIVE_LEVEL = LOW;

static const uint8_t PIN_RESET_CFG = 12;     // D6 / GPIO12 (INPUT_PULLUP)
static const uint8_t RESET_ACTIVE_LEVEL = LOW;

static const uint8_t PIN_LED = LED_BUILTIN;  // D4 / GPIO2 on D1 Mini / NodeMCU
static const uint8_t LED_ACTIVE_LEVEL = LOW;

// ============================================================================
// Timing Constants
// ============================================================================
static const uint32_t SAMPLE_WINDOW_MS = 60;          // 3 periods of 50 Hz
static const uint16_t OPTO_MIN_ACTIVE_SAMPLES = 3;    // noise rejection threshold
static const uint32_t DEBOUNCE_MS = 3000;             // 3 sec status confirmation
static const uint32_t RESET_HOLD_MS = 5000;           // 5 sec hold to open config portal
static const uint32_t PORTAL_TIMEOUT_SEC = 300;       // 5 min portal timeout

static const uint32_t HEARTBEAT_MS = 20000;           // 20 sec village heartbeat
static const uint32_t RETRY_SEND_MS = 5000;           // 5 sec retry on network error
static const uint32_t HTTP_TIMEOUT_MS = 10000;        // 10 sec HTTP timeout

static const uint32_t OTA_INITIAL_DELAY_MS = 60000;   // 60 sec after boot
static const uint32_t OTA_INTERVAL_MS = 21600000UL;   // 6 hours

static const char* CONFIG_PATH = "/config.json";
static const char* AP_NAME = "Korotych-Light-Setup";

// ============================================================================
// Dark Mobile Captive Portal CSS (Russian UI)
// ============================================================================
static const char CUSTOM_CSS[] PROGMEM = R"rawliteral(
<style>
  :root {
    --bg: #0f172a;
    --card: #1e293b;
    --input: #0b1220;
    --border: #334155;
    --text: #f1f5f9;
    --muted: #94a3b8;
    --accent: #f59e0b;
    --accent-hover: #d97706;
    --ok: #22c55e;
  }
  * { box-sizing: border-box; }
  body {
    background: var(--bg);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    margin: 0;
    padding: 16px;
  }
  .wrap {
    max-width: 420px;
    margin: 0 auto;
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 16px;
    padding: 20px;
    box-shadow: 0 12px 32px rgba(0, 0, 0, 0.45);
  }
  h1, h2, h3 {
    color: var(--accent);
    text-align: center;
    margin-top: 0;
  }
  .sec-title {
    margin: 18px 0 8px 0;
    padding-top: 12px;
    border-top: 1px solid var(--border);
    color: var(--accent);
    font-size: 14px;
    font-weight: 700;
    letter-spacing: 0.2px;
  }
  .hint {
    color: var(--muted);
    font-size: 12px;
    margin: 2px 0 10px 0;
    line-height: 1.4;
  }
  label {
    display: block;
    color: var(--muted);
    font-size: 13px;
    margin: 10px 0 4px 0;
  }
  input, select {
    width: 100%;
    padding: 12px;
    border-radius: 10px;
    border: 1px solid var(--border);
    background: var(--input);
    color: var(--text);
    font-size: 15px;
  }
  input:focus {
    outline: none;
    border-color: var(--accent);
  }
  button {
    width: 100%;
    padding: 13px;
    margin-top: 12px;
    border: none;
    border-radius: 10px;
    background: var(--accent);
    color: #0f172a;
    font-weight: 700;
    font-size: 15px;
    cursor: pointer;
  }
  button:hover { background: var(--accent-hover); }
  a { color: var(--accent); text-decoration: none; }
  div.msg {
    background: #172554;
    border: 1px solid #1d4ed8;
    border-radius: 10px;
    padding: 10px;
    color: #dbeafe;
  }
</style>
)rawliteral";

// ============================================================================
// Persistent User Config (LittleFS /config.json — survives OTA)
// ============================================================================
struct UserConfig {
  char sensor_label[64] = "";
  char tg_bot_token[80] = "";
  char tg_chat_id[32] = "";
};

UserConfig g_cfg;
char g_sensorId[16] = "";
bool g_shouldSaveConfig = false;

// ============================================================================
// Runtime State
// ============================================================================
bool g_mainsState = false;
bool g_candidateState = false;
uint32_t g_candidateSinceMs = 0;

bool g_villagePendingSend = true;
bool g_villageLastFailed = false;
uint32_t g_lastVillageAttemptMs = 0;

uint32_t g_bootMs = 0;
bool g_otaInitialDone = false;
uint32_t g_lastOtaCheckMs = 0;

bool g_resetHolding = false;
uint32_t g_resetHoldStartMs = 0;

// ============================================================================
// Helpers
// ============================================================================
void setLed(bool on) {
  digitalWrite(PIN_LED, on ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL);
}

void blinkLed(uint8_t count, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < count; i++) {
    setLed(true);
    delay(onMs);
    setLed(false);
    delay(offMs);
    yield();
  }
  setLed(g_mainsState);
}

String escapeHtml(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else out += c;
  }
  return out;
}

bool isTelegramConfigured() {
  return strlen(g_cfg.tg_bot_token) >= 20 && strlen(g_cfg.tg_chat_id) >= 3;
}

// ============================================================================
// 50 Hz Mains Sampling (60 ms window = 3 full AC cycles)
// ============================================================================
bool sampleMains50Hz() {
  uint32_t startMs = millis();
  uint32_t activeHits = 0;

  while ((uint32_t)(millis() - startMs) < SAMPLE_WINDOW_MS) {
    if (digitalRead(PIN_OPTO) == OPTO_ACTIVE_LEVEL) {
      activeHits++;
    }
    delay(1);
    yield();
  }

  return activeHits >= OPTO_MIN_ACTIVE_SAMPLES;
}

// ============================================================================
// LittleFS Config Storage
// ============================================================================
bool loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println(F("[FS] Mount failed, formatting..."));
    if (!LittleFS.format() || !LittleFS.begin()) {
      Serial.println(F("[FS] Format/mount failed"));
      return false;
    }
  }

  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println(F("[FS] /config.json not found, using defaults"));
    return false;
  }

  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    Serial.println(F("[FS] Failed to open /config.json"));
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    Serial.printf("[FS] JSON parse error: %s\n", err.c_str());
    return false;
  }

  strlcpy(g_cfg.sensor_label, doc["sensor_label"] | "", sizeof(g_cfg.sensor_label));
  strlcpy(g_cfg.tg_bot_token, doc["tg_bot_token"] | "", sizeof(g_cfg.tg_bot_token));
  strlcpy(g_cfg.tg_chat_id, doc["tg_chat_id"] | "", sizeof(g_cfg.tg_chat_id));

  Serial.printf("[FS] Loaded config: label='%s', tg=%s\n",
                g_cfg.sensor_label,
                isTelegramConfigured() ? "enabled" : "disabled");
  return true;
}

bool saveConfig() {
  JsonDocument doc;
  doc["sensor_label"] = g_cfg.sensor_label;
  doc["tg_bot_token"] = g_cfg.tg_bot_token;
  doc["tg_chat_id"] = g_cfg.tg_chat_id;

  File file = LittleFS.open(CONFIG_PATH, "w");
  if (!file) {
    Serial.println(F("[FS] Failed to open /config.json for writing"));
    return false;
  }

  if (serializeJson(doc, file) == 0) {
    Serial.println(F("[FS] Failed to serialize config"));
    file.close();
    return false;
  }

  file.close();
  Serial.println(F("[FS] Saved /config.json"));
  return true;
}

// ============================================================================
// WiFiManager Captive Portal
// ============================================================================
void onSaveConfigCallback() {
  g_shouldSaveConfig = true;
}

void runConfigPortal(bool forcePortal) {
  String customHead = FPSTR(CUSTOM_CSS);
  WiFiManager wm;
  g_shouldSaveConfig = false;

  wm.setSaveConfigCallback(onSaveConfigCallback);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SEC);
  wm.setConnectTimeout(25);
  wm.setTitle("⚡ Пеленгатор света · Коротич");
  wm.setCustomHeadElement(customHead.c_str());

  String headerHtml = String(F("<div class='hint'>ID датчика: <b>")) + g_sensorId +
                      F("</b> · Прошивка: <b>v") + FW_VERSION + F("</b></div>");
  WiFiManagerParameter pHeader(headerHtml.c_str());

  WiFiManagerParameter pSec1(
      "<div class='sec-title'>1. Расположение датчика</div>"
      "<div class='hint'>Укажите улицу или ориентир для отображения на панели посёлка.</div>");
  WiFiManagerParameter pLabel(
      "sensor_label",
      "Улица / точка (например: ул. Центральная)",
      g_cfg.sensor_label,
      sizeof(g_cfg.sensor_label) - 1);

  WiFiManagerParameter pSec2(
      "<div class='sec-title'>2. Личный Telegram-бот (по желанию)</div>"
      "<div class='hint'>Оставьте пустым, если личные уведомления не нужны.</div>");
  WiFiManagerParameter pTgToken(
      "tg_bot_token",
      "Токен личного бота (BotFather)",
      g_cfg.tg_bot_token,
      sizeof(g_cfg.tg_bot_token) - 1);
  WiFiManagerParameter pTgChat(
      "tg_chat_id",
      "Ваш Chat ID (например: 123456789)",
      g_cfg.tg_chat_id,
      sizeof(g_cfg.tg_chat_id) - 1);

  wm.addParameter(&pHeader);
  wm.addParameter(&pSec1);
  wm.addParameter(&pLabel);
  wm.addParameter(&pSec2);
  wm.addParameter(&pTgToken);
  wm.addParameter(&pTgChat);

  blinkLed(3, 120, 120);

  bool connected = false;
  if (forcePortal) {
    Serial.println(F("[WiFi] Starting forced config portal..."));
    connected = wm.startConfigPortal(AP_NAME);
  } else {
    Serial.println(F("[WiFi] Running autoConnect..."));
    connected = wm.autoConnect(AP_NAME);
  }

  if (g_shouldSaveConfig) {
    strlcpy(g_cfg.sensor_label, pLabel.getValue(), sizeof(g_cfg.sensor_label));
    strlcpy(g_cfg.tg_bot_token, pTgToken.getValue(), sizeof(g_cfg.tg_bot_token));
    strlcpy(g_cfg.tg_chat_id, pTgChat.getValue(), sizeof(g_cfg.tg_chat_id));
    saveConfig();
  }

  if (!connected) {
    Serial.println(F("[WiFi] Portal timed out / not connected, rebooting..."));
    delay(1500);
    ESP.restart();
  }

  Serial.printf("[WiFi] Connected! IP: %s, RSSI: %d dBm\n",
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
}

// ============================================================================
// Channel 1: Village Bot API (POST /api/meter)
// ============================================================================
bool sendToVillageBot(bool online) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[Village] Skip send: WiFi disconnected"));
    return false;
  }

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 512);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(client, VILLAGE_URL)) {
    Serial.println(F("[Village] http.begin failed"));
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Meter-Token", VILLAGE_TOKEN);

  JsonDocument doc;
  doc["online"] = online;
  doc["sensor_id"] = g_sensorId;
  doc["sensor_label"] = strlen(g_cfg.sensor_label) > 0 ? g_cfg.sensor_label : g_sensorId;
  doc["source"] = SENSOR_SOURCE;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  String resp = (code > 0) ? http.getString() : http.errorToString(code);
  http.end();

  bool ok = (code >= 200 && code < 300);
  Serial.printf("[Village] POST online=%s -> HTTP %d (%s)\n",
                online ? "true" : "false",
                code,
                resp.c_str());
  return ok;
}

// ============================================================================
// Channel 2: Personal Telegram Bot (optional)
// ============================================================================
bool sendTelegramMessage(const String& htmlText) {
  if (!isTelegramConfigured()) {
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[TG] Skip send: WiFi disconnected"));
    return false;
  }

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 512);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  String url = String(F("https://api.telegram.org/bot")) + g_cfg.tg_bot_token + F("/sendMessage");
  if (!http.begin(client, url)) {
    Serial.println(F("[TG] http.begin failed"));
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["chat_id"] = g_cfg.tg_chat_id;
  doc["text"] = htmlText;
  doc["parse_mode"] = "HTML";
  doc["disable_web_page_preview"] = true;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  http.end();

  bool ok = (code >= 200 && code < 300);
  Serial.printf("[TG] sendMessage -> HTTP %d\n", code);
  return ok;
}

void notifyPersonalBoot(bool online) {
  if (!isTelegramConfigured()) return;

  String label = strlen(g_cfg.sensor_label) > 0 ? escapeHtml(g_cfg.sensor_label) : String(g_sensorId);
  String msg = F("🔌 <b>Датчик запущен</b>\n");
  msg += F("📍 Точка: <b>") + label + F("</b> (<code>") + g_sensorId + F("</code>)\n");
  msg += F("⚡ Сеть 220 В: <b>") + String(online ? "ЕСТЬ" : "НЕТ") + F("</b>\n");
  msg += F("📶 Wi-Fi RSSI: ") + String(WiFi.RSSI()) + F(" dBm\n");
  msg += F("📦 Прошивка: <code>v");
  msg += FW_VERSION;
  msg += F("</code>");

  sendTelegramMessage(msg);
}

void notifyPersonalStatusChange(bool online) {
  if (!isTelegramConfigured()) return;

  String label = strlen(g_cfg.sensor_label) > 0 ? escapeHtml(g_cfg.sensor_label) : String(g_sensorId);
  String msg;
  if (online) {
    msg = F("⚡ <b>Свет ПОЯВИЛСЯ</b>\n");
  } else {
    msg = F("🌑 <b>Свет ПРОПАЛ</b>\n");
  }
  msg += F("📍 Точка: <b>") + label + F("</b> (<code>") + g_sensorId + F("</code>)");

  sendTelegramMessage(msg);
}

// ============================================================================
// Pull-OTA Update from GitHub (ESP8266httpUpdate)
// ============================================================================
void onOtaProgress(int cur, int total) {
  static uint32_t lastToggle = 0;
  static bool ledState = false;
  uint32_t now = millis();
  if (now - lastToggle >= 120) {
    lastToggle = now;
    ledState = !ledState;
    setLed(ledState);
  }
  (void)cur;
  (void)total;
}

void checkAndApplyOta() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[OTA] Skip: WiFi disconnected"));
    return;
  }

  Serial.printf("[OTA] Checking %s (current v%s, freeHeap=%u)\n",
                OTA_VERSION_URL, FW_VERSION, ESP.getFreeHeap());

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 512);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, OTA_VERSION_URL)) {
    Serial.println(F("[OTA] Version check http.begin failed"));
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Version check HTTP %d\n", code);
    http.end();
    return;
  }

  String remoteVer = http.getString();
  http.end();
  remoteVer.trim();

  if (remoteVer.isEmpty() || remoteVer.length() > 24 || remoteVer.indexOf('<') >= 0) {
    Serial.printf("[OTA] Invalid version response: '%s'\n", remoteVer.c_str());
    return;
  }

  if (remoteVer == FW_VERSION) {
    Serial.printf("[OTA] Up to date (v%s)\n", FW_VERSION);
    return;
  }

  Serial.printf("[OTA] New version found: v%s -> v%s\n", FW_VERSION, remoteVer.c_str());

  if (isTelegramConfigured()) {
    String label = strlen(g_cfg.sensor_label) > 0 ? escapeHtml(g_cfg.sensor_label) : String(g_sensorId);
    String msg = F("🔄 <b>Обновление прошивки (OTA)</b>\n");
    msg += F("📍 Точка: <b>") + label + F("</b>\n");
    msg += F("📦 Версия: <code>v");
    msg += FW_VERSION;
    msg += F("</code> → <code>v");
    msg += escapeHtml(remoteVer);
    msg += F("</code>");
    sendTelegramMessage(msg);
  }

  BearSSL::WiFiClientSecure otaClient;
  otaClient.setInsecure();
  otaClient.setBufferSizes(2048, 512);

  ESPhttpUpdate.setLedPin(PIN_LED, LED_ACTIVE_LEVEL);
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.onProgress(onOtaProgress);
  ESPhttpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = ESPhttpUpdate.update(otaClient, OTA_BIN_URL);
  if (ret == HTTP_UPDATE_FAILED) {
    Serial.printf("[OTA] Update failed (%d): %s\n",
                  ESPhttpUpdate.getLastError(),
                  ESPhttpUpdate.getLastErrorString().c_str());
    setLed(g_mainsState);

    if (isTelegramConfigured()) {
      String msg = F("⚠️ <b>Ошибка OTA-обновления</b>\n");
      msg += F("Код: <code>") + String(ESPhttpUpdate.getLastError()) + F("</code>\n");
      msg += escapeHtml(ESPhttpUpdate.getLastErrorString());
      sendTelegramMessage(msg);
    }
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    Serial.println(F("[OTA] No updates"));
    setLed(g_mainsState);
  }
}

// ============================================================================
// Reset Config Button Check (5 sec hold on GND)
// ============================================================================
void handleResetButton() {
  bool pressed = (digitalRead(PIN_RESET_CFG) == RESET_ACTIVE_LEVEL);
  uint32_t now = millis();

  if (pressed) {
    if (!g_resetHolding) {
      g_resetHolding = true;
      g_resetHoldStartMs = now;
      Serial.println(F("[CFG] Reset button pressed..."));
    } else if ((uint32_t)(now - g_resetHoldStartMs) >= RESET_HOLD_MS) {
      g_resetHolding = false;
      Serial.println(F("[CFG] 5s hold detected -> opening config portal"));
      runConfigPortal(true);
      g_villagePendingSend = true;
    }
  } else {
    g_resetHolding = false;
  }
}

// ============================================================================
// Setup & Main Loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.printf("\n=== Korotych ESP8266 Opto Sensor v%s ===\n", FW_VERSION);

  pinMode(PIN_OPTO, INPUT_PULLUP);
  pinMode(PIN_RESET_CFG, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  setLed(false);

  snprintf(g_sensorId, sizeof(g_sensorId), "esp_%06x", ESP.getChipId());
  Serial.printf("[SYS] Sensor ID: %s\n", g_sensorId);

  loadConfig();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  bool forcePortalOnBoot = (digitalRead(PIN_RESET_CFG) == RESET_ACTIVE_LEVEL);
  if (forcePortalOnBoot) {
    Serial.println(F("[CFG] Reset pin held LOW at boot -> forcing config portal"));
  }
  runConfigPortal(forcePortalOnBoot);

  // Initial mains measurement
  g_mainsState = sampleMains50Hz();
  g_candidateState = g_mainsState;
  g_candidateSinceMs = millis();
  setLed(g_mainsState);

  Serial.printf("[MAINS] Initial state: %s\n", g_mainsState ? "220V ON" : "220V OFF");

  // Initial notifications
  g_villageLastFailed = !sendToVillageBot(g_mainsState);
  g_villagePendingSend = false;
  g_lastVillageAttemptMs = millis();

  notifyPersonalBoot(g_mainsState);

  g_bootMs = millis();
  g_otaInitialDone = false;
  g_lastOtaCheckMs = g_bootMs;
}

void loop() {
  handleResetButton();

  // 1. Sample 50 Hz mains and debounce for DEBOUNCE_MS (3 sec)
  bool rawMains = sampleMains50Hz();
  uint32_t now = millis();

  if (rawMains != g_candidateState) {
    g_candidateState = rawMains;
    g_candidateSinceMs = now;
  } else if (g_candidateState != g_mainsState &&
             (uint32_t)(now - g_candidateSinceMs) >= DEBOUNCE_MS) {
    g_mainsState = g_candidateState;
    setLed(g_mainsState);
    Serial.printf("[MAINS] Confirmed change -> %s\n", g_mainsState ? "220V ON" : "220V OFF");

    g_villagePendingSend = true;
    notifyPersonalStatusChange(g_mainsState);
  }

  // 2. Village bot immediate send / retry (5s) / heartbeat (20s)
  now = millis();
  uint32_t interval = g_villageLastFailed ? RETRY_SEND_MS : HEARTBEAT_MS;
  if (g_villagePendingSend || (uint32_t)(now - g_lastVillageAttemptMs) >= interval) {
    bool ok = sendToVillageBot(g_mainsState);
    g_villageLastFailed = !ok;
    g_villagePendingSend = false;
    g_lastVillageAttemptMs = millis();
  }

  // 3. Pull-OTA check (60s after boot, then every 6 hours)
  now = millis();
  if (!g_otaInitialDone && (uint32_t)(now - g_bootMs) >= OTA_INITIAL_DELAY_MS) {
    g_otaInitialDone = true;
    g_lastOtaCheckMs = now;
    checkAndApplyOta();
  } else if (g_otaInitialDone && (uint32_t)(now - g_lastOtaCheckMs) >= OTA_INTERVAL_MS) {
    g_lastOtaCheckMs = now;
    checkAndApplyOta();
  }

  yield();
}
