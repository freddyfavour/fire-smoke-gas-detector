# Fire, Smoke & Gas Detection System — ESP32

A production-ready IoT safety monitor that detects fire, smoke, and gas leaks in real time, sends all sensor data to Firebase Realtime Database, and alerts via buzzer and LEDs.

---

## Features

- Real-time detection of fire (IR flame sensor), smoke and gas leaks (MQ-2 analog)
- Temperature and humidity monitoring via DHT11
- Tiered alert system — SAFE → SMOKE → GAS_LEAK → FIRE
- Buzzer with distinct tone patterns per alert type
- Red/green LED status indicators
- Single batched Firebase write per cycle (efficient, low quota usage)
- Persistent alert event log in Firebase at `/logs/<timestamp>`
- NTP timestamps on every reading and log entry
- WiFi watchdog with automatic reconnection
- MQ-2 30-second warm-up on boot to prevent false alarms
- DHT11 NaN guard — skips cycle if sensor read fails

---

## Hardware Requirements

| Component | Purpose | Notes |
|---|---|---|
| ESP32 dev board | Microcontroller | Any 38-pin ESP32 works |
| MQ-2 gas sensor | Smoke + gas detection | Analog output only |
| KY-026 flame sensor | Fire detection | IR digital output |
| DHT11 | Temperature + humidity | Needs 10kΩ pull-up |
| Passive buzzer | Audio alert | Needs PWM, not DC |
| Red LED | Alarm indicator | 220Ω resistor required |
| Green LED | Safe indicator | 220Ω resistor required |
| Resistors | 10kΩ (×1), 220Ω (×2) | See wiring section |
| Jumper wires + breadboard | Connections | — |

---

## Wiring

### ESP32 Pin Assignments

| GPIO | Connection |
|---|---|
| GPIO 4 | DHT11 DATA |
| GPIO 18 | Flame sensor DO |
| GPIO 34 | MQ-2 AO (analog, input-only) |
| GPIO 25 | Buzzer + |
| GPIO 2 | Red LED anode (via 220Ω) |
| GPIO 15 | Green LED anode (via 220Ω) |
| 3.3V | DHT11 VCC, Flame sensor VCC |
| 5V (Vin) | MQ-2 VCC |
| GND | All sensor GNDs, LED cathodes, buzzer – |

### Per-Sensor Wiring

**DHT11**
```
VCC  → 3.3V
DATA → GPIO 4  + 10kΩ resistor to 3.3V   ← pull-up required
GND  → GND
```

**MQ-2 Gas Sensor**
```
VCC → 5V (Vin)   ← must be 5V, not 3.3V
GND → GND
AO  → GPIO 34
DO  → not connected
```

**Flame Sensor (KY-026)**
```
VCC → 3.3V
GND → GND
DO  → GPIO 18
AO  → not connected
```

**Passive Buzzer**
```
+   → GPIO 25
–   → GND
```

**LEDs**
```
Red LED:   GPIO 2  → 220Ω → LED anode (+) ... LED cathode (–) → GND
Green LED: GPIO 15 → 220Ω → LED anode (+) ... LED cathode (–) → GND
```

> **Power note:** The MQ-2 heater draws ~150mA and requires 5V. Use the ESP32's Vin pin (connected directly to USB power). Do not use 3.3V — the onboard regulator cannot supply enough current and readings will be unreliable.

---

## Required Libraries

Install all three via **Arduino IDE → Library Manager** (Sketch → Include Library → Manage Libraries):

| Library | Author | Version |
|---|---|---|
| Firebase_ESP_Client | Mobizt | 4.x or later |
| DHT sensor library | Adafruit | Latest |
| Adafruit Unified Sensor | Adafruit | Latest |

---

## Firebase Setup

### 1. Create a Firebase project

1. Go to [console.firebase.google.com](https://console.firebase.google.com)
2. Click **Add project** and follow the steps
3. Once created, go to **Project Settings → General**
4. Scroll to **Your apps** → click the `</>` (Web) icon → register an app
5. Copy the **Web API Key** shown in the config block

### 2. Enable Realtime Database

1. In the left sidebar click **Build → Realtime Database**
2. Click **Create database**
3. Choose a region, then start in **locked mode**
4. Your database URL will be: `https://YOUR_PROJECT_ID-default-rtdb.firebaseio.com`

### 3. Set database rules

For development / testing, use open rules (tighten these before deploying to production):

```json
{
  "rules": {
    ".read":  true,
    ".write": true
  }
}
```

For production with a service account:

```json
{
  "rules": {
    ".read":  "auth != null",
    ".write": "auth != null"
  }
}
```

### 4. (Optional) Create a device user

If you set auth rules to `auth != null`:

1. Go to **Build → Authentication → Get started**
2. Enable **Email/Password** provider
3. Under **Users** tab, click **Add user**
4. Use something like `device@yourproject.com` with a secure password
5. Enter these credentials in the sketch

---

## Configuration

Open `fire_smoke_gas_detector.ino` and fill in the block at the top:

```cpp
#define WIFI_SSID        "YOUR_WIFI_NAME"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

#define FIREBASE_HOST    "YOUR_PROJECT_ID-default-rtdb.firebaseio.com"
#define FIREBASE_API_KEY "YOUR_WEB_API_KEY"

// Optional — leave empty if using open rules
#define USER_EMAIL    "device@yourproject.com"
#define USER_PASSWORD "your_secure_password"

// Adjust for your timezone (seconds offset from UTC)
// UTC+0 = 0, UTC+1 = 3600, UTC+5:30 = 19800, UTC-5 = -18000
#define UTC_OFFSET_SEC 0
```

---

## Threshold Calibration

Default thresholds in the sketch are a starting point. Calibrate after your MQ-2 has had a full 24-hour burn-in period:

```cpp
const int GAS_LEAK_THRESHOLD = 1600;  // Raw ADC value ≥ this → GAS_LEAK
const int SMOKE_THRESHOLD    = 1000;  // Raw ADC value ≥ this → SMOKE
const float TEMP_ALARM_C     = 60.0;  // °C above which high-temp + smoke = FIRE
```

**How to calibrate:**
1. Upload the sketch and open Serial Monitor at 115200 baud
2. Note the baseline gas ADC reading in clean air (usually 200–400)
3. Light a match near the MQ-2 — note at what value it spikes
4. Set `SMOKE_THRESHOLD` to about 2× your clean-air baseline
5. Set `GAS_LEAK_THRESHOLD` to about 70% of the spike value you observed

---

## Firebase Data Structure

```
/sensors/current/
  ├── gasRaw       (int)     — raw ADC value from MQ-2, 0–4095
  ├── fire         (bool)    — true if flame detected
  ├── smoke        (bool)    — true if smoke level detected
  ├── gasLeak      (bool)    — true if gas leak level detected
  ├── status       (string)  — "SAFE" | "SMOKE" | "GAS_LEAK" | "FIRE"
  ├── alertLevel   (int)     — 0=safe, 1=smoke, 2=gas, 3=fire
  ├── temp         (float)   — temperature in °C (-999 if sensor error)
  ├── humidity     (float)   — relative humidity % (-999 if sensor error)
  └── timestamp    (string)  — ISO-8601 UTC e.g. "2024-06-13T10:45:22Z"

/logs/<timestamp>/
  ├── status       (string)
  ├── alertLevel   (int)
  ├── gasRaw       (int)
  ├── temp         (float)
  └── timestamp    (string)
```

Log entries are only written when `alertLevel > 0` (i.e. any alert condition is active). The `/sensors/current` node is always overwritten with the latest reading.

---

## Serial Monitor Output

Open the Serial Monitor at **115200 baud** to see live output:

```
[WiFi] Connecting to MyNetwork
......
[WiFi] Connected. IP: 192.168.1.42
[NTP] Syncing time...
[Firebase] Authenticating......
[Firebase] Ready.
[MQ-2] Warming up sensor — please wait 30 seconds...
..............................
[MQ-2] Warm-up complete.
========================================
  Detection System ACTIVE
========================================
========================================
[2024-06-13T10:45:22Z]
  Gas ADC   : 312  OK
  Flame pin : 1  OK
  Temp      : 27.4 °C
  Humidity  : 58.2 %
  STATUS    : SAFE
========================================
```

---

## Alert Behaviour

| Condition | Status | Buzzer | LEDs |
|---|---|---|---|
| All clear | SAFE | Silent | Green on |
| Gas ADC 1000–1599 | SMOKE | Short 800Hz beep | Red on |
| Gas ADC ≥ 1600 | GAS_LEAK | Long 1.2kHz pulse | Red on |
| Flame sensor LOW, or high temp + smoke | FIRE | Fast triple 2kHz beep | Red on |

The system upgrades SMOKE to FIRE automatically if temperature also exceeds `TEMP_ALARM_C` (default 60°C). This catches fires that are out of the flame sensor's line of sight.

---

## Troubleshooting

**MQ-2 always reads high / triggers false alarms**
- Make sure it is powered by 5V, not 3.3V
- Wait the full 30-second warm-up (fresh sensors may need a 24-hour burn-in)
- Raise `SMOKE_THRESHOLD` if baseline air readings are high

**DHT11 shows ERR / -999**
- Confirm the 10kΩ pull-up resistor is connected between DATA and 3.3V
- Try a longer wire or shorter breadboard connection

**Buzzer makes no sound**
- Verify it is a passive buzzer (active buzzers also work, but tone varies)
- Check GPIO 25 is not tied to GND elsewhere on the board

**Firebase writes fail**
- Check Serial Monitor for `[Firebase] Write failed:` and the error reason
- Confirm the API key, host URL, and credentials are correct
- Verify database rules allow writes (see Firebase Setup above)
- If the error is `token is not valid`, re-flash the sketch — the token may have expired mid-session

**No WiFi connection**
- Double check SSID and password (case-sensitive)
- Confirm the network is 2.4GHz — ESP32 does not support 5GHz

---

## Project Structure

```
fire_smoke_gas_detector/
├── fire_smoke_gas_detector.ino   — main sketch
└── README.md                     — this file
```

---

## License

MIT — free to use, modify, and distribute.
