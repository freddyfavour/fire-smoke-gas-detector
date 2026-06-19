// ======================================================
// 🔥 FIRE + 💨 SMOKE + 🧪 GAS DETECTION SYSTEM (ESP32)
// Compatible: ESP32 (Arduino IDE or PlatformIO)
// Libraries Required:
//   - Firebase_ESP_Client (mobizt) v4.x+
//   - DHT sensor library (Adafruit)
//   - Adafruit Unified Sensor
// ======================================================

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"  // Token generation callback
#include "addons/RTDBHelper.h"   // RTDB helper
#include "DHT.h"
#include <time.h>               // NTP time for logs

// ======================================================
// CONFIGURATION — fill these in before uploading
// ======================================================
#define WIFI_SSID       "YOUR_WIFI_NAME"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

#define FIREBASE_HOST   "YOUR_PROJECT_ID.firebaseio.com"
#define FIREBASE_API_KEY "YOUR_WEB_API_KEY"       // Firebase project Web API Key

// Firebase Auth (choose ONE method below)
// Option A: Anonymous / no-auth project (open rules) — leave these empty
// Option B: Email/password service account
#define USER_EMAIL    ""   // e.g. "device@yourproject.com"
#define USER_PASSWORD ""   // e.g. "securepassword"

// NTP server for timestamps (UTC; adjust offset for your timezone)
#define NTP_SERVER    "pool.ntp.org"
#define UTC_OFFSET_SEC 0   // Change to e.g. 3600 for UTC+1

// ======================================================
// PIN DEFINITIONS
// ======================================================
const int FLAME_PIN    = 18;  // Digital flame sensor  (LOW = fire detected)
const int GAS_PIN      = 34;  // MQ-2 analog out       (GPIO 34 = input only, correct)
const int BUZZER_PIN   = 25;  // Active or passive buzzer
const int DHT_PIN      = 4;   // DHT11 data pin
const int LED_RED_PIN  = 2;   // Optional: onboard LED or external red LED
const int LED_GRN_PIN  = 15;  // Optional: green LED for SAFE status

// ======================================================
// SENSOR / ALERT THRESHOLDS  (calibrate after burn-in)
// ======================================================
const int GAS_LEAK_THRESHOLD = 1600;  // Raw ADC >= this → GAS_LEAK
const int SMOKE_THRESHOLD    = 1000;  // Raw ADC >= this → SMOKE
const float TEMP_ALARM_C     = 60.0; // Temp threshold to flag fire risk

// ======================================================
// TIMING
// ======================================================
const unsigned long LOOP_INTERVAL_MS   = 2000;   // Main loop cadence
const unsigned long FIREBASE_MIN_MS    = 3000;   // Min gap between Firebase writes
const unsigned long WIFI_RETRY_MS      = 5000;   // WiFi reconnect interval
const unsigned long MQ2_WARMUP_MS      = 30000;  // MQ-2 warm-up (30 s)

// ======================================================
// DHT
// ======================================================
#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

// ======================================================
// FIREBASE OBJECTS
// ======================================================
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

bool firebaseReady = false;

// ======================================================
// STATE
// ======================================================
unsigned long lastLoopTime     = 0;
unsigned long lastFirebaseTime = 0;
unsigned long lastWifiRetry    = 0;
bool          warming          = true;

// ======================================================
// HELPER: Get ISO-8601 timestamp string
// ======================================================
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01T00:00:00Z"; // fallback
  }
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

// ======================================================
// HELPER: Buzzer patterns
// ======================================================
void buzzerFire() {
  // Fast triple beep for fire
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 2000, 150);
    delay(200);
  }
}

void buzzerGas() {
  // Slow long beep for gas
  tone(BUZZER_PIN, 1200, 800);
}

void buzzerOff() {
  noTone(BUZZER_PIN);
}

// ======================================================
// HELPER: LED indicator
// ======================================================
void setLEDs(bool alarm) {
  digitalWrite(LED_RED_PIN, alarm  ? HIGH : LOW);
  digitalWrite(LED_GRN_PIN, alarm  ? LOW  : HIGH);
}

// ======================================================
// HELPER: Connect WiFi (blocking with timeout)
// ======================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Connection FAILED — will retry.");
  }
}

// ======================================================
// HELPER: Init Firebase (called once WiFi is up)
// ======================================================
void initFirebase() {
  config.host           = FIREBASE_HOST;
  config.api_key        = FIREBASE_API_KEY;
  config.token_status_callback = tokenStatusCallback;  // from TokenHelper

  // Auth setup — if credentials set, use email/password; else anonymous
  if (strlen(USER_EMAIL) > 0) {
    auth.user.email    = USER_EMAIL;
    auth.user.password = USER_PASSWORD;
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Wait for token ready (max 10 s)
  Serial.print("[Firebase] Authenticating");
  unsigned long start = millis();
  while (!Firebase.ready() && millis() - start < 10000) {
    Serial.print(".");
    delay(500);
  }

  if (Firebase.ready()) {
    Serial.println("\n[Firebase] Ready.");
    firebaseReady = true;
  } else {
    Serial.println("\n[Firebase] Auth FAILED — data will NOT be sent.");
  }
}

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Pins
  pinMode(FLAME_PIN,   INPUT_PULLUP);
  pinMode(GAS_PIN,     INPUT);
  pinMode(BUZZER_PIN,  OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GRN_PIN, OUTPUT);

  // DHT
  dht.begin();

  // WiFi
  connectWiFi();

  // NTP time sync
  if (WiFi.status() == WL_CONNECTED) {
    configTime(UTC_OFFSET_SEC, 0, NTP_SERVER);
    Serial.println("[NTP] Syncing time...");
    delay(2000);
  }

  // Firebase
  initFirebase();

  // MQ-2 warm-up
  Serial.println("[MQ-2] Warming up sensor — please wait 30 seconds...");
  unsigned long warmStart = millis();
  while (millis() - warmStart < MQ2_WARMUP_MS) {
    // Blink green LED during warmup so user knows system is alive
    digitalWrite(LED_GRN_PIN, HIGH); delay(400);
    digitalWrite(LED_GRN_PIN, LOW);  delay(400);
    Serial.print(".");
  }
  Serial.println("\n[MQ-2] Warm-up complete.");
  warming = false;

  Serial.println("========================================");
  Serial.println("  Detection System ACTIVE");
  Serial.println("========================================");
}

// ======================================================
// LOOP
// ======================================================
void loop() {
  unsigned long now = millis();

  // Throttle loop cadence
  if (now - lastLoopTime < LOOP_INTERVAL_MS) return;
  lastLoopTime = now;

  // ── WiFi watchdog ────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiRetry > WIFI_RETRY_MS) {
      lastWifiRetry = now;
      Serial.println("[WiFi] Disconnected — reconnecting...");
      connectWiFi();
      if (WiFi.status() == WL_CONNECTED && !firebaseReady) {
        initFirebase(); // re-init Firebase if we hadn't gotten it before
      }
    }
    Serial.println("[WARN] Offline — skipping Firebase write.");
    return;
  }

  // ── Read sensors ─────────────────────────────────────
  int   flameRaw = digitalRead(FLAME_PIN);
  int   gasRaw   = analogRead(GAS_PIN);   // 0–4095 (12-bit ADC on ESP32)
  float temp     = dht.readTemperature(); // Celsius
  float hum      = dht.readHumidity();

  // DHT error guard
  if (isnan(temp) || isnan(hum)) {
    Serial.println("[DHT] Read failed — retrying next cycle.");
    temp = -999.0;
    hum  = -999.0;
  }

  // ── Detection logic ───────────────────────────────────
  bool fireDetected = (flameRaw == LOW);
  bool gasLeak      = (gasRaw >= GAS_LEAK_THRESHOLD);
  bool smoke        = (gasRaw >= SMOKE_THRESHOLD && gasRaw < GAS_LEAK_THRESHOLD);
  bool tempAlarm    = (temp > TEMP_ALARM_C && temp != -999.0);

  // Treat high temp + smoke as implied fire risk
  if (tempAlarm && smoke) fireDetected = true;

  String status = "SAFE";
  int    alertLevel = 0; // 0=safe 1=smoke 2=gas 3=fire

  if (fireDetected) {
    status     = "FIRE";
    alertLevel = 3;
  } else if (gasLeak) {
    status     = "GAS_LEAK";
    alertLevel = 2;
  } else if (smoke) {
    status     = "SMOKE";
    alertLevel = 1;
  }

  // ── Buzzer & LED ──────────────────────────────────────
  if (alertLevel == 3) {
    buzzerFire();
    setLEDs(true);
  } else if (alertLevel == 2) {
    buzzerGas();
    setLEDs(true);
  } else if (alertLevel == 1) {
    tone(BUZZER_PIN, 800, 200);
    setLEDs(true);
  } else {
    buzzerOff();
    setLEDs(false);
  }

  // ── Serial log ────────────────────────────────────────
  String ts = getTimestamp();
  Serial.println("========================================");
  Serial.println("[" + ts + "]");
  Serial.println("  Gas ADC   : " + String(gasRaw) +
                 (gasLeak ? "  ⚠ GAS LEAK" : smoke ? "  ⚠ SMOKE" : "  OK"));
  Serial.println("  Flame pin : " + String(flameRaw) +
                 (fireDetected ? "  🔥 FIRE" : "  OK"));
  Serial.println("  Temp      : " + (temp == -999.0 ? "ERR" : String(temp, 1) + " °C") +
                 (tempAlarm ? "  ⚠ HIGH" : ""));
  Serial.println("  Humidity  : " + (hum == -999.0 ? "ERR" : String(hum, 1) + " %"));
  Serial.println("  STATUS    : " + status);
  Serial.println("========================================");

  // ── Firebase write (batched via FirebaseJson) ─────────
  if (firebaseReady && (now - lastFirebaseTime >= FIREBASE_MIN_MS)) {
    lastFirebaseTime = now;

    // ---- Current sensor state ----
    FirebaseJson sensorJson;
    sensorJson.set("gasRaw",       gasRaw);
    sensorJson.set("fire",         fireDetected);
    sensorJson.set("smoke",        smoke);
    sensorJson.set("gasLeak",      gasLeak);
    sensorJson.set("status",       status.c_str());
    sensorJson.set("alertLevel",   alertLevel);
    sensorJson.set("temp",         (temp == -999.0) ? 0 : temp);
    sensorJson.set("humidity",     (hum  == -999.0) ? 0 : hum);
    sensorJson.set("timestamp",    ts.c_str());

    if (!Firebase.RTDB.setJSON(&fbdo, "/sensors/current", &sensorJson)) {
      Serial.println("[Firebase] Write failed: " + fbdo.errorReason());
    }

    // ---- Event log entry (only on alert) ----
    if (alertLevel > 0) {
      FirebaseJson logJson;
      logJson.set("status",      status.c_str());
      logJson.set("alertLevel",  alertLevel);
      logJson.set("gasRaw",      gasRaw);
      logJson.set("temp",        (temp == -999.0) ? 0 : temp);
      logJson.set("timestamp",   ts.c_str());

      String logPath = "/logs/" + ts;  // Use timestamp as unique key
      // Replace colons (invalid in Firebase paths) with dashes
      logPath.replace(":", "-");

      if (!Firebase.RTDB.setJSON(&fbdo, logPath.c_str(), &logJson)) {
        Serial.println("[Firebase] Log write failed: " + fbdo.errorReason());
      } else {
        Serial.println("[Firebase] Alert logged at " + logPath);
      }
    }
  }
}

// ======================================================
// END OF FILE
// ======================================================
// WIRING GUIDE:
// ─────────────────────────────────────────────────────
//  MQ-2 Gas Sensor:
//    VCC  → ESP32 5V (Vin)
//    GND  → ESP32 GND
//    AO   → GPIO 34  (analog)
//    DO   → (not used — we use analog)
//
//  Flame Sensor (IR):
//    VCC  → 3.3V
//    GND  → GND
//    DO   → GPIO 18  (digital, active LOW)
//
//  DHT11:
//    VCC  → 3.3V
//    GND  → GND
//    DATA → GPIO 4  (add 10k pull-up to 3.3V)
//
//  Buzzer (passive):
//    +    → GPIO 25
//    -    → GND
//    (active buzzer: same pins, but tone() not needed)
//
//  LEDs (optional):
//    Red  → GPIO 2  → 220Ω resistor → GND
//    Green→ GPIO 15 → 220Ω resistor → GND
//
// FIREBASE RULES (for testing — tighten in production):
// ─────────────────────────────────────────────────────
// {
//   "rules": {
//     ".read":  "auth != null",
//     ".write": "auth != null"
//   }
// }
//
// REQUIRED LIBRARIES (install via Arduino Library Manager):
//   1. Firebase_ESP_Client  by Mobizt  (v4.x)
//   2. DHT sensor library   by Adafruit
//   3. Adafruit Unified Sensor
// ======================================================
