/*
 * Sandbatteri - Arduino OPTA Pro
 *
 * Job: Poll backend for heater/pump commands, execute relays,
 *      send heartbeat, count DDS661 energy pulses.
 *
 * Relay 1 = Heating element 1  (D0)
 * Relay 2 = Heating element 2  (D1)
 * Relay 3 = Heating element 3  (D2)
 * Relay 4 = Water pump         (D3)
 * DDS661  = Energy meter via pulse output (terminals 20/21 → A0)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiSSLClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <mbed.h>

mbed::Ticker pumpTicker;

// ─── NON BLOCKING SERIAL SETUP ───────────────────────────────────────────────
#define DBG(x) \
  if (Serial) Serial.print(x)
#define DBGLN(x) \
  if (Serial) Serial.println(x)

// ─── USER CONFIG ─────────────────────────────────────────────────────────────
const char* WIFI_SSID = "SSD";
const char* WIFI_PASS = "PASSWORD";
const char* PRODUCT_KEY = "PRODUCT_KEY";

const char* API_HOST = "API_ADDRESS";
const int API_PORT = 443;

const char* CF_CLIENT_ID = "CF_CLIENT_ID";
const char* CF_CLIENT_SECRET = "CF_CLIENT_SECRET";

// ─── PINS ────────────────────────────────────────────────────────────────────
#define RELAY_HEAT1 0
#define RELAY_HEAT2 1
#define RELAY_HEAT3 2
#define RELAY_PUMP 3

// ─── DDS661 PULSE CONFIG ─────────────────────────────────────────────────────
// Connect DDS661 terminal 20 or 21 to A0
// Check imp/kWh printed on your DDS661 label
#define DDS661_PIN I1
#define IMP_PER_KWH 1000

// ─── INTERVALS ───────────────────────────────────────────────────────────────
#define POLL_INTERVAL_MS 2000
#define HEARTBEAT_INTERVAL_MS 30000
#define WATCHDOG_TIMEOUT_MS 60000

// ─── STATE ───────────────────────────────────────────────────────────────────
bool heaterActive = false;
bool pumpActive = false;

volatile unsigned long pulseCounts = 0;
float energyKwh = 0.0;

unsigned long lastPoll = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastSuccess = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// ─── SETTINGS STATE ──────────────────────────────────────────────
bool autoPumpEnabled = false;
int pumpIntervalSeconds = 0;
bool pumpAutoState = false;

volatile bool pumpTickerFlag = false;   // set in ISR, read in loop
int activePumpInterval = 0;             // track current interval to detect changes

// ─── TICKER ISR ──────────────────────────────────────────────────
void onPumpTick() {
  pumpTickerFlag = true;
}

// ─── REPLACE updatePumpTicker (call when settings change) ────────
void updatePumpTicker() {
  pumpTicker.detach();
  if (autoPumpEnabled && pumpIntervalSeconds > 0) {
    pumpTicker.attach(&onPumpTick, (float)pumpIntervalSeconds);
    activePumpInterval = pumpIntervalSeconds;
  } else {
    activePumpInterval = 0;
  }
}

// ─── ISR ─────────────────────────────────────────────────────────────────────
void onDDS661Pulse() {
  pulseCounts++;
}

// ─── DDS661 READ ─────────────────────────────────────────────────────────────
void readDDS661() {
  energyKwh = (float)pulseCounts / IMP_PER_KWH;
  // DBG("[DDS661] Energy: ");
  // DBG(energyKwh);
  // DBGLN(" kWh");
}

// ─── HELPERS ─────────────────────────────────────────────────────────────────
String getTimestamp() {
  time_t t = timeClient.getEpochTime();
  struct tm* ptm = gmtime(&t);
  char buf[25];
  sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
          ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
          ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return String(buf);
}

// ─── RAW HTTPS REQUEST ───────────────────────────────────────────────────────
int rawHttps(const char* method, const char* path, const String& body, String& responseBody) {
  WiFiSSLClient client;

  if (!client.connect(API_HOST, API_PORT)) {
    // DBGLN("[HTTP] Connect failed");
    return -1;
  }

  client.print(String(method) + " " + path + " HTTP/1.1\r\n");
  client.print("Host: " + String(API_HOST) + "\r\n");
  client.print("X-Product-Key: " + String(PRODUCT_KEY) + "\r\n");
  client.print("CF-Access-Client-Id: " + String(CF_CLIENT_ID) + "\r\n");
  client.print("CF-Access-Client-Secret: " + String(CF_CLIENT_SECRET) + "\r\n");
  if (body.length() > 0) {
    client.print("Content-Type: application/json\r\n");
    client.print("Content-Length: " + String(body.length()) + "\r\n");
  }
  client.print("Connection: close\r\n\r\n");
  if (body.length() > 0) client.print(body);
  client.flush();

  // Wait for response
  unsigned long t = millis();
  while (!client.available()) {
    if (millis() - t > 5000) {
      // DBGLN("[HTTP] Timeout");
      client.stop();
      return -1;
    }
    delay(10);
  }

  // Read status line
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  int code = -1;
  int sp1 = statusLine.indexOf(' ');
  if (sp1 >= 0) {
    int sp2 = statusLine.indexOf(' ', sp1 + 1);
    code = statusLine.substring(sp1 + 1, sp2 > 0 ? sp2 : statusLine.length()).toInt();
  }

  // Skip headers
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
  }

  // Read body
  responseBody = "";
  t = millis();
  while ((client.connected() || client.available()) && millis() - t < 3000) {
    while (client.available()) {
      responseBody += (char)client.read();
    }
    delay(1);
  }

  client.stop();
  return code;
}

// ─── POLL SETTINGS ───────────────────────────────────────────────
void pollSettings() {
  String empty, responseBody;
  int code = rawHttps("GET", "/api/v1/settings", empty, responseBody);
  if (code != 200) return;

  int braceIndex = responseBody.indexOf('{');
  int lastBrace = responseBody.lastIndexOf('}');
  if (braceIndex >= 0 && lastBrace >= 0)
    responseBody = responseBody.substring(braceIndex, lastBrace + 1);

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, responseBody)) return;

  bool newAutoPump = doc["auto_pump_enabled"] | false;
  int newInterval  = doc["pump_interval_seconds"] | 0;

  // Only reattach if something changed
  if (newAutoPump != autoPumpEnabled || newInterval != activePumpInterval) {
    autoPumpEnabled      = newAutoPump;
    pumpIntervalSeconds  = newInterval;
    updatePumpTicker();
  }
}

// ─── RELAYS ──────────────────────────────────────────────────────────────────

void setHeater(int index, bool on) {
  switch (index) {
    case 0:
      digitalWrite(RELAY_HEAT1, on ? HIGH : LOW);
      digitalWrite(LED_D0, on ? HIGH : LOW);
      break;
    case 1:
      digitalWrite(RELAY_HEAT2, on ? HIGH : LOW);
      digitalWrite(LED_D1, on ? HIGH : LOW);
      break;
    case 2:
      digitalWrite(RELAY_HEAT3, on ? HIGH : LOW);
      digitalWrite(LED_D2, on ? HIGH : LOW);
      break;
  }
  // DBG("[RELAY] Heater ");
  // DBG(index + 1);
  // DBGLN(on ? " ON" : " OFF");
}


void setPump(bool on) {
  pumpActive = on;
  digitalWrite(RELAY_PUMP, on ? HIGH : LOW);
  digitalWrite(LED_D3, on ? HIGH : LOW);
  // DBGLN(on ? "[RELAY] Pump ON" : "[RELAY] Pump OFF");
}

void safeState() {
  setHeater(0, false);
  setHeater(1, false);
  setHeater(2, false);
  setPump(false);
}

// ─── POLL /control/status ────────────────────────────────────────────────────
void pollControlStatus() {
  String empty, responseBody;
  int code = rawHttps("GET", "/api/v1/control/status", empty, responseBody);

  int braceIndex = responseBody.indexOf('{');
  int lastBrace = responseBody.lastIndexOf('}');
  if (braceIndex >= 0 && lastBrace >= 0)
    responseBody = responseBody.substring(braceIndex, lastBrace + 1);

  // DBGLN("[POLL] parsed: ");
  // DBGLN(responseBody);

  if (code != 200) {
    // DBGLN("[POLL] HTTP ");
    // DBG(code);
    return;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, responseBody)) {
    // DBGLN("[POLL] JSON err");
    return;
  }

  JsonArray heaters = doc["heaters"];
  for (JsonObject h : heaters) {
    int idx = h["index"] | -1;
    bool active = h["active"] | false;
    if (idx >= 0) setHeater(idx, active);
  }

  if (!autoPumpEnabled) {
    bool wantPump = doc["pump"]["active"] | false;
    if (wantPump != pumpActive) setPump(wantPump);
  }

  lastSuccess = millis();
}

// ─── SEND ENERGY ─────────────────────────────────────────────────────────────
void sendEnergyReading() {
  StaticJsonDocument<128> doc;
  doc["timestamp"] = getTimestamp();
  doc["energy_kwh"] = energyKwh;
  String body;
  serializeJson(doc, body);

  readDDS661();

  String responseBody;
  int code = rawHttps("POST", "/api/v1/data/energy", body, responseBody);

  if (code == 201) {
    // DBGLN("[ENERGY] OK");
  } else {
    // DBGLN("[ENERGY] HTTP ");
    // DBG(code);
  }
}

// ─── HEARTBEAT ───────────────────────────────────────────────────────────────
void sendHeartbeat() {
  StaticJsonDocument<128> doc;
  doc["product_key"] = PRODUCT_KEY;
  doc["timestamp"] = getTimestamp();
  doc["uptime_seconds"] = (int)(millis() / 1000);
  String body;
  serializeJson(doc, body);

  String responseBody;
  int code = rawHttps("POST", "/api/v1/events/heartbeat", body, responseBody);

  if (code == 200) {
    // DBGLN("[HEARTBEAT] OK");
    lastSuccess = millis();
  } else {
    // DBGLN("[HEARTBEAT] HTTP ");
    // DBG(code);
  }
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_D0, OUTPUT);
  pinMode(LED_D1, OUTPUT);
  pinMode(LED_D2, OUTPUT);
  pinMode(LED_D3, OUTPUT);


  pinMode(RELAY_HEAT1, OUTPUT);
  pinMode(RELAY_HEAT2, OUTPUT);
  pinMode(RELAY_HEAT3, OUTPUT);
  pinMode(RELAY_PUMP, OUTPUT);
  safeState();

  pinMode(DDS661_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(DDS661_PIN), onDDS661Pulse, FALLING);
  // DBGLN("[DDS661] Pulse counter ready");

  // DBG("[WiFi] Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    // DBG(".");
  }
  // DBGLN(" OK: " + WiFi.localIP().toString());

  updatePumpTicker();

  timeClient.begin();
  timeClient.update();
  // DBGLN("[NTP] " + getTimestamp());

  lastSuccess = millis();
  // DBGLN("[BOOT] OPTA ready.");
}

// ─── LOOP ────────────────────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    // DBGLN("[WiFi] Lost! Reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    delay(5000);
    return;
  }

  timeClient.update();

  // Auto pump toggle
  // DBGLN("[AUTO] Autopump enabled: ");
  // DBGLN(autoPumpEnabled);

  // DBGLN("[AUTO] Last pump toggled at tick ");
  // DBGLN(lastPumpToggle);

  // DBGLN("[AUTO] Pump interval (ms): ");
  // DBGLN(pumpIntervalSeconds * 1000);

  if (pumpTickerFlag) {
    pumpTickerFlag = false;
    if (autoPumpEnabled) {
      pumpAutoState = !pumpAutoState;
      setPump(pumpAutoState);
    }
  }

  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();
    pollControlStatus();
    pollSettings();
    sendEnergyReading();
  }

  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }

  if (millis() - lastSuccess >= WATCHDOG_TIMEOUT_MS) {
    // DBGLN("[WATCHDOG] Timeout! Resetting...");
    safeState();
    delay(1000);
    WiFi.disconnect();
    WiFi.end();
    delay(500);
    NVIC_SystemReset();
  }
}
