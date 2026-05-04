/*
 * Sandbatteri - Arduino UNO R4 WiFi
 *
 * Sensors:
 *   DS18B20 #1       = Sand temp (side)     → One-Wire digital
 *   DS18B20 #2       = Sand temp (core)     → One-Wire digital
 *   YF-B7 #1 flow    = Flow in              → Pulse interrupt pin
 *   YF-B7 #1 NTC     = Water temp in        → Analog NTC 50KΩ
 *   YF-B7 #2 flow    = Flow out             → Pulse interrupt pin
 *   YF-B7 #2 NTC     = Water temp out       → Analog NTC 50KΩ
 *
 * Job: Read sensors → POST /data every 10s
 *      Heartbeat every 30s
 *      Watchdog software reset if no success in 60s

 */

#include <WiFiS3.h>
#include <time.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ─── NON BLOCKING SERIAL SETUP ───────────────────────────────────────────────
#define DBG(x) if (Serial) Serial.print(x)
#define DBGLN(x) if (Serial) Serial.println(x)

// ─── CONFIG ──────────────────────────────────────────────────────────────────
const char* WIFI_SSID = "Lab-ZBC";
const char* WIFI_PASS = "Prestige#PuzzledCASH48!";

const char* API_HOST  = "dunepower-api.acceptable.pro";
const int   API_PORT  = 443;
const char* PRODUCT_KEY      = "DP-SB-01";
const char* CF_CLIENT_ID     = "273cbc1aec51a1e9aec4d1f0e7ba73f1.access";
const char* CF_CLIENT_SECRET = "2ce6650dd96b4bcb48b1dc2482b8874f0b4fd89fe1f060bb6af6bd27aebc8955";

// ─── PINS ────────────────────────────────────────────────────────────────────
#define ONE_WIRE_PIN     2    // DS18B20 both on same bus
#define FLOW_IN_PIN      3    // YF-B7 #1 pulse (must be interrupt-capable)
#define FLOW_OUT_PIN     4    // YF-B7 #2 pulse (must be interrupt-capable)
#define NTC_WATER_IN     A2   // YF-B7 #1 NTC temp
#define NTC_WATER_OUT    A3   // YF-B7 #2 NTC temp

// ─── NTC CONSTANTS (50KΩ, Beta 3950) ────────────────────────────────────────
#define NTC_NOMINAL      50000.0   // 50KΩ at 25°C
#define NTC_BETA         3950.0
#define NTC_REF_TEMP     298.15    // 25°C in Kelvin
#define SERIES_RESISTOR  50000.0   // Use 50KΩ series resistor to match NTC
#define ADC_MAX          1023.0

// ─── INTERVALS ───────────────────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS    10000
#define HEARTBEAT_INTERVAL_MS 30000
#define WATCHDOG_TIMEOUT_MS   60000

// ─── GLOBALS ─────────────────────────────────────────────────────────────────
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sandSensors(&oneWire);
DeviceAddress sandSideAddr, sandCoreAddr;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

volatile unsigned long flowInPulses  = 0;
volatile unsigned long flowOutPulses = 0;

unsigned long lastSensorTime = 0;
unsigned long lastHeartbeat  = 0;
unsigned long lastSuccess    = 0;

// ─── ISR ─────────────────────────────────────────────────────────────────────
void flowInISR()  { flowInPulses++;  }
void flowOutISR() { flowOutPulses++; }

// ─── NTC TEMP READ ───────────────────────────────────────────────────────────
// Steinhart-Hart simplified
float readNTC(int pin) {
  int raw = analogRead(pin);
  if (raw == 0) return -999.0;  // short circuit guard

  float resistance = SERIES_RESISTOR * (ADC_MAX / raw - 1.0);

  float steinhart = resistance / NTC_NOMINAL;
  steinhart = log(steinhart);
  steinhart /= NTC_BETA;
  steinhart += 1.0 / NTC_REF_TEMP;
  steinhart = 1.0 / steinhart;
  steinhart -= 273.15;  // Kelvin to Celsius

  return steinhart;
}

// ─── FLOW RATE ───────────────────────────────────────────────────────────────
// YF-B7: Q [L/min] = f [Hz] / 11
float calcFlowRate(volatile unsigned long& pulses, unsigned long dtMs) {
  unsigned long p = pulses;
  pulses = 0;
  float hz = p / (dtMs / 1000.0);
  return hz / 11.0;
}

// ─── TIMESTAMP ───────────────────────────────────────────────────────────────
String getTimestamp() {
  time_t t = (time_t)timeClient.getEpochTime();
  struct tm* ptm = gmtime(&t);
  char buf[25];
  sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
    ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
    ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return String(buf);
}

// ─── HEADERS HELPER ──────────────────────────────────────────────────────────
void addHeaders(HttpClient& http, int bodyLen = -1) {
  http.sendHeader("X-Product-Key",          PRODUCT_KEY);
  http.sendHeader("CF-Access-Client-Id",     CF_CLIENT_ID);
  http.sendHeader("CF-Access-Client-Secret", CF_CLIENT_SECRET);
  if (bodyLen >= 0) {
    http.sendHeader("Content-Type",   "application/json");
    http.sendHeader("Content-Length", bodyLen);
  }
}

// ─── HTTP POST ───────────────────────────────────────────────────────────────
int httpPost(const char* path, const String& body) {
  WiFiSSLClient ssl;
  HttpClient http(ssl, API_HOST, API_PORT);

  http.beginRequest();
  http.post(path);
  addHeaders(http, body.length());
  http.beginBody();
  http.print(body);
  http.endRequest();

  int code = http.responseStatusCode();
  http.stop();
  return code;
}

// ─── POST SENSOR DATA ────────────────────────────────────────────────────────
void postSensorData(float sandSide, float sandCore,
                    float waterIn, float waterOut,
                    float flowIn, float flowOut) {
  StaticJsonDocument<512> doc;
  doc["product_key"] = PRODUCT_KEY;
  doc["timestamp"]   = getTimestamp();
  doc["power_w"]     = 0;
  doc["energy_kwh"]  = 0;

  JsonArray temps = doc.createNestedArray("temperatures");
  JsonObject t0 = temps.createNestedObject(); t0["index"] = 0; t0["label"] = "sand_side"; t0["value"] = serialized(String(sandSide, 2));
  JsonObject t1 = temps.createNestedObject(); t1["index"] = 1; t1["label"] = "sand_core"; t1["value"] = serialized(String(sandCore, 2));
  JsonObject t2 = temps.createNestedObject(); t2["index"] = 2; t2["label"] = "water_in";  t2["value"] = serialized(String(waterIn,  2));
  JsonObject t3 = temps.createNestedObject(); t3["index"] = 3; t3["label"] = "water_out"; t3["value"] = serialized(String(waterOut, 2));

  JsonArray flows = doc.createNestedArray("flow_rates");
  JsonObject f0 = flows.createNestedObject(); f0["index"] = 0; f0["value"] = serialized(String(flowIn,  2));
  JsonObject f1 = flows.createNestedObject(); f1["index"] = 1; f1["value"] = serialized(String(flowOut, 2));

  String body;
  serializeJson(doc, body);

  int code = httpPost("/api/v1/data", body);
  if (code == 201) {
    DBGLN("[POST /data] OK 201");
    lastSuccess = millis();
  } else {
    DBG("[POST /data] Failed: ");
    DBGLN(code);
  }
}

// ─── POST HEARTBEAT ──────────────────────────────────────────────────────────
void postHeartbeat() {
  StaticJsonDocument<128> doc;
  doc["product_key"]    = PRODUCT_KEY;
  doc["timestamp"]      = getTimestamp();
  doc["uptime_seconds"] = (int)(millis() / 1000);

  String body;
  serializeJson(doc, body);

  int code = httpPost("/api/v1/events/heartbeat", body);
  if (code == 200) {
    DBGLN("[HEARTBEAT] OK");
    lastSuccess = millis();
  } else {
    DBG("[HEARTBEAT] Failed: ");
    DBGLN(code);
  }
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  DBGLN("[BOOT] Sandbatteri R4 starting...");

  // DS18B20
  sandSensors.begin();
  if (sandSensors.getDeviceCount() < 2) {
    DBGLN("[WARN] Expected 2x DS18B20, found: " + String(sandSensors.getDeviceCount()));
  }
  sandSensors.getAddress(sandSideAddr, 0);
  sandSensors.getAddress(sandCoreAddr, 1);

  // Flow sensor interrupts
  pinMode(FLOW_IN_PIN,  INPUT_PULLUP);
  pinMode(FLOW_OUT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_IN_PIN),  flowInISR,  RISING);
  attachInterrupt(digitalPinToInterrupt(FLOW_OUT_PIN), flowOutISR, RISING);

  // WiFi
  DBG("[WiFi] Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); DBG("."); }
  DBGLN(" OK: " + WiFi.localIP().toString());

  // NTP
  timeClient.begin();
  timeClient.update();
  DBGLN("[NTP] " + getTimestamp());

  lastSuccess = millis();
  DBGLN("[BOOT] R4 ready.");
}

// ─── LOOP ────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    DBGLN("[WiFi] Lost! Reconnecting...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    delay(5000);
    return;
  }

  timeClient.update();

  // ── SENSOR READ + POST ───────────────────────────────────────────────────
  if (now - lastSensorTime >= SENSOR_INTERVAL_MS) {
    unsigned long dt = now - lastSensorTime;
    lastSensorTime = now;

    sandSensors.requestTemperatures();
    float sandSide = sandSensors.getTempC(sandSideAddr);
    float sandCore = sandSensors.getTempC(sandCoreAddr);

    float waterIn  = readNTC(NTC_WATER_IN);
    float waterOut = readNTC(NTC_WATER_OUT);

    float flowIn  = calcFlowRate(flowInPulses,  dt);
    float flowOut = calcFlowRate(flowOutPulses, dt);

    DBG("[SENSOR] Sand side: "); DBG(sandSide);
    DBG("C  Core: ");            DBG(sandCore);
    DBG("C  WaterIn: ");         DBG(waterIn);
    DBG("C  WaterOut: ");        DBG(waterOut);
    DBG("C  FlowIn: ");          DBG(flowIn);
    DBG(" L/min  FlowOut: ");    DBG(flowOut);
    DBGLN(" L/min");

    postSensorData(sandSide, sandCore, waterIn, waterOut, flowIn, flowOut);
  }

  // ── HEARTBEAT ────────────────────────────────────────────────────────────
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    postHeartbeat();
  }

  // ── WATCHDOG ─────────────────────────────────────────────────────────────
  if (millis() - lastSuccess >= WATCHDOG_TIMEOUT_MS) {
    DBGLN("[WATCHDOG] No success in 60s. Resetting...");
    delay(1000);
    NVIC_SystemReset();
  }
}
