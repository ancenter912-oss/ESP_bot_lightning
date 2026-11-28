#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WebServer.h>
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
static const char* FW_VERSION = "1.0.2";
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
// Timing, Watchdog & Diagnostics Constants
// ============================================================================
static const uint32_t SAMPLE_WINDOW_MS = 60;               // 3 periods of 50 Hz
static const uint16_t OPTO_MIN_ACTIVE_SAMPLES = 3;         // noise rejection threshold
static const uint32_t DEBOUNCE_MS = 3000;                  // 3 sec status confirmation
static const uint32_t RESET_HOLD_MS = 5000;                // 5 sec hold to open config portal
static const uint32_t PORTAL_TIMEOUT_SEC = 300;            // 5 min portal timeout

static const uint32_t HEARTBEAT_MS = 90000;                // 90 sec village heartbeat (6x margin under 600s)
static const uint32_t RETRY_SEND_MS = 15000;               // 15 sec retry on network error
static const uint32_t HTTP_TIMEOUT_MS = 10000;             // 10 sec HTTP timeout

static const uint32_t MIN_FREE_HEAP_BYTES = 12000;         // Software WDT heap floor (12 KB)
static const uint32_t MIN_OTA_FREE_HEAP_BYTES = 20000;     // Minimum heap before starting OTA TLS
static const uint32_t VILLAGE_STALE_RESTART_MS = 600000UL; // 10 min without village OK while WiFi up

static const uint32_t OTA_INITIAL_DELAY_MS = 60000;        // 60 sec after boot
static const uint32_t OTA_INTERVAL_MS = 21600000UL;        // 6 hours

static const char* CONFIG_PATH = "/config.json";
static const char* DIAG_LOG_PATH = "/diag.log";
static const char* DIAG_PREV_PATH = "/diag.prev.log";
static const size_t DIAG_LOG_MAX_BYTES = 24576;            // 24 KB x 2 rotating files in 1 MB LittleFS
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
bool g_fsMounted = false;

// Single reusable BearSSL client & lightweight diagnostic HTTP server
BearSSL::WiFiClientSecure g_tlsClient;
ESP8266WebServer g_diagServer(80);
bool g_diagServerRunning = false;

// Wi-Fi event handlers (must remain global to stay subscribed)
WiFiEventHandler g_wifiDiscHandler;
WiFiEventHandler g_wifiGotIpHandler;

// ============================================================================
// Runtime & Diagnostics State
// ============================================================================
bool g_mainsState = false;
bool g_candidateState = false;
uint32_t g_candidateSinceMs = 0;

bool g_villagePendingSend = true;
bool g_villageLastFailed = false;
uint32_t g_lastVillageAttemptMs = 0;
uint32_t g_lastVillageSuccessMs = 0;

uint32_t g_bootMs = 0;
bool g_otaInitialDone = false;
uint32_t g_lastOtaCheckMs = 0;

bool g_resetHolding = false;
uint32_t g_resetHoldStartMs = 0;

// Telemetry counters & last error details
char g_bootResetReason[40] = "";
char g_bootResetInfo[128] = "";
int g_lastVillageHttpCode = 0;
char g_lastVillageSslErr[64] = "";
uint32_t g_villageOkCount = 0;
uint32_t g_villageFailCount = 0;

int g_lastTgHttpCode = 0;
char g_lastTgSslErr[64] = "";
uint32_t g_tgOkCount = 0;
uint32_t g_tgFailCount = 0;

uint32_t g_minFreeHeapSeen = 0xFFFFFFFFUL;
uint32_t g_minMaxBlockSeen = 0xFFFFFFFFUL;
uint8_t g_maxFragSeen = 0;

volatile bool g_wifiDiscPending = false;
volatile uint8_t g_lastWifiDiscReason = 0;
volatile uint32_t g_wifiDiscCount = 0;
volatile bool g_wifiGotIpPending = false;

// ============================================================================
// Helpers & Persistent Diagnostic Logger (LittleFS rotating /diag.log)
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

void updateHeapStats() {
  uint32_t h = ESP.getFreeHeap();
  uint32_t b = ESP.getMaxFreeBlockSize();
  uint8_t f = ESP.getHeapFragmentation();
  if (h < g_minFreeHeapSeen) g_minFreeHeapSeen = h;
  if (b < g_minMaxBlockSeen) g_minMaxBlockSeen = b;
  if (f > g_maxFragSeen) g_maxFragSeen = f;
}

void diagLog(const char* tag, const char* fmt, ...) {
  updateHeapStats();

  char msg[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  char line[240];
  uint32_t upSec = millis() / 1000UL;
  snprintf(line, sizeof(line),
           "[%lu][%s] %s | h=%u b=%u f=%u%% rssi=%d\n",
           static_cast<unsigned long>(upSec),
           tag,
           msg,
           ESP.getFreeHeap(),
           ESP.getMaxFreeBlockSize(),
           ESP.getHeapFragmentation(),
           (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0);

  Serial.print(line);

  if (!g_fsMounted) return;

  if (LittleFS.exists(DIAG_LOG_PATH)) {
    File check = LittleFS.open(DIAG_LOG_PATH, "r");
    size_t sz = check ? check.size() : 0;
    if (check) check.close();
    if (sz >= DIAG_LOG_MAX_BYTES) {
      if (LittleFS.exists(DIAG_PREV_PATH)) {
        LittleFS.remove(DIAG_PREV_PATH);
      }
      LittleFS.rename(DIAG_LOG_PATH, DIAG_PREV_PATH);
    }
  }

  File f = LittleFS.open(DIAG_LOG_PATH, "a");
  if (f) {
    f.print(line);
    f.close();
  }
}

void prepareTlsClient(uint16_t rxBuf, uint16_t txBuf) {
  g_tlsClient.stop();
  g_tlsClient.setInsecure();
  g_tlsClient.setBufferSizes(rxBuf, txBuf);
}

void captureTlsError(char* dst, size_t dstSize) {
  if (!dst || dstSize == 0) return;
  dst[0] = '\0';
  int sslCode = g_tlsClient.getLastSSLError(dst, dstSize);
  if (sslCode == 0) {
    dst[0] = '\0';
  }
}

void releaseTlsClient() {
  g_tlsClient.stop();
  yield();
}

void escapeHtmlToBuf(const char* src, char* dst, size_t dstSize) {
  if (!dst || dstSize == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }

  size_t out = 0;
  for (size_t i = 0; src[i] != '\0'; i++) {
    const char* rep = nullptr;
    size_t repLen = 1;
    if (src[i] == '&') {
      rep = "&amp;";
      repLen = 5;
    } else if (src[i] == '<') {
      rep = "&lt;";
      repLen = 4;
    } else if (src[i] == '>') {
      rep = "&gt;";
      repLen = 4;
    }

    if (out + repLen >= dstSize) {
      break;
    }
    if (rep) {
      memcpy(dst + out, rep, repLen);
      out += repLen;
    } else {
      dst[out++] = src[i];
    }
  }
  dst[out] = '\0';
}

void trimInPlace(char* s) {
  if (!s) return;
  size_t start = 0;
  while (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n') {
    start++;
  }
  size_t len = strlen(s + start);
  if (start > 0) {
    memmove(s, s + start, len + 1);
  }
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
    s[--len] = '\0';
  }
}

bool isTelegramConfigured() {
  return strlen(g_cfg.tg_bot_token) >= 20 && strlen(g_cfg.tg_chat_id) >= 3;
}

const char* activeSensorLabel() {
  return strlen(g_cfg.sensor_label) > 0 ? g_cfg.sensor_label : g_sensorId;
}

bool isAbnormalReset() {
  return strstr(g_bootResetReason, "Power On") == nullptr &&
         strstr(g_bootResetReason, "External System") == nullptr;
}

// ============================================================================
// Diagnostic Status Summary, HTTP Server (/status, /diag, /clear) & Serial CLI
// ============================================================================
void formatStatusSummary(char* buf, size_t bufSize) {
  updateHeapStats();
  snprintf(buf, bufSize,
           "=== Korotych ESP8266 Diagnostics ===\n"
           "fw_version: %s\n"
           "sensor_id: %s\n"
           "sensor_label: %s\n"
           "uptime_sec: %lu\n"
           "mains_220v: %s\n"
           "wifi_status: %d (ip=%s, rssi=%d dBm, disc=%lu, last_disc_reason=%u)\n"
           "heap_now: %u B (min_seen=%u B)\n"
           "max_block_now: %u B (min_seen=%u B)\n"
           "frag_now: %u%% (max_seen=%u%%)\n"
           "reset_reason: %s\n"
           "reset_info: %s\n"
           "village: ok=%lu fail=%lu last_http=%d ssl_err='%s'\n"
           "telegram: configured=%s ok=%lu fail=%lu last_http=%d ssl_err='%s'\n"
           "endpoints: /status | /diag (download full log) | /clear\n",
           FW_VERSION,
           g_sensorId,
           activeSensorLabel(),
           static_cast<unsigned long>(millis() / 1000UL),
           g_mainsState ? "ON" : "OFF",
           static_cast<int>(WiFi.status()),
           WiFi.localIP().toString().c_str(),
           (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0,
           static_cast<unsigned long>(g_wifiDiscCount),
           static_cast<unsigned int>(g_lastWifiDiscReason),
           ESP.getFreeHeap(),
           (g_minFreeHeapSeen == 0xFFFFFFFFUL) ? ESP.getFreeHeap() : g_minFreeHeapSeen,
           ESP.getMaxFreeBlockSize(),
           (g_minMaxBlockSeen == 0xFFFFFFFFUL) ? ESP.getMaxFreeBlockSize() : g_minMaxBlockSeen,
           ESP.getHeapFragmentation(),
           g_maxFragSeen,
           g_bootResetReason,
           g_bootResetInfo,
           static_cast<unsigned long>(g_villageOkCount),
           static_cast<unsigned long>(g_villageFailCount),
           g_lastVillageHttpCode,
           g_lastVillageSslErr,
           isTelegramConfigured() ? "yes" : "no",
           static_cast<unsigned long>(g_tgOkCount),
           static_cast<unsigned long>(g_tgFailCount),
           g_lastTgHttpCode,
           g_lastTgSslErr);
}

void streamFileToSerial(const char* path) {
  if (!g_fsMounted || !LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  if (!f) return;
  char chunk[129];
  while (f.available()) {
    size_t n = f.readBytes(chunk, sizeof(chunk) - 1);
    chunk[n] = '\0';
    Serial.print(chunk);
    yield();
  }
  f.close();
}

void dumpDiagnosticsToSerial() {
  char summary[768];
  formatStatusSummary(summary, sizeof(summary));
  Serial.println(F("----- BEGIN ESP8266 DIAG DUMP -----"));
  Serial.print(summary);
  Serial.println(F("--- PERSISTED LOGS ---"));
  streamFileToSerial(DIAG_PREV_PATH);
  streamFileToSerial(DIAG_LOG_PATH);
  Serial.println(F("----- END ESP8266 DIAG DUMP -----"));
}

void streamFileToHttp(const char* path) {
  if (!g_fsMounted || !LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  if (!f) return;
  char chunk[257];
  while (f.available()) {
    size_t n = f.readBytes(chunk, sizeof(chunk) - 1);
    chunk[n] = '\0';
    g_diagServer.sendContent(chunk);
    yield();
  }
  f.close();
}

void handleHttpStatus() {
  char summary[768];
  formatStatusSummary(summary, sizeof(summary));
  g_diagServer.send(200, "text/plain; charset=utf-8", summary);
}

void handleHttpDiagDownload() {
  char disp[64];
  snprintf(disp, sizeof(disp), "attachment; filename=\"%s_diag.txt\"", g_sensorId);
  g_diagServer.sendHeader("Content-Disposition", disp);
  g_diagServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_diagServer.send(200, "text/plain; charset=utf-8", "");

  char summary[768];
  formatStatusSummary(summary, sizeof(summary));
  g_diagServer.sendContent(summary);
  g_diagServer.sendContent("\n--- PERSISTED LOGS ---\n");
  streamFileToHttp(DIAG_PREV_PATH);
  streamFileToHttp(DIAG_LOG_PATH);
  g_diagServer.sendContent("");
}

void handleHttpClearLogs() {
  if (g_fsMounted) {
    LittleFS.remove(DIAG_PREV_PATH);
    LittleFS.remove(DIAG_LOG_PATH);
  }
  diagLog("DIAG", "Logs cleared via HTTP");
  g_diagServer.send(200, "text/plain; charset=utf-8", "OK: diagnostic logs cleared\n");
}

void startDiagServer() {
  if (g_diagServerRunning) return;
  g_diagServer.on("/", HTTP_GET, handleHttpStatus);
  g_diagServer.on("/status", HTTP_GET, handleHttpStatus);
  g_diagServer.on("/diag", HTTP_GET, handleHttpDiagDownload);
  g_diagServer.on("/clear", HTTP_GET, handleHttpClearLogs);
  g_diagServer.begin();
  g_diagServerRunning = true;
}

void stopDiagServer() {
  if (!g_diagServerRunning) return;
  g_diagServer.stop();
  g_diagServerRunning = false;
  yield();
}

void handleSerialCommands() {
  static char cmdBuf[24];
  static size_t cmdLen = 0;

  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';
        cmdLen = 0;
        trimInPlace(cmdBuf);

        if (strcasecmp(cmdBuf, "DUMP") == 0) {
          dumpDiagnosticsToSerial();
        } else if (strcasecmp(cmdBuf, "STATUS") == 0) {
          char summary[768];
          formatStatusSummary(summary, sizeof(summary));
          Serial.print(summary);
        } else if (strcasecmp(cmdBuf, "CLEAR") == 0) {
          if (g_fsMounted) {
            LittleFS.remove(DIAG_PREV_PATH);
            LittleFS.remove(DIAG_LOG_PATH);
          }
          diagLog("DIAG", "Logs cleared via Serial");
        } else if (strcasecmp(cmdBuf, "REBOOT") == 0) {
          diagLog("SYS", "Manual reboot via Serial");
          Serial.flush();
          delay(150);
          ESP.restart();
        }
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

void flushWifiEvents() {
  if (g_wifiDiscPending) {
    g_wifiDiscPending = false;
    diagLog("WIFI", "Disconnected reason=%u count=%lu",
            static_cast<unsigned int>(g_lastWifiDiscReason),
            static_cast<unsigned long>(g_wifiDiscCount));
  }
  if (g_wifiGotIpPending) {
    g_wifiGotIpPending = false;
    diagLog("WIFI", "GotIP=%s", WiFi.localIP().toString().c_str());
  }
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
bool mountFilesystem() {
  if (g_fsMounted) return true;
  if (!LittleFS.begin()) {
    Serial.println(F("[FS] Mount failed, formatting..."));
    if (!LittleFS.format() || !LittleFS.begin()) {
      Serial.println(F("[FS] Format/mount failed"));
      return false;
    }
  }
  g_fsMounted = true;
  return true;
}

bool loadConfig() {
  if (!mountFilesystem()) {
    return false;
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
  if (!mountFilesystem()) return false;

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
  stopDiagServer();
  releaseTlsClient();

  // Temporarily allow SDK to persist newly entered Wi-Fi credentials to Flash
  WiFi.persistent(true);

  String customHead = FPSTR(CUSTOM_CSS);
  WiFiManager wm;
  g_shouldSaveConfig = false;

  wm.setSaveConfigCallback(onSaveConfigCallback);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SEC);
  wm.setConnectTimeout(25);
  wm.setTitle("⚡ Пеленгатор света · Коротич");
  wm.setCustomHeadElement(customHead.c_str());

  char headerHtml[160];
  snprintf(headerHtml, sizeof(headerHtml),
           "<div class='hint'>ID датчика: <b>%s</b> · Прошивка: <b>v%s</b></div>",
           g_sensorId, FW_VERSION);
  WiFiManagerParameter pHeader(headerHtml);

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
    diagLog("WIFI", "Starting forced config portal");
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

  // Disable Flash writes during normal runtime reconnects
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  if (!connected) {
    diagLog("WIFI", "Portal timeout/not connected -> reboot");
    Serial.flush();
    delay(1500);
    ESP.restart();
  }

  g_lastVillageSuccessMs = millis();
  startDiagServer();

  diagLog("WIFI", "Connected IP=%s", WiFi.localIP().toString().c_str());
}

// ============================================================================
// Channel 1: Village Bot API (POST /api/meter)
// ============================================================================
bool sendToVillageBot(bool online) {
  if (WiFi.status() != WL_CONNECTED) {
    g_villageFailCount++;
    g_lastVillageHttpCode = -100;
    strlcpy(g_lastVillageSslErr, "wifi_disconnected", sizeof(g_lastVillageSslErr));
    diagLog("VLG", "Skip send: WiFi down");
    return false;
  }

  prepareTlsClient(1024, 512);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(g_tlsClient, VILLAGE_URL)) {
    g_villageFailCount++;
    g_lastVillageHttpCode = -101;
    strlcpy(g_lastVillageSslErr, "http_begin_failed", sizeof(g_lastVillageSslErr));
    releaseTlsClient();
    diagLog("VLG", "http.begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Meter-Token", VILLAGE_TOKEN);

  JsonDocument doc;
  doc["online"] = online;
  doc["sensor_id"] = g_sensorId;
  doc["sensor_label"] = activeSensorLabel();
  doc["source"] = SENSOR_SOURCE;

  char payload[256];
  size_t len = serializeJson(doc, payload, sizeof(payload));

  int code = http.POST(reinterpret_cast<uint8_t*>(payload), len);
  if (code <= 0) {
    captureTlsError(g_lastVillageSslErr, sizeof(g_lastVillageSslErr));
  } else {
    g_lastVillageSslErr[0] = '\0';
  }
  http.end();
  releaseTlsClient();

  g_lastVillageHttpCode = code;
  bool ok = (code >= 200 && code < 300);
  if (ok) {
    g_villageOkCount++;
    g_lastVillageSuccessMs = millis();
    diagLog("VLG", "POST on=%d -> %d (ok=%lu)", online ? 1 : 0, code, static_cast<unsigned long>(g_villageOkCount));
  } else {
    g_villageFailCount++;
    diagLog("VLG", "FAIL on=%d -> %d ssl='%s' (fail=%lu)",
            online ? 1 : 0, code, g_lastVillageSslErr, static_cast<unsigned long>(g_villageFailCount));
  }
  return ok;
}

// ============================================================================
// Channel 2: Personal Telegram Bot (optional, one-shot sendMessage only)
// ============================================================================
bool sendTelegramMessage(const char* htmlText) {
  if (!isTelegramConfigured() || !htmlText || htmlText[0] == '\0') {
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    g_tgFailCount++;
    g_lastTgHttpCode = -100;
    strlcpy(g_lastTgSslErr, "wifi_disconnected", sizeof(g_lastTgSslErr));
    diagLog("TG", "Skip send: WiFi down");
    return false;
  }

  prepareTlsClient(1024, 512);

  char url[160];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", g_cfg.tg_bot_token);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(g_tlsClient, url)) {
    g_tgFailCount++;
    g_lastTgHttpCode = -101;
    strlcpy(g_lastTgSslErr, "http_begin_failed", sizeof(g_lastTgSslErr));
    releaseTlsClient();
    diagLog("TG", "http.begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["chat_id"] = g_cfg.tg_chat_id;
  doc["text"] = htmlText;
  doc["parse_mode"] = "HTML";
  doc["disable_web_page_preview"] = true;

  char payload[512];
  size_t len = serializeJson(doc, payload, sizeof(payload));

  int code = http.POST(reinterpret_cast<uint8_t*>(payload), len);
  if (code <= 0) {
    captureTlsError(g_lastTgSslErr, sizeof(g_lastTgSslErr));
  } else {
    g_lastTgSslErr[0] = '\0';
  }
  http.end();
  releaseTlsClient();

  g_lastTgHttpCode = code;
  bool ok = (code >= 200 && code < 300);
  if (ok) {
    g_tgOkCount++;
    diagLog("TG", "sendMessage -> %d", code);
  } else {
    g_tgFailCount++;
    diagLog("TG", "FAIL -> %d ssl='%s'", code, g_lastTgSslErr);
  }
  return ok;
}

void notifyPersonalBoot(bool online) {
  if (!isTelegramConfigured()) return;

  char escapedLabel[128];
  escapeHtmlToBuf(activeSensorLabel(), escapedLabel, sizeof(escapedLabel));

  char resetLine[128] = "";
  if (isAbnormalReset()) {
    char escReason[64];
    escapeHtmlToBuf(g_bootResetReason, escReason, sizeof(escReason));
    snprintf(resetLine, sizeof(resetLine), "\n⚠️ Причина рестарта: <code>%s</code>", escReason);
  }

  char msg[480];
  snprintf(msg, sizeof(msg),
           "🔌 <b>Датчик запущен</b>\n"
           "📍 Точка: <b>%s</b> (<code>%s</code>)\n"
           "⚡ Сеть 220 В: <b>%s</b>\n"
           "📶 Wi-Fi RSSI: %d dBm\n"
           "📦 Прошивка: <code>v%s</code>\n"
           "🩺 Логи (в локальной сети): http://%s/diag%s",
           escapedLabel,
           g_sensorId,
           online ? "ЕСТЬ" : "НЕТ",
           WiFi.RSSI(),
           FW_VERSION,
           WiFi.localIP().toString().c_str(),
           resetLine);

  sendTelegramMessage(msg);
}

void notifyPersonalStatusChange(bool online) {
  if (!isTelegramConfigured()) return;

  char escapedLabel[128];
  escapeHtmlToBuf(activeSensorLabel(), escapedLabel, sizeof(escapedLabel));

  char msg[320];
  snprintf(msg, sizeof(msg),
           "%s\n📍 Точка: <b>%s</b> (<code>%s</code>)",
           online ? "⚡ <b>Свет ПОЯВИЛСЯ</b>" : "🌑 <b>Свет ПРОПАЛ</b>",
           escapedLabel,
           g_sensorId);

  sendTelegramMessage(msg);
}

// ============================================================================
// Pull-OTA Update from GitHub (ESP8266httpUpdate)
// ============================================================================
void onOtaProgress(int cur, int total) {
  static uint32_t lastToggle = 0;
  static bool ledState = false;
  uint32_t now = millis();
  if ((uint32_t)(now - lastToggle) >= 120) {
    lastToggle = now;
    ledState = !ledState;
    setLed(ledState);
  }
  (void)cur;
  (void)total;
}

void checkAndApplyOta() {
  if (WiFi.status() != WL_CONNECTED) {
    diagLog("OTA", "Skip: WiFi down");
    return;
  }

  stopDiagServer();
  releaseTlsClient();

  uint32_t freeHeap = ESP.getFreeHeap();
  diagLog("OTA", "Check v%s", FW_VERSION);

  if (freeHeap < MIN_OTA_FREE_HEAP_BYTES) {
    diagLog("OTA", "Skip: low heap (%u < %u)", freeHeap, MIN_OTA_FREE_HEAP_BYTES);
    startDiagServer();
    return;
  }

  prepareTlsClient(1024, 512);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(g_tlsClient, OTA_VERSION_URL)) {
    diagLog("OTA", "Version http.begin failed");
    releaseTlsClient();
    startDiagServer();
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char sslErr[64] = "";
    if (code <= 0) captureTlsError(sslErr, sizeof(sslErr));
    http.end();
    releaseTlsClient();
    diagLog("OTA", "Version HTTP %d ssl='%s'", code, sslErr);
    startDiagServer();
    return;
  }

  char remoteVer[32] = "";
  WiFiClient* stream = http.getStreamPtr();
  if (stream) {
    size_t idx = 0;
    uint32_t readStart = millis();
    while ((http.connected() || stream->available()) &&
           idx < sizeof(remoteVer) - 1 &&
           (uint32_t)(millis() - readStart) < 3000) {
      while (stream->available() && idx < sizeof(remoteVer) - 1) {
        remoteVer[idx++] = static_cast<char>(stream->read());
      }
      yield();
    }
    remoteVer[idx] = '\0';
  }
  http.end();
  releaseTlsClient();

  trimInPlace(remoteVer);

  if (remoteVer[0] == '\0' || strlen(remoteVer) > 24 || strchr(remoteVer, '<') != nullptr) {
    diagLog("OTA", "Invalid version='%s'", remoteVer);
    startDiagServer();
    return;
  }

  if (strcmp(remoteVer, FW_VERSION) == 0) {
    diagLog("OTA", "Up to date (v%s)", FW_VERSION);
    startDiagServer();
    return;
  }

  diagLog("OTA", "New version v%s -> v%s", FW_VERSION, remoteVer);

  if (isTelegramConfigured()) {
    char escapedLabel[128];
    char escapedVer[48];
    escapeHtmlToBuf(activeSensorLabel(), escapedLabel, sizeof(escapedLabel));
    escapeHtmlToBuf(remoteVer, escapedVer, sizeof(escapedVer));

    char msg[320];
    snprintf(msg, sizeof(msg),
             "🔄 <b>Обновление прошивки (OTA)</b>\n"
             "📍 Точка: <b>%s</b>\n"
             "📦 Версия: <code>v%s</code> → <code>v%s</code>",
             escapedLabel, FW_VERSION, escapedVer);
    sendTelegramMessage(msg);
  }

  releaseTlsClient();

  freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_OTA_FREE_HEAP_BYTES) {
    diagLog("OTA", "Abort download: low heap (%u)", freeHeap);
    startDiagServer();
    return;
  }

  prepareTlsClient(2048, 512);

  ESPhttpUpdate.setLedPin(PIN_LED, LED_ACTIVE_LEVEL);
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.onProgress(onOtaProgress);
  ESPhttpUpdate.rebootOnUpdate(true);

  t_httpUpdate_return ret = ESPhttpUpdate.update(g_tlsClient, OTA_BIN_URL);
  releaseTlsClient();

  if (ret == HTTP_UPDATE_FAILED) {
    int errCode = ESPhttpUpdate.getLastError();
    char escapedErr[128];
    escapeHtmlToBuf(ESPhttpUpdate.getLastErrorString().c_str(), escapedErr, sizeof(escapedErr));

    diagLog("OTA", "Update failed (%d): %s", errCode, escapedErr);
    setLed(g_mainsState);

    if (isTelegramConfigured()) {
      char msg[320];
      snprintf(msg, sizeof(msg),
               "⚠️ <b>Ошибка OTA-обновления</b>\n"
               "Код: <code>%d</code>\n%s",
               errCode, escapedErr);
      sendTelegramMessage(msg);
    }
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    diagLog("OTA", "No updates");
    setLed(g_mainsState);
  }

  startDiagServer();
}

// ============================================================================
// Software Watchdog (Heap & Village Delivery Health)
// ============================================================================
void checkSystemHealth() {
  updateHeapStats();
  uint32_t freeHeap = ESP.getFreeHeap();

  if (freeHeap < MIN_FREE_HEAP_BYTES) {
    diagLog("WDT", "Low heap (%u < %u) -> restart", freeHeap, MIN_FREE_HEAP_BYTES);
    Serial.flush();
    delay(200);
    ESP.restart();
  }

  if (WiFi.status() == WL_CONNECTED) {
    uint32_t sinceOk = (uint32_t)(millis() - g_lastVillageSuccessMs);
    if (sinceOk >= VILLAGE_STALE_RESTART_MS) {
      diagLog("WDT", "No village OK for %lu ms -> restart", static_cast<unsigned long>(sinceOk));
      Serial.flush();
      delay(200);
      ESP.restart();
    }
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
      diagLog("CFG", "5s hold -> opening config portal");
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

  strlcpy(g_bootResetReason, ESP.getResetReason().c_str(), sizeof(g_bootResetReason));
  strlcpy(g_bootResetInfo, ESP.getResetInfo().c_str(), sizeof(g_bootResetInfo));

  loadConfig();
  diagLog("BOOT", "v%s id=%s reason='%s' info='%s'",
          FW_VERSION, g_sensorId, g_bootResetReason, g_bootResetInfo);

  g_wifiDiscHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& evt) {
    g_lastWifiDiscReason = evt.reason;
    g_wifiDiscCount++;
    g_wifiDiscPending = true;
  });
  g_wifiGotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& evt) {
    (void)evt;
    g_wifiGotIpPending = true;
  });

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  bool forcePortalOnBoot = (digitalRead(PIN_RESET_CFG) == RESET_ACTIVE_LEVEL);
  if (forcePortalOnBoot) {
    diagLog("CFG", "Reset pin LOW at boot -> forcing portal");
  }
  runConfigPortal(forcePortalOnBoot);

  // Initial mains measurement
  g_mainsState = sampleMains50Hz();
  g_candidateState = g_mainsState;
  g_candidateSinceMs = millis();
  setLed(g_mainsState);

  diagLog("MAINS", "Initial=%s", g_mainsState ? "220V_ON" : "220V_OFF");

  // Initial notifications
  g_lastVillageSuccessMs = millis();
  g_villageLastFailed = !sendToVillageBot(g_mainsState);
  g_villagePendingSend = false;
  g_lastVillageAttemptMs = millis();

  notifyPersonalBoot(g_mainsState);

  g_bootMs = millis();
  g_otaInitialDone = false;
  g_lastOtaCheckMs = g_bootMs;
}

void loop() {
  handleSerialCommands();
  flushWifiEvents();
  if (g_diagServerRunning) {
    g_diagServer.handleClient();
  }
  handleResetButton();
  checkSystemHealth();

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
    diagLog("MAINS", "Change -> %s", g_mainsState ? "220V_ON" : "220V_OFF");

    g_villagePendingSend = true;
    notifyPersonalStatusChange(g_mainsState);
  }

  // 2. Village bot immediate send / retry (15s) / heartbeat (90s)
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
