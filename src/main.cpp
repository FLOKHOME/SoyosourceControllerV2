// SoyosourceControllerV2 — main.cpp
// Verbesserte Firmware für ESP8266 → RS485 → Soyosource 1200 Wechselrichter
// Nulleinspeisungs-Regler mit Shelly 3EM Pro Anbindung
//
// Basierend auf Rekonstruktion aus firmware_v1.0.3.3.bin (KlausLi/BSGuy)
// RS485-Protokoll korrigiert nach macaron-software & syssi Referenz
//
// Pin-Belegung (Standard):
//   D1/GPIO5 → RS485 TX (über MAX485 DI)
//   D2/GPIO4 → RS485 RX (über MAX485 RO)
//   D3/GPIO0 → RS485 DE/RE (direction control)
//   D4/GPIO2 → LED Herzschlag (BUILTIN_LED)

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ElegantOTA.h>
#include <PubSubClient.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SoftwareSerial.h>
#include <ESP8266mDNS.h>

// ============================================================
// KONFIGURATION
// ============================================================

// Pins
#define PIN_RS485_TX   D1   // GPIO5
#define PIN_RS485_RX   D2   // GPIO4
#define PIN_RS485_DE   D5   // GPIO14 (DE/RE) — strap-frei, kein GPIO0/GPIO2/GPIO15!
#define PIN_LED        D4   // GPIO2 / BUILTIN_LED

// RS485
#define RS485_BAUD     9600
#define STATUS_FRAME_LEN  15
#define CMD_FRAME_LEN     6
#define TARGET_FRAME_LEN  8

// Timing
#define STATUS_INTERVAL_MS   2000   // Alle 2s Status abfragen
#define SEND_INTERVAL_MS     3000   // 3s Pause zwischen Sends
#define MQTT_INTERVAL_MS     15000  // MQTT alle 15s
#define WEB_REFRESH_MS       1500   // WebUI Auto-Refresh
#define MQTT_RECONNECT_MS    5000

// Toleranz Nulleinspeisung
#define TOLERANCE_MIN        -20    // -20W
#define TOLERANCE_MAX        +5     // +5W

// Hardware-Limit pro Soyo im Batterie-Modus (Batterie-CP)
#define SOYO_MAX_POWER_W     900

// ============================================================
// STRUKTUREN
// ============================================================

struct SoyoStatus {
  int    outputPower = 0;   // Aktuelle Ausgangsleistung (W)
  byte   opmode = 0;        // Betriebsmodus (high nibble: 1=CP, 2=PV, 4=STBY, 8=LIM)
  byte   errstat = 0;       // Error-Bitmask
  float  batteryV = 0;      // Batteriespannung (V)
  float  batteryA = 0;      // Batteriestrom (A)
  float  gridV = 0;         // Netzspannung (V)
  float  gridHz = 0;        // Netzfrequenz (Hz)
  float  tempC = 0;         // Temperatur (°C)
  int    frameCount = 0;    // Empfangene Frames
  int    errorCount = 0;    // Checksum-Fehler
};

struct AppConfig {
  // Allgemein
  char version[16]    = "2.0.0";
  char controllerName[48] = "SOYOSOURCE Controller";
  char controllerMode[24] = "AutoMqtt";        // AutoHttpInterface|AutoMqtt|AutoMqtt2|AutoShellyHttpClient|StaticHttpInterface
  bool notAus         = false;
  int  maxPower       = 1800;                  // Maximale Einspeiseleistung GESAMT (W) — Default 2×900W
  int  waitSekunden   = 3;                     // Pause zwischen Sends
  int  startTime      = 0;

  // SOYO
  char soyoID[16]     = "12345678";
  int  soyoCount      = 1;

  // Shelly 3EM
  char shellyIP[16]   = "192.168.2.237";
  bool shelly3emPro   = true;                  // true=Gen2 RPC, false=Gen1 /emeter/
  float L1            = 1.0;                   // Phasen-Korrekturfaktor
  float L2            = 1.0;
  float L3            = 1.0;

  // HTTP-Client
  char httpGetUrl[256] = "";
  char jsonValuePath[64] = "";

  // MQTT
  char mqttBroker[64] = "";
  int  mqttPort       = 1883;
  char mqttTopic[32]  = "soyosource";
  char topicL1L2L3[48]   = "soyosource/L1L2L3";
  char topicNotAus[48]   = "soyosource/NotAus";
  char topicWaitSek[48]  = "soyosource/WaitSekunden";
  char topicMaxPower[48] = "soyosource/MaxPower";
  char topicInfo[48]     = "soyosource/info";
  char topicData[48]     = "soyosource/data";

  // Live (nicht gespeichert)
  float dcVolts  = 0;
  float dcAmps   = 0;
  float dcWatts  = 0;
  float wattsOut = 0;
  int   targetPower = 0;    // Gesamtziel (alle Soyos zusammen)
  int   perSoyoPower = 0;   // Befehlswert pro Soyo (Gesamtziel / SoyoCount)
  float wifiRSSI = 0;
  char  wifiSSID[32] = "";

  // WLAN-Zugangsdaten (in config.json gespeichert — robust statt SDK-Persistenz)
  char apSSID[32] = "";
  char apPass[64] = "";
};

// ============================================================
// GLOBALE VARIABLEN
// ============================================================

AppConfig cfg;
SoyoStatus soyo;

SoftwareSerial rs485(PIN_RS485_RX, PIN_RS485_TX);  // RX, TX
AsyncWebServer server(80);
DNSServer dns;                    // Captive-Portal-DNS (AP-Modus)
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Status
bool notAusActive = false;
bool pendingNotAusSend = false; // NotAus-0W-Sendung in der loop() ausstehend
bool heartbeatState = false;
bool dcActiveState = false;
bool inAPMode = false;          // WLAN-Konfigurationsmodus aktiv
bool staBgActive = false;       // Dauer-Hintergrund-STA-Versuch läuft (AP-Modus)
unsigned long lastStaLog = 0;
unsigned long configSaveRebootMs = 0;  // Reboot nach Config-Speicherunge (loop-seitig)
int lastSentPower = 0;
int controllerTarget = 0;
bool configPortalRequested = false;

// Timing
unsigned long lastStatusMs = 0;
unsigned long lastSendMs = 0;
unsigned long lastMqttMs = 0;
unsigned long lastWebUpdateMs = 0;
unsigned long lastMqttReconnectMs = 0;
unsigned long heartbeatMs = 0;

// WebUI-Cache
String cachedJsonData;
unsigned long cacheValidUntil = 0;

// ============================================================
// FUNKTIONSDEKLARATIONEN
// ============================================================

void setupWiFi();
void setupWebServer();
void setupOTA();
void setupMQTT();
void handleMQTT();
void loadConfig();
void saveConfig();
bool soyoRequestStatus();
void soyoSetPower(int watts);
void soyoSendCommand(byte cmd, byte d0, byte d1, byte d2);
uint8_t soyoChecksumCmd(const uint8_t *data);
uint8_t soyoChecksumStatus(const uint8_t *data);
bool soyoParseStatus(const uint8_t *buf, size_t len);
int  shellyGetPower();
int  httpGetPower();
void processController();
void publishMQTT();
void publishDiscovery();
String buildJsonData();

// ============================================================
// RS485 PROTOKOLL — SOYOSOURCE
// ============================================================

// ESP → SOYO: 0x55 | CMD | D0 | D1 | D2 | CKSUM(0xFF - sum(B1..B4))
void soyoSendCommand(byte cmd, byte d0, byte d1, byte d2) {
  uint8_t frame[CMD_FRAME_LEN];
  frame[0] = 0x55;
  frame[1] = cmd;
  frame[2] = d0;
  frame[3] = d1;
  frame[4] = d2;
  frame[5] = soyoChecksumCmd(frame);

  digitalWrite(PIN_RS485_DE, HIGH);
  delay(2);
  rs485.write(frame, CMD_FRAME_LEN);
  rs485.flush();
  delay(2);
  digitalWrite(PIN_RS485_DE, LOW);
}

uint8_t soyoChecksumCmd(const uint8_t *data) {
  uint8_t cs = 0;
  for (int i = 1; i < 5; i++) cs += data[i];
  return 0xFF - cs;
}

uint8_t soyoChecksumStatus(const uint8_t *data) {
  uint8_t cs = 0;
  for (int i = 1; i < 14; i++) cs += data[i];
  return 0xFF - cs;
}

bool soyoParseStatus(const uint8_t *buf, size_t len) {
  if (len < STATUS_FRAME_LEN || buf[0] != 0xA6) return false;

  // Checksum
  if (soyoChecksumStatus(buf) != buf[14]) {
    soyo.errorCount++;
    return false;
  }

  // Byte 1-2: Output Power (big-endian)
  soyo.outputPower = (buf[1] << 8) | buf[2];
  // Byte 3: Opmode
  soyo.opmode = buf[3];
  // Byte 4: Errstat
  soyo.errstat = buf[4];
  // Byte 5-6: Battery Voltage (0.1V)
  soyo.batteryV = 0.1f * ((buf[5] << 8) | buf[6]);
  // Byte 7-8: Battery Current (0.1A)
  soyo.batteryA = 0.1f * ((buf[7] << 8) | buf[8]);
  // Byte 9-10: Grid Voltage (V)
  soyo.gridV = (float)((buf[9] << 8) | buf[10]);
  // Byte 11: Grid Frequency (0.5Hz)
  soyo.gridHz = 0.5f * buf[11];
  // Byte 12-13: Temperature (0.1°C, Offset -30°C)
  soyo.tempC = 0.1f * ((buf[12] << 8) | buf[13]) - 30.0f;

  soyo.frameCount++;
  dcActiveState = (soyo.outputPower > 0);
  return true;
}

bool soyoRequestStatus() {
  uint8_t buf[STATUS_FRAME_LEN];
  size_t idx = 0;

  // Vorherige Antwort aus Buffer lesen
  unsigned long start = millis();
  while (millis() - start < 30) {
    if (rs485.available() && idx < STATUS_FRAME_LEN) {
      buf[idx++] = rs485.read();
    }
  }
  if (idx >= STATUS_FRAME_LEN && soyoParseStatus(buf, idx)) {
    return true;
  }

  // Status-Request senden: 55 01 00 00 00 FE
  soyoSendCommand(0x01, 0x00, 0x00, 0x00);

  // Auf Antwort warten
  delay(80);
  idx = 0;
  start = millis();
  while (millis() - start < 300) {
    if (rs485.available() && idx < STATUS_FRAME_LEN) {
      buf[idx++] = rs485.read();
    }
  }

  return (idx >= STATUS_FRAME_LEN && soyoParseStatus(buf, idx));
}

void soyoSetPower(int watts) {
  // Leistung an Soyosource senden — Wert ist PRO SOYO (Gesamtziel / SoyoCount)
  // Befehl 0x13: Bat CP Mode Power setzen (in 10W-Schritten)
  // Hardware-Limit pro Soyo im Batteriemodus: 900W → 90 Einheiten à 10W
  int p = constrain(abs(watts), 0, SOYO_MAX_POWER_W);
  byte power10W = (byte)(p / 10);

  if (watts == 0 || notAusActive) {
    // NotAus: 55 13 00 64 00 XX → 0W
    soyoSendCommand(0x13, 0x00, 0x64, 0x00);
    // Zusätzlich Limiter-Off: 55 03 23 64 01 XX
    soyoSendCommand(0x03, 0x23, 0x64, 0x01);
  } else {
    // Leistung setzen
    soyoSendCommand(0x13, power10W, 0x64, 0x00);
    // Auf CP+Limiter Mode schalten: 55 02 02 00 12 XX
    soyoSendCommand(0x02, 0x02, 0x00, 0x12);
  }

  cfg.wattsOut = watts;
  lastSentPower = watts;
  lastSendMs = millis();

  // Soyosource braucht ~200ms zum Reagieren
  delay(200);
}

// ============================================================
// SHELLY 3EM PRO CLIENT
// ============================================================

int shellyGetPower() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  HTTPClient http;
  String url;

  if (cfg.shelly3emPro) {
    // Gen2 (Shelly Pro 3EM) RPC API
    url = "http://";
    url += cfg.shellyIP;
    url += "/rpc/EM.GetStatus?id=0";
  } else {
    // Tasmota API: http://IP/cm?cmnd=status+10
    url = "http://";
    url += cfg.shellyIP;
    url += "/cm?cmnd=status+10";
  }

  http.begin(wifiClient, url);
  http.setTimeout(2000);
  int httpCode = http.GET();

  if (httpCode != 200) {
    http.end();
    return -1;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return -1;

  float l1 = 0, l2 = 0, l3 = 0;

  if (cfg.shelly3emPro) {
    // Gen2 RPC: {"a_act_power":123, "b_act_power":456, "c_act_power":789}
    l1 = doc["a_act_power"] | 0.0f;
    l2 = doc["b_act_power"] | 0.0f;
    l3 = doc["c_act_power"] | 0.0f;
  } else {
    // Tasmota: {"StatusSNS":{"ENERGY":{"TotalStartTime":"...","Voltage":230,"Current":5.2,"Power":1200,"Total":12345,"Yesterday":...}}}
    // oder "/status": {"total_power": 1234}
    l1 = doc["StatusSNS"]["ENERGY"]["Power"] | 0.0f;
    if (l1 == 0) l1 = doc["total_power"] | doc["power"] | 0.0f;
  }

  float total = (l1 * cfg.L1) + (l2 * cfg.L2) + (l3 * cfg.L3);
  return (int)total;
}

// ============================================================
// HTTP CLIENT
// ============================================================

int httpGetPower() {
  if (WiFi.status() != WL_CONNECTED || strlen(cfg.httpGetUrl) == 0) return -1;

  HTTPClient http;
  http.begin(wifiClient, cfg.httpGetUrl);
  http.setTimeout(2000);
  int httpCode = http.GET();

  if (httpCode != 200) {
    http.end();
    return -1;
  }

  String payload = http.getString();
  http.end();

  if (strlen(cfg.jsonValuePath) > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, payload) == DeserializationError::Ok) {
      return doc[cfg.jsonValuePath].as<int>();
    }
    return -1;
  }

  return payload.toInt();
}

// ============================================================
// CONTROLLER — RULE-ENGINE
// ============================================================

void processController() {
  // Erster Regelzyklus: sauber 0W senden (definierter Startzustand nach Boot)
  static bool firstCycle = true;
  if (firstCycle) {
    firstCycle = false;
    soyoSetPower(0);
    return;
  }

  if (notAusActive) {
    if (lastSentPower != 0) {
      soyoSetPower(0);
    }
    return;
  }

  // 3-Sekunden-Pause einhalten
  if (millis() - lastSendMs < (unsigned long)cfg.waitSekunden * 1000) {
    return;
  }

  int newTarget = controllerTarget;

  if (strcmp(cfg.controllerMode, "AutoShellyHttpClient") == 0) {
    newTarget = shellyGetPower();
  } else if (strcmp(cfg.controllerMode, "AutoHttpInterface") == 0) {
    newTarget = httpGetPower();
  } else if (strcmp(cfg.controllerMode, "StaticHttpInterface") == 0) {
    // Statischer Wert — nichts tun
    return;
  }
  // AutoMqtt/AutoMqtt2: controllerTarget wird via MQTT-Callback gesetzt

  // Toleranzbereich: -20W bis +5W = Nulleinspeisung
  if (newTarget > TOLERANCE_MIN && newTarget < TOLERANCE_MAX) {
    newTarget = 0;
  }

  // Leistung auf die Soyos verteilen (Original-Verhalten):
  // gewünschte GESAMTleistung / SoyoCount, identischer Befehl an alle am Bus.
  // Integer-Division rundet ab → nie mehr einspeisen als nötig.
  int count = (cfg.soyoCount > 0) ? cfg.soyoCount : 1;

  // Negativ (Netz speist ein) → 0 einspeisen; Obergrenze = MaxPower,
  // hart gedeckelt durch Batteriemodus-Limit pro Soyo (900W × Count)
  if (newTarget < 0) newTarget = 0;
  int hardCap = SOYO_MAX_POWER_W * count;
  int softCap = min(cfg.maxPower, hardCap);
  newTarget = min(newTarget, softCap);

  int perSoyo = newTarget / count;

  // Nur senden wenn sich der Pro-Soyo-Wert geändert hat (min. 5W Differenz)
  if (abs(perSoyo - lastSentPower) >= 5 || perSoyo == 0) {
    if (perSoyo >= 0) {
      cfg.targetPower = newTarget;      // Gesamtziel
      cfg.perSoyoPower = perSoyo;       // Befehl pro Soyo
      soyoSetPower(perSoyo);
    }
  }
}

// ============================================================
// MQTT
// ============================================================

void setupMQTT() {
  if (strlen(cfg.mqttBroker) > 0) {
    mqtt.setServer(cfg.mqttBroker, cfg.mqttPort);
    mqtt.setCallback([](char *topic, byte *payload, unsigned int length) {
      String msg;
      for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
      String t = String(topic);

      if (t == cfg.topicL1L2L3 || t.endsWith("/L1L2L3")) {
        controllerTarget = msg.toInt();
      } else if (t == cfg.topicNotAus || t.endsWith("/NotAus")) {
        notAusActive = (msg == "1" || msg.startsWith("ON"));
        cfg.notAus = notAusActive;
        saveConfig();   // NotAus-Zustand persistent halten (Stromausfall-sicher)
      } else if (t == cfg.topicWaitSek || t.endsWith("/WaitSekunden")) {
        cfg.waitSekunden = constrain(msg.toInt(), 1, 10);
        saveConfig();
      } else if (t == cfg.topicMaxPower || t.endsWith("/MaxPower")) {
        int count = (cfg.soyoCount > 0) ? cfg.soyoCount : 1;
        cfg.maxPower = constrain(msg.toInt(), 0, SOYO_MAX_POWER_W * count);
        saveConfig();
      }
    });
  }
}

void handleMQTT() {
  if (strlen(cfg.mqttBroker) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqtt.connected()) {
    if (millis() - lastMqttReconnectMs > MQTT_RECONNECT_MS) {
      lastMqttReconnectMs = millis();
      String clientId = "Soyosource-" + String(ESP.getChipId(), HEX);
      if (mqtt.connect(clientId.c_str())) {
        mqtt.subscribe(cfg.topicL1L2L3);
        mqtt.subscribe(cfg.topicNotAus);
        mqtt.subscribe(cfg.topicWaitSek);
        mqtt.subscribe(cfg.topicMaxPower);
        publishDiscovery();
      }
    }
  } else {
    mqtt.loop();
  }
}

void publishMQTT() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["ControllerName"] = cfg.controllerName;
  doc["Version"] = cfg.version;
  doc["ControllerMode"] = cfg.controllerMode;
  doc["SOYOID"] = cfg.soyoID;
  doc["NotAus"] = notAusActive;
  doc["MaxPower"] = cfg.maxPower;
  doc["DCVolts"] = soyo.batteryV;
  doc["DCAmps"] = soyo.batteryA;
  doc["DCWatts"] = soyo.outputPower;
  doc["WattsOut"] = cfg.wattsOut;
  doc["targetPower"] = cfg.targetPower;
  doc["WIFI_RSSI"] = WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(cfg.topicInfo, payload.c_str());
  mqtt.publish(cfg.topicData, payload.c_str());
}

void publishDiscovery() {
  // Home Assistant MQTT Auto-Discovery
  String stateTopic = String(cfg.mqttTopic) + "/state";
  String deviceName = "soyosource";
  String mac = WiFi.macAddress();
  mac.replace(":", "");

  auto publishSensor = [&](const char *name, const char *cls, const char *unit, const char *tpl) {
    JsonDocument doc;
    doc["name"] = String(deviceName) + " " + name;
    doc["uniq_id"] = deviceName + "_" + name;
    doc["stat_t"] = stateTopic;
    doc["unit_of_meas"] = unit;
    doc["device_class"] = cls;
    doc["val_tpl"] = String("{{ value_json.") + name + " | default(0) }}";
    JsonObject dev = doc.createNestedObject("device");
    dev["ids"][0] = mac;
    dev["cu"] = "http://" + WiFi.localIP().toString();
    dev["mdl"] = "ESP8266 Soyosource";

    char buffer[512];
    size_t n = serializeJson(doc, buffer);
    String discTopic = "homeassistant/sensor/" + deviceName + "_" + name + "/config";
    mqtt.publish(discTopic.c_str(), (const uint8_t*)buffer, n, true);
  };

  publishSensor("Power", "power", "W", "Power");
  publishSensor("DCVolts", "voltage", "V", "DCVolts");
  publishSensor("DCAmps", "current", "A", "DCAmps");
  publishSensor("WattsOut", "power", "W", "WattsOut");
  publishSensor("targetPower", "power", "W", "targetPower");
  publishSensor("Temperature", "temperature", "°C", "Temperature");
}

// ============================================================
// KONFIGURATION — LITTLEFS
// ============================================================

void loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("[FS] LittleFS-Mount FEHLGESCHLAGEN!");
  }
  if (!LittleFS.exists("/config.json")) {
    Serial.println("[FS] config.json fehlt, schreibe Defaults");
    saveConfig();  // Defaults speichern
    return;
  }

  File f = LittleFS.open("/config.json", "r");
  if (!f) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  strlcpy(cfg.version,        doc["version"]        | "2.0.0", sizeof(cfg.version));
  strlcpy(cfg.controllerName, doc["controllerName"] | "SOYOSOURCE Controller", sizeof(cfg.controllerName));
  strlcpy(cfg.controllerMode, doc["controllerMode"] | "AutoMqtt", sizeof(cfg.controllerMode));
  strlcpy(cfg.soyoID,         doc["soyoID"]         | "12345678", sizeof(cfg.soyoID));
  strlcpy(cfg.shellyIP,       doc["shellyIP"]       | "192.168.2.237", sizeof(cfg.shellyIP));
  strlcpy(cfg.httpGetUrl,     doc["httpGetUrl"]     | "", sizeof(cfg.httpGetUrl));
  strlcpy(cfg.jsonValuePath,  doc["jsonValuePath"]  | "", sizeof(cfg.jsonValuePath));
  strlcpy(cfg.mqttBroker,     doc["mqttBroker"]     | "", sizeof(cfg.mqttBroker));
  strlcpy(cfg.mqttTopic,      doc["mqttTopic"]      | "soyosource", sizeof(cfg.mqttTopic));
  strlcpy(cfg.topicL1L2L3,    doc["topicL1L2L3"]    | "soyosource/L1L2L3", sizeof(cfg.topicL1L2L3));
  strlcpy(cfg.topicNotAus,    doc["topicNotAus"]    | "soyosource/NotAus", sizeof(cfg.topicNotAus));
  strlcpy(cfg.topicWaitSek,   doc["topicWaitSek"]   | "soyosource/WaitSekunden", sizeof(cfg.topicWaitSek));
  strlcpy(cfg.topicMaxPower,  doc["topicMaxPower"]  | "soyosource/MaxPower", sizeof(cfg.topicMaxPower));
  strlcpy(cfg.topicInfo,      doc["topicInfo"]      | "soyosource/info", sizeof(cfg.topicInfo));
  strlcpy(cfg.topicData,      doc["topicData"]      | "soyosource/data", sizeof(cfg.topicData));
  strlcpy(cfg.apSSID,         doc["wifiSSID"]       | "", sizeof(cfg.apSSID));
  strlcpy(cfg.apPass,         doc["wifiPass"]       | "", sizeof(cfg.apPass));

  cfg.notAus          = doc["notAus"]         | false;
  cfg.maxPower        = doc["maxPower"]       | 2400;
  cfg.waitSekunden    = doc["waitSekunden"]   | 3;
  cfg.soyoCount       = doc["soyoCount"]      | 1;
  cfg.shelly3emPro    = doc["shelly3emPro"]   | true;
  cfg.L1              = doc["L1"]             | 1.0f;
  cfg.L2              = doc["L2"]             | 1.0f;
  cfg.L3              = doc["L3"]             | 1.0f;
  cfg.mqttPort        = doc["mqttPort"]       | 1883;
}

void saveConfig() {
  JsonDocument doc;
  doc["version"]         = cfg.version;
  doc["controllerName"]  = cfg.controllerName;
  doc["controllerMode"]  = cfg.controllerMode;
  doc["soyoID"]          = cfg.soyoID;
  doc["shellyIP"]        = cfg.shellyIP;
  doc["httpGetUrl"]      = cfg.httpGetUrl;
  doc["jsonValuePath"]   = cfg.jsonValuePath;
  doc["mqttBroker"]      = cfg.mqttBroker;
  doc["mqttPort"]        = cfg.mqttPort;
  doc["mqttTopic"]       = cfg.mqttTopic;
  doc["topicL1L2L3"]     = cfg.topicL1L2L3;
  doc["topicNotAus"]     = cfg.topicNotAus;
  doc["topicWaitSek"]    = cfg.topicWaitSek;
  doc["topicMaxPower"]   = cfg.topicMaxPower;
  doc["topicInfo"]       = cfg.topicInfo;
  doc["topicData"]       = cfg.topicData;
  doc["wifiSSID"]        = cfg.apSSID;
  doc["wifiPass"]        = cfg.apPass;
  doc["notAus"]          = cfg.notAus;
  doc["maxPower"]        = cfg.maxPower;
  doc["waitSekunden"]    = cfg.waitSekunden;
  doc["soyoCount"]       = cfg.soyoCount;
  doc["shelly3emPro"]    = cfg.shelly3emPro;
  doc["L1"]              = cfg.L1;
  doc["L2"]              = cfg.L2;
  doc["L3"]              = cfg.L3;

  File f = LittleFS.open("/config.json", "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
}

// ============================================================
// WLAN-ZIEL-FINDUNG
// ============================================================

struct WifiTarget {
  uint8_t bssid[6];
  uint8_t channel;
  int rssi;
};

// Findet das VERSCHLÜSSELTE AP mit der konfigurierten SSID (höchster RSSI).
// Wichtig bei SSID-Kollisionen: gleichnamige offene Gäste-Netze würden
// WPA2-Auth-Versuche scheitern lassen (WL_WRONG_PASSWORD-Dauerschleife).
bool findWifiTarget(struct WifiTarget &t) {
  int n = WiFi.scanNetworks();
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != String(cfg.apSSID)) continue;
    if (WiFi.encryptionType(i) == ENC_TYPE_NONE) continue;  // offen → überspringen
    if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
  }
  if (best < 0) {
    WiFi.scanDelete();
    return false;
  }
  memcpy(t.bssid, WiFi.BSSID(best), 6);
  t.channel = WiFi.channel(best);
  t.rssi = WiFi.RSSI(best);
  Serial.printf("[WiFi] Ziel-AP: BSSID %02X:%02X:%02X:%02X:%02X:%02X Kanal %d RSSI %d\n",
                t.bssid[0], t.bssid[1], t.bssid[2], t.bssid[3], t.bssid[4], t.bssid[5],
                t.channel, t.rssi);
  WiFi.scanDelete();
  return true;
}

// ============================================================
// CONFIG-UI (PROGMEM) — Einstellungsseite
// ============================================================

const char CONFIG_PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang='de'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>Config — SOYOSOURCE Controller</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',Arial,sans-serif;background:#0d0d1a;color:#e0e0e0;padding:16px;max-width:560px;margin:0 auto}
h1{color:#0f3460;font-size:1.3em;margin:10px 0}
h2{color:#4fc3f7;font-size:.95em;margin:16px 0 8px;padding-bottom:4px;border-bottom:1px solid #1f1f3a;text-transform:uppercase;letter-spacing:1px}
label{display:block;color:#888;font-size:.85em;margin:8px 0 2px}
input,select{width:100%;padding:10px;border:2px solid #0f3460;border-radius:6px;background:#1a1a2e;color:#e0e0e0;font-size:1em}
input:focus,select:focus{outline:none;border-color:#4fc3f7}
.grid{display:flex;gap:8px}.grid>div{flex:1}
.check{display:flex;align-items:center;gap:8px;margin:10px 0}.check input{width:auto}
button{width:100%;padding:14px;margin-top:16px;border:none;border-radius:8px;background:#2e7d32;color:#fff;font-size:1.05em;font-weight:bold;cursor:pointer}
button:active{transform:scale(.98)}
.note{color:#666;font-size:.78em;margin-top:4px}
#status{text-align:center;margin-top:12px;min-height:1.2em;color:#4ecca3}
</style>
</head>
<body>
<h1>⚙️ SOYOSOURCE Controller — Einstellungen</h1>

<h2>Allgemein</h2>
<label>Name</label><input id='controllerName'>
<label>Betriebsmodus</label>
<select id='controllerMode'>
  <option value='AutoMqtt'>Auto Mqtt</option>
  <option value='AutoMqtt2'>Auto Mqtt2</option>
  <option value='AutoShellyHttpClient'>Auto Shelly HttpClient</option>
  <option value='AutoHttpInterface'>Auto HttpInterface</option>
  <option value='StaticHttpInterface'>Static HttpInterface</option>
</select>
<div class='grid'>
  <div><label>MaxPower (W, max = 900×Count)</label><input id='maxPower' type='number' min='0' max='1800'></div>
  <div><label>WaitSekunden</label><input id='waitSekunden' type='number' min='1' max='10'></div>
</div>

<h2>SOYO</h2>
<div class='grid'>
  <div><label>SOYO ID</label><input id='soyoID'></div>
  <div><label>SoyoCount (1-8, max 900W/Soyo)</label><input id='soyoCount' type='number' min='1' max='8' onchange="document.getElementById('maxPower').max = 900*this.value"></div>
</div>

<h2>Shelly / Tasmota</h2>
<label>IP-Adresse</label><input id='shellyIP'>
<div class='check'><input type='checkbox' id='shelly3emPro'><label for='shelly3emPro' style='margin:0'>Shelly 3EM Pro / Gen2 RPC (sonst Tasmota-API)</label></div>
<div class='grid'>
  <div><label>L1-Faktor</label><input id='L1' type='number' step='0.01'></div>
  <div><label>L2-Faktor</label><input id='L2' type='number' step='0.01'></div>
  <div><label>L3-Faktor</label><input id='L3' type='number' step='0.01'></div>
</div>

<h2>HTTP-Client (AutoHttpInterface)</h2>
<label>HttpGetUrl</label><input id='httpGetUrl' placeholder='http://...'>
<label>JsonValuePath</label><input id='jsonValuePath' placeholder='z.B. power'>

<h2>MQTT</h2>
<div class='grid'>
  <div><label>Broker</label><input id='mqttBroker'></div>
  <div><label>Port</label><input id='mqttPort' type='number'></div>
</div>
<label>Topic-Basis</label><input id='mqttTopic'>

<h2>MQTT-Topics</h2>
<label>topicL1L2L3</label><input id='topicL1L2L3'>
<label>topicNotAus</label><input id='topicNotAus'>
<div class='grid'>
  <div><label>topicWaitSek</label><input id='topicWaitSek'></div>
  <div><label>topicMaxPower</label><input id='topicMaxPower'></div>
</div>
<div class='grid'>
  <div><label>topicInfo</label><input id='topicInfo'></div>
  <div><label>topicData</label><input id='topicData'></div>
</div>

<h2>WLAN</h2>
<label>SSID (leer = unverändert)</label><input id='wifiSSID'>
<label>Passwort (leer = unverändert)</label><input id='wifiPass' type='password'>

<button onclick='save()'>💾 Speichern &amp; Neustart</button>
<div id='status'></div>
<p class='note'>Nach dem Speichern startet der ESP neu. WLAN-Felder leer lassen, um die Verbindung nicht zu ändern.</p>

<script>
async function load(){
  try{
    const d = await (await fetch('/config.json')).json();
    for(const k of ['controllerName','controllerMode','maxPower','waitSekunden','soyoID','soyoCount',
      'shellyIP','httpGetUrl','jsonValuePath','mqttBroker','mqttPort','mqttTopic',
      'topicL1L2L3','topicNotAus','topicWaitSek','topicMaxPower','topicInfo','topicData','wifiSSID']){
      const el = document.getElementById(k);
      if(el && d[k] !== undefined) el.value = d[k];
    }
    document.getElementById('shelly3emPro').checked = !!d.shelly3emPro;
    document.getElementById('L1').value = d.L1 ?? 1;
    document.getElementById('L2').value = d.L2 ?? 1;
    document.getElementById('L3').value = d.L3 ?? 1;
  }catch(e){ document.getElementById('status').textContent = 'Config konnte nicht geladen werden'; }
}
async function save(){
  const g = id => document.getElementById(id).value;
  const body = {
    controllerName: g('controllerName'), controllerMode: g('controllerMode'),
    maxPower: parseInt(g('maxPower'))||0, waitSekunden: parseInt(g('waitSekunden'))||3,
    soyoID: g('soyoID'), soyoCount: parseInt(g('soyoCount'))||1,
    shellyIP: g('shellyIP'), shelly3emPro: document.getElementById('shelly3emPro').checked,
    L1: parseFloat(g('L1'))||1, L2: parseFloat(g('L2'))||1, L3: parseFloat(g('L3'))||1,
    httpGetUrl: g('httpGetUrl'), jsonValuePath: g('jsonValuePath'),
    mqttBroker: g('mqttBroker'), mqttPort: parseInt(g('mqttPort'))||1883, mqttTopic: g('mqttTopic'),
    topicL1L2L3: g('topicL1L2L3'), topicNotAus: g('topicNotAus'),
    topicWaitSek: g('topicWaitSek'), topicMaxPower: g('topicMaxPower'),
    topicInfo: g('topicInfo'), topicData: g('topicData')
  };
  if(g('wifiSSID')) body.wifiSSID = g('wifiSSID');
  if(g('wifiPass')) body.wifiPass = g('wifiPass');
  document.getElementById('status').textContent = 'Speichere...';
  try{
    const r = await fetch('/config',{method:'POST',body:JSON.stringify(body)});
    document.getElementById('status').textContent = (r.ok) ? '✅ Gespeichert — ESP startet neu...' : '❌ Fehler: '+await r.text();
  }catch(e){ document.getElementById('status').textContent = 'Gespeichert (Verbindung getrennt = Reboot)'; }
}
load();
</script>
</body>
</html>
)rawliteral";

// ============================================================
// SETUP — WIFI + WEBSERVER
// ============================================================

void setupWiFi() {
  // Hostname für Router/FritzBox-Anzeige (muss vor dem WLAN-Start gesetzt werden)
  WiFi.setHostname("soyosource");

  // Hinweis: Kein D3/GPIO0-Trigger für das Config-Portal!
  // GPIO0 ist der Boot-Strapping-Pin (FLASH-Button am D1 mini) und hängt an der
  // DTR-Leitung von USB-Seriell-Adaptern — jedes serielle Terminal mit DTR-Assert
  // würde das Portal fälschlich auslösen. Config-Portal erreicht man über:
  //   a) automatisch, wenn WLAN-Verbindung fehlschlägt / keine Credentials
  //   b) WebUI: /wifi
  //   c) Serial: "wifi:SSID,PASSWORT"
  configPortalRequested = false;

  // Gespeicherte WLAN-Credentials? (aus config.json — LittleFS)
  bool hasCreds = (strlen(cfg.apSSID) > 0 && strlen(cfg.apPass) > 0);

  if (!configPortalRequested && hasCreds) {
    WiFi.mode(WIFI_STA);
    Serial.print("Verbinde mit WLAN '");
    Serial.print(cfg.apSSID);
    Serial.println("'");

    // BSSID-Scan: verschlüsseltes AP mit dieser SSID suchen (Same-SSID-Kollisionen!)
    struct WifiTarget t;
    bool locked = findWifiTarget(t);

    if (locked) {
      WiFi.begin(cfg.apSSID, cfg.apPass, t.channel, t.bssid);
    } else {
      Serial.println("[WiFi] kein verschlüsseltes AP gefunden — offener Versuch");
      WiFi.begin(cfg.apSSID, cfg.apPass);
    }

    // Robuster Verbindungsversuch: bis 45s, begin() alle 10s neu auslösen.
    // Beobachtet: Router (vermutlich Band-Steering) braucht teils 20-40s
    // durchgehende Versuche, bevor er den 2,4-GHz-Client durchlässt.
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 90) {
      if (attempts % 20 == 19) {  // alle 10s neu beginnen
        Serial.println("[WiFi] neu versuchen");
        WiFi.disconnect();
        WiFi.mode(WIFI_STA);
        if (locked) {
          WiFi.begin(cfg.apSSID, cfg.apPass, t.channel, t.bssid);
        } else {
          WiFi.begin(cfg.apSSID, cfg.apPass);
        }
      }
      delay(500);
      if (attempts % 4 == 3) {
        Serial.print("[WiFi] Status=");
        Serial.print(WiFi.status());
        Serial.print(" RSSI=");
        Serial.println(WiFi.RSSI());
      }
      attempts++;
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("[WiFi] Verbindung fehlgeschlagen nach 30s, letzter Status=");
      Serial.println(WiFi.status());
      Serial.println("[WiFi] Codes: 1=SSID nicht gefunden  4=Verbindungsfehler  7=FALSCHES PASSWORT");
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    // REINER AP-Modus (kein STA!): WIFI_AP_STA-Kanal-Hopping zerstört die Beacons
    // und Pixel-Handys brechen die Assoziation ab. Captive-Portal-DNS sorgt dafür,
    // dass Android das "Anmelden"-Fenster automatisch öffnet.
    inAPMode = true;
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP("SOYOSOURCE-Controller");
    dns.start(53, "*", IPAddress(192, 168, 4, 1));
    Serial.print("AP-Modus: 'SOYOSOURCE-Controller' (offen, Captive Portal) @ ");
    Serial.println(WiFi.softAPIP().toString());
  } else {
    Serial.print("WLAN verbunden: ");
    Serial.print(WiFi.SSID());
    Serial.print(" IP: ");
    Serial.println(WiFi.localIP().toString());
  }
}

void setupWebServer() {
  // Hauptseite
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
  });

  // WLAN-Konfigurationsseite (AP-Modus)
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                  "<title>WLAN Setup</title><style>"
                  "body{font-family:Arial;background:#0d0d1a;color:#eee;padding:20px;max-width:420px;margin:0 auto}"
                  "h2{color:#0f3460}input{width:100%;padding:10px;margin:6px 0;border:2px solid #0f3460;border-radius:6px;background:#1a1a2e;color:#eee;font-size:16px}"
                  "button{width:100%;padding:12px;margin-top:10px;border:none;border-radius:6px;background:#e94560;color:#fff;font-size:16px;cursor:pointer}"
                  "</style></head><body><h2>WLAN-Konfiguration</h2>"
                  "<form method='get' action='/wifisave'>"
                  "<label>SSID</label><input name='s' length='32' placeholder='WLAN-Name'>"
                  "<label>Passwort</label><input name='p' length='64' type='password' placeholder='Passwort'>"
                  "<button>Speichern &amp; Neustart</button></form>"
                  "<p style='color:#888;font-size:13px'>Aktueller Modus: " + String(inAPMode ? "AP (Konfiguration)" : "Station") + "</p>"
                  "</body></html>";
    request->send(200, "text/html", html);
  });

  // WLAN-Credentials speichern + Reboot (LittleFS — robust statt SDK-Persistenz)
  server.on("/wifisave", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("s") && request->hasParam("p")) {
      String ssid = request->getParam("s")->value();
      String pass = request->getParam("p")->value();
      strlcpy(cfg.apSSID, ssid.c_str(), sizeof(cfg.apSSID));
      strlcpy(cfg.apPass, pass.c_str(), sizeof(cfg.apPass));
      saveConfig();
      Serial.print("WLAN gespeichert: ");
      Serial.println(ssid);
    }
    request->send(200, "text/html",
                  "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='8; url=/'></head>"
                  "<body style='font-family:Arial;background:#0d0d1a;color:#eee;padding:30px;text-align:center'>"
                  "<h2 style='color:#4ecca3'>WLAN gespeichert</h2><p>ESP startet neu und verbindet sich.<br>"
                  "IP-Adresse danach via Router oder Serial ermitteln.</p></body></html>");
    delay(1500);
    ESP.restart();
  });

  // WLAN-Scan — im reinen AP-Modus nicht möglich (manuelle SSID-Eingabe)
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (inAPMode) {
      request->send(200, "application/json", "{\"hint\":\"Im AP-Modus SSID manuell eingeben\"}");
      return;
    }
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
      request->send(202, "text/plain", "scan läuft");
      return;
    }
    if (n == WIFI_SCAN_FAILED) {
      WiFi.scanNetworks(true);  // async scan starten
      request->send(202, "text/plain", "scan gestartet, in 3s erneut laden");
      return;
    }
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    request->send(200, "application/json", json);
  });

  // JSON-API
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (millis() < cacheValidUntil && cachedJsonData.length() > 0) {
      request->send(200, "application/json", cachedJsonData);
      return;
    }
    String json = buildJsonData();
    cachedJsonData = json;
    cacheValidUntil = millis() + 500;
    request->send(200, "application/json", json);
  });

  // Manuelle Leistung
  server.on("/dataonce", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("value")) {
      int count = (cfg.soyoCount > 0) ? cfg.soyoCount : 1;
      int cap = min(cfg.maxPower, SOYO_MAX_POWER_W * count);
      int val = constrain(request->getParam("value")->value().toInt(), 0, cap);
      controllerTarget = val;
      cfg.targetPower = val;
      notAusActive = false;
    }
    request->redirect("/");
  });

  // NotAus — NICHT blockierend + PERSISTENT (überlebt Stromausfall, wie Original v1.0.3.3)
  server.on("/NotAus", HTTP_GET, [](AsyncWebServerRequest *request) {
    notAusActive = true;
    cfg.notAus = true;
    saveConfig();
    pendingNotAusSend = true;
    request->redirect("/");
  });

  server.on("/NotAusAus", HTTP_GET, [](AsyncWebServerRequest *request) {
    notAusActive = false;
    cfg.notAus = false;
    saveConfig();
    request->redirect("/");
  });

  // L1L2L3 Auto/Static
  server.on("/L1L2L3Auto", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("Value")) {
      float v = request->getParam("Value")->value().toFloat();
      cfg.L1 = cfg.L2 = cfg.L3 = v;
      saveConfig();
    }
    request->redirect("/");
  });

  server.on("/L1L2L3Static", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("Value")) {
      float v = request->getParam("Value")->value().toFloat();
      cfg.L1 = cfg.L2 = cfg.L3 = v;
      saveConfig();
    }
    request->redirect("/");
  });

  // MaxPower
  server.on("/MaxPower", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("Value")) {
      int count = (cfg.soyoCount > 0) ? cfg.soyoCount : 1;
      cfg.maxPower = constrain(request->getParam("Value")->value().toInt(), 0, SOYO_MAX_POWER_W * count);
      saveConfig();
    }
    request->redirect("/");
  });

  // ReSeT
  server.on("/ReSeT", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Rebooting...");
    delay(500);
    ESP.restart();
  });

  // Close
  server.on("/close", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->redirect("/");
  });

  // Config-Seite: echte Settings-GUI (Formular, Prefill aus /config.json)
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", CONFIG_PAGE_HTML);
  });

  // Speichern: roher JSON-Body (vom Config-UI gepostet)
  server.on("/config", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // Antwort kommt im onBody-Abschluss
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      static String body;
      static bool saving = false;
      if (index == 0) { body = ""; saving = false; }
      body.concat((const char *)data, len);

      if (index + len == total && !saving) {
        saving = true;
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        body = String();  // Speicher freigeben
        if (err) {
          request->send(400, "text/plain", "JSON-Fehler");
          return;
        }

        // Nur vorhandene Schlüssel übernehmen (Defaults = aktuelle Werte)
        if (doc["controllerName"].is<const char*>()) strlcpy(cfg.controllerName, doc["controllerName"] | cfg.controllerName, sizeof(cfg.controllerName));
        if (doc["controllerMode"].is<const char*>()) strlcpy(cfg.controllerMode, doc["controllerMode"] | cfg.controllerMode, sizeof(cfg.controllerMode));
        if (doc["soyoID"].is<const char*>())         strlcpy(cfg.soyoID, doc["soyoID"] | cfg.soyoID, sizeof(cfg.soyoID));
        if (doc["shellyIP"].is<const char*>())       strlcpy(cfg.shellyIP, doc["shellyIP"] | cfg.shellyIP, sizeof(cfg.shellyIP));
        if (doc["httpGetUrl"].is<const char*>())     strlcpy(cfg.httpGetUrl, doc["httpGetUrl"] | cfg.httpGetUrl, sizeof(cfg.httpGetUrl));
        if (doc["jsonValuePath"].is<const char*>())  strlcpy(cfg.jsonValuePath, doc["jsonValuePath"] | cfg.jsonValuePath, sizeof(cfg.jsonValuePath));
        if (doc["mqttBroker"].is<const char*>())     strlcpy(cfg.mqttBroker, doc["mqttBroker"] | cfg.mqttBroker, sizeof(cfg.mqttBroker));
        if (doc["mqttTopic"].is<const char*>())      strlcpy(cfg.mqttTopic, doc["mqttTopic"] | cfg.mqttTopic, sizeof(cfg.mqttTopic));
        if (doc["topicL1L2L3"].is<const char*>())    strlcpy(cfg.topicL1L2L3, doc["topicL1L2L3"] | cfg.topicL1L2L3, sizeof(cfg.topicL1L2L3));
        if (doc["topicNotAus"].is<const char*>())    strlcpy(cfg.topicNotAus, doc["topicNotAus"] | cfg.topicNotAus, sizeof(cfg.topicNotAus));
        if (doc["topicWaitSek"].is<const char*>())   strlcpy(cfg.topicWaitSek, doc["topicWaitSek"] | cfg.topicWaitSek, sizeof(cfg.topicWaitSek));
        if (doc["topicMaxPower"].is<const char*>())  strlcpy(cfg.topicMaxPower, doc["topicMaxPower"] | cfg.topicMaxPower, sizeof(cfg.topicMaxPower));
        if (doc["topicInfo"].is<const char*>())      strlcpy(cfg.topicInfo, doc["topicInfo"] | cfg.topicInfo, sizeof(cfg.topicInfo));
        if (doc["topicData"].is<const char*>())      strlcpy(cfg.topicData, doc["topicData"] | cfg.topicData, sizeof(cfg.topicData));

      // MaxPower: hart gedeckelt durch Batteriemodus-Limit pro Soyo (900W × Count)
      int mpCount = (cfg.soyoCount > 0) ? cfg.soyoCount : 1;
      if (doc["maxPower"].is<int>()) cfg.maxPower = constrain(doc["maxPower"] | cfg.maxPower, 0, SOYO_MAX_POWER_W * mpCount);
        if (doc["waitSekunden"].is<int>()) cfg.waitSekunden = constrain(doc["waitSekunden"] | cfg.waitSekunden, 1, 10);
        if (doc["soyoCount"].is<int>())    cfg.soyoCount = doc["soyoCount"] | cfg.soyoCount;
        if (doc["mqttPort"].is<int>())     cfg.mqttPort = doc["mqttPort"] | cfg.mqttPort;
        if (doc["shelly3emPro"].is<bool>()) cfg.shelly3emPro = doc["shelly3emPro"] | cfg.shelly3emPro;
        if (doc["L1"].is<float>()) cfg.L1 = doc["L1"] | cfg.L1;
        if (doc["L2"].is<float>()) cfg.L2 = doc["L2"] | cfg.L2;
        if (doc["L3"].is<float>()) cfg.L3 = doc["L3"] | cfg.L3;

        // WLAN: nur überschreiben wenn ausgefüllt (leer = unverändert)
        if (doc["wifiSSID"].is<const char*>() && strlen(doc["wifiSSID"] | "") > 0) {
          strlcpy(cfg.apSSID, doc["wifiSSID"] | "", sizeof(cfg.apSSID));
        }
        if (doc["wifiPass"].is<const char*>() && strlen(doc["wifiPass"] | "") > 0) {
          strlcpy(cfg.apPass, doc["wifiPass"] | "", sizeof(cfg.apPass));
        }

        saveConfig();
        Serial.println("[Config] gespeichert via Web — Reboot in 1,5s");
        request->send(200, "text/plain", "OK — gespeichert. ESP startet neu...");
        configSaveRebootMs = millis() + 1500;   // Reboot via loop() (nicht im Async-Task!)
      }
    });

  // Captive-Portal-Fallback: unbekannte URLs (Android-/generate_204, Apple /hotspot-detect.html,
  // Windows /connecttest.txt) ins Portal umleiten — nur im AP-Modus, sonst 404.
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (inAPMode && request->method() == HTTP_GET) {
      request->redirect("http://192.168.4.1/wifi");
      return;
    }
    request->send(404, "text/plain", "Not Found");
  });

  server.serveStatic("/", LittleFS, "/");
}

void setupOTA() {
  ElegantOTA.begin(&server);
  ElegantOTA.setAutoReboot(true);
}

// ============================================================
// JSON DATA BUILDER
// ============================================================

String buildJsonData() {
  JsonDocument doc;
  doc["ControllerMode"]  = cfg.controllerMode;
  doc["ControllerName"]  = cfg.controllerName;
  doc["Version"]         = cfg.version;
  doc["SOYOID"]          = cfg.soyoID;
  doc["NotAus"]          = notAusActive;
  doc["MaxPower"]        = cfg.maxPower;
  doc["WaitSekunden"]    = cfg.waitSekunden;
  doc["ShellyIP"]        = cfg.shellyIP;
  doc["S3emPro"]         = cfg.shelly3emPro;
  doc["L1"]              = cfg.L1;
  doc["L2"]              = cfg.L2;
  doc["L3"]              = cfg.L3;
  doc["DCVolts"]         = soyo.batteryV;
  doc["DCAmps"]          = soyo.batteryA;
  doc["DCWatts"]         = soyo.outputPower;
  doc["WattsOut"]        = cfg.wattsOut;
  doc["targetPower"]     = cfg.targetPower;
  doc["perSoyoPower"]    = cfg.perSoyoPower;
  doc["lastSentPower"]   = lastSentPower;
  doc["WIFI_RSSI"]       = WiFi.RSSI();
  doc["WIFI_SSID"]       = WiFi.SSID();
  doc["frameCount"]      = soyo.frameCount;
  doc["errorCount"]      = soyo.errorCount;
  doc["opmode"]          = soyo.opmode;
  doc["gridV"]           = soyo.gridV;
  doc["gridHz"]          = soyo.gridHz;
  doc["tempC"]           = soyo.tempC;
  doc["dcActive"]        = dcActiveState;
  doc["heartbeat"]       = heartbeatState;

  String json;
  serializeJson(doc, json);
  return json;
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("SOYOSOURCE Controller v2.0.0");
  Serial.println("=============================");

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  pinMode(PIN_RS485_DE, OUTPUT);
  digitalWrite(PIN_RS485_DE, LOW);

  // RS485
  rs485.begin(RS485_BAUD);

  // Config laden
  loadConfig();

  // NotAus-Zustand wiederherstellen (persistent — Sicherheitsverhalten bei Stromausfall)
  notAusActive = cfg.notAus;

  // WiFi + Web
  setupWiFi();
  setupWebServer();
  setupOTA();

  // MQTT
  setupMQTT();

  server.begin();

  // mDNS: http://soyosource.local (erleichtert das Wiederfinden im Netz)
  if (MDNS.begin("soyosource")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS aktiv: http://soyosource.local");
  }

  Serial.print("HTTP server started @ ");
  Serial.println(WiFi.localIP().toString().length() > 0 ? WiFi.localIP().toString() : WiFi.softAPIP().toString());

  Serial.println("Soyo Controller is Ready:");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  ElegantOTA.loop();
  handleMQTT();
  MDNS.update();

  unsigned long now = millis();

  // Reboot nach Config-Speicherung (aus dem Async-Handler heraus verplant)
  if (configSaveRebootMs && millis() > configSaveRebootMs) {
    configSaveRebootMs = 0;
    ESP.restart();
  }

  // Captive-Portal-DNS nur im AP-Modus bedienen
  if (inAPMode) {
    dns.processNextRequest();

    // Selbstheilung: Im AP-Fallback DAUERHAFT STA in AP_STA probieren.
    // (Fenster-Versuche mit 15s waren zu kurz — der Router lässt 2,4-GHz-Clients
    // teils erst nach 20-40s durch. AP bleibt dabei für das Portal erreichbar.)
    if (strlen(cfg.apSSID) > 0 && !staBgActive) {
      Serial.println("[WiFi] Dauer-Hintergrundversuch aktiv (AP bleibt online)");
      WiFi.mode(WIFI_AP_STA);
      WiFi.begin(cfg.apSSID, cfg.apPass);
      staBgActive = true;
      lastStaLog = millis();
    }
    if (staBgActive) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] STA verbunden! Reboot in STA-Modus...");
        delay(300);
        ESP.restart();
      }
      if (millis() - lastStaLog > 30000) {   // alle 30s Status loggen
        lastStaLog = millis();
        Serial.print("[WiFi] Hintergrund: Status=");
        Serial.println(WiFi.status());
      }
    }
  }

  // ---- Serial-Kommandos ----
  // wifi:SSID,PASSWORT  → WLAN speichern + Reboot (Konfig ohne Browser möglich)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("wifi:")) {
      int comma = cmd.indexOf(',', 5);
      if (comma > 5) {
        String ssid = cmd.substring(5, comma);
        String pass = cmd.substring(comma + 1);
        strlcpy(cfg.apSSID, ssid.c_str(), sizeof(cfg.apSSID));
        strlcpy(cfg.apPass, pass.c_str(), sizeof(cfg.apPass));
        saveConfig();
        Serial.print("WLAN gespeichert in config.json: SSID='");
        Serial.print(ssid);
        Serial.println("' → Reboot...");
        delay(1500);
        ESP.restart();
      } else {
        Serial.println("Format: wifi:SSID,PASSWORT");
      }
    } else if (cmd == "creds") {
      Serial.print("Gespeicherte WLAN-SSID: '");
      Serial.print(cfg.apSSID);
      Serial.println(strlen(cfg.apSSID) ? "'" : "' (keine gesetzt)");
    } else if (cmd == "fs") {
      Serial.print("[FS] LittleFS mounted: ");
      Serial.println(LittleFS.begin() ? "ja" : "NEIN");
      Serial.print("[FS] /config.json: ");
      Serial.println(LittleFS.exists("/config.json") ? "vorhanden" : "FEHLT");
      Serial.print("[FS] /index.html: ");
      Serial.println(LittleFS.exists("/index.html") ? "vorhanden" : "FEHLT");
      File f = LittleFS.open("/config.json", "r");
      if (f) {
        JsonDocument d;
        DeserializationError e = deserializeJson(d, f);
        f.close();
        Serial.print("[FS] config.json parse: ");
        if (e == DeserializationError::Ok) Serial.println("ok");
        else Serial.println(e.f_str());
        if (e == DeserializationError::Ok) {
          Serial.print("[FS] wifiSSID in config: '");
          Serial.print(d["wifiSSID"] | "<leer>");
          Serial.println("'");
        }
      } else {
        Serial.println("[FS] config.json nicht lesbar");
      }
    } else if (cmd == "ip") {
      Serial.print("IP: ");
      Serial.println(WiFi.localIP().toString());
    }
  }

  // ---- Herzschlag (2s) ----
  if (now - heartbeatMs > 2000) {
    heartbeatMs = now;
    heartbeatState = !heartbeatState;
    digitalWrite(PIN_LED, heartbeatState ? HIGH : LOW);

    // Status abfragen
    soyoRequestStatus();

    cfg.wifiRSSI = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    strlcpy(cfg.wifiSSID, WiFi.SSID().c_str(), sizeof(cfg.wifiSSID));

    cfg.dcVolts = soyo.batteryV;
    cfg.dcAmps  = soyo.batteryA;
    cfg.dcWatts = soyo.outputPower;

    // MQTT-Publish (alle 15s)
    if (strlen(cfg.mqttBroker) > 0 && now - lastMqttMs > MQTT_INTERVAL_MS) {
      lastMqttMs = now;
      publishMQTT();
    }
  }

  // ---- NotAus-Sofortversand (unabhängig vom 3s-Gate) ----
  if (pendingNotAusSend) {
    pendingNotAusSend = false;
    soyoSetPower(0);
    cfg.perSoyoPower = 0;   // Anzeige ehrlich: es fliessen 0W
    lastSentPower = 0;
  }

  // ---- Controller (3s-Intervall) ----
  if (now - lastSendMs >= (unsigned long)cfg.waitSekunden * 1000) {
    processController();
  }
}

// ============================================================
// WEBSEITE — als HTML-Datei auf LittleFS
// ============================================================

// Wird über data/index.html in den Flash geladen
// Siehe separate Datei data/index.html