#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "DHT.h"
#include <time.h>

#define WIFI_SSID        "YOUR_WIFI_NAME"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"
#define FIREBASE_HOST    "YOUR_PROJECT_ID.firebaseio.com"
#define FIREBASE_API_KEY "YOUR_WEB_API_KEY"
#define USER_EMAIL       ""
#define USER_PASSWORD    ""
#define NTP_SERVER       "pool.ntp.org"
#define UTC_OFFSET_SEC   0

const int FLAME_PIN   = 18;
const int GAS_PIN     = 34;
const int BUZZER_PIN  = 25;
const int DHT_PIN     = 4;
const int LED_RED_PIN = 2;
const int LED_GRN_PIN = 15;

const int   GAS_LEAK_THRESHOLD = 1600;
const int   SMOKE_THRESHOLD    = 1000;
const float TEMP_ALARM_C       = 60.0;

const unsigned long LOOP_INTERVAL_MS = 2000;
const unsigned long FIREBASE_MIN_MS  = 3000;
const unsigned long WIFI_RETRY_MS    = 5000;
const unsigned long MQ2_WARMUP_MS    = 30000;

#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

bool          firebaseReady    = false;
unsigned long lastLoopTime     = 0;
unsigned long lastFirebaseTime = 0;
unsigned long lastWifiRetry    = 0;

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01T00:00:00Z";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buf);
}

void buzzerFire() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 2000, 150);
    delay(200);
  }
}

void buzzerGas() {
  tone(BUZZER_PIN, 1200, 800);
}

void buzzerOff() {
  noTone(BUZZER_PIN);
}

void setLEDs(bool alarm) {
  digitalWrite(LED_RED_PIN, alarm ? HIGH : LOW);
  digitalWrite(LED_GRN_PIN, alarm ? LOW  : HIGH);
}

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

void initFirebase() {
  config.host                  = FIREBASE_HOST;
  config.api_key               = FIREBASE_API_KEY;
  config.token_status_callback = tokenStatusCallback;
  if (strlen(USER_EMAIL) > 0) {
    auth.user.email    = USER_EMAIL;
    auth.user.password = USER_PASSWORD;
  }
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
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

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(FLAME_PIN,   INPUT_PULLUP);
  pinMode(GAS_PIN,     INPUT);
  pinMode(BUZZER_PIN,  OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GRN_PIN, OUTPUT);

  dht.begin();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    configTime(UTC_OFFSET_SEC, 0, NTP_SERVER);
    Serial.println("[NTP] Syncing time...");
    delay(2000);
  }

  initFirebase();

  Serial.println("[MQ-2] Warming up sensor — please wait 30 seconds...");
  unsigned long warmStart = millis();
  while (millis() - warmStart < MQ2_WARMUP_MS) {
    digitalWrite(LED_GRN_PIN, HIGH); delay(400);
    digitalWrite(LED_GRN_PIN, LOW);  delay(400);
    Serial.print(".");
  }
  Serial.println("\n[MQ-2] Warm-up complete.");

  Serial.println("========================================");
  Serial.println("  Detection System ACTIVE");
  Serial.println("========================================");
}

void loop() {
  unsigned long now = millis();
  if (now - lastLoopTime < LOOP_INTERVAL_MS) return;
  lastLoopTime = now;

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiRetry > WIFI_RETRY_MS) {
      lastWifiRetry = now;
      Serial.println("[WiFi] Disconnected — reconnecting...");
      connectWiFi();
      if (WiFi.status() == WL_CONNECTED && !firebaseReady) initFirebase();
    }
    Serial.println("[WARN] Offline — skipping Firebase write.");
    return;
  }

  int   flameRaw = digitalRead(FLAME_PIN);
  int   gasRaw   = analogRead(GAS_PIN);
  float temp     = dht.readTemperature();
  float hum      = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("[DHT] Read failed — retrying next cycle.");
    temp = -999.0;
    hum  = -999.0;
  }

  bool fireDetected = (flameRaw == LOW);
  bool gasLeak      = (gasRaw >= GAS_LEAK_THRESHOLD);
  bool smoke        = (gasRaw >= SMOKE_THRESHOLD && gasRaw < GAS_LEAK_THRESHOLD);
  bool tempAlarm    = (temp > TEMP_ALARM_C && temp != -999.0);

  if (tempAlarm && smoke) fireDetected = true;

  String status    = "SAFE";
  int    alertLevel = 0;

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

  String ts = getTimestamp();
  Serial.println("========================================");
  Serial.println("[" + ts + "]");
  Serial.println("  Gas ADC   : " + String(gasRaw) +
                 (gasLeak ? "  WARNING GAS LEAK" : smoke ? "  WARNING SMOKE" : "  OK"));
  Serial.println("  Flame pin : " + String(flameRaw) +
                 (fireDetected ? "  FIRE DETECTED" : "  OK"));
  Serial.println("  Temp      : " + (temp == -999.0 ? "ERR" : String(temp, 1) + " C") +
                 (tempAlarm ? "  WARNING HIGH" : ""));
  Serial.println("  Humidity  : " + (hum == -999.0 ? "ERR" : String(hum, 1) + " %"));
  Serial.println("  STATUS    : " + status);
  Serial.println("========================================");

  if (firebaseReady && (now - lastFirebaseTime >= FIREBASE_MIN_MS)) {
    lastFirebaseTime = now;

    FirebaseJson sensorJson;
    sensorJson.set("gasRaw",     gasRaw);
    sensorJson.set("fire",       fireDetected);
    sensorJson.set("smoke",      smoke);
    sensorJson.set("gasLeak",    gasLeak);
    sensorJson.set("status",     status.c_str());
    sensorJson.set("alertLevel", alertLevel);
    sensorJson.set("temp",       (temp == -999.0) ? 0 : temp);
    sensorJson.set("humidity",   (hum  == -999.0) ? 0 : hum);
    sensorJson.set("timestamp",  ts.c_str());

    if (!Firebase.RTDB.setJSON(&fbdo, "/sensors/current", &sensorJson)) {
      Serial.println("[Firebase] Write failed: " + fbdo.errorReason());
    }

    if (alertLevel > 0) {
      FirebaseJson logJson;
      logJson.set("status",     status.c_str());
      logJson.set("alertLevel", alertLevel);
      logJson.set("gasRaw",     gasRaw);
      logJson.set("temp",       (temp == -999.0) ? 0 : temp);
      logJson.set("timestamp",  ts.c_str());

      String logPath = "/logs/" + ts;
      logPath.replace(":", "-");

      if (!Firebase.RTDB.setJSON(&fbdo, logPath.c_str(), &logJson)) {
        Serial.println("[Firebase] Log write failed: " + fbdo.errorReason());
      } else {
        Serial.println("[Firebase] Alert logged at " + logPath);
      }
    }
  }
}
