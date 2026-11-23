/**
 * ============================================================================
 *  Smart EV Monitoring & Anti-Theft System
 *  ---------------------------------------------------------------------------
 *  Target   : ESP32 DevKit V1 (ESP32-WROOM-32)
 *  Toolchain: PlatformIO + Arduino framework
 *  Author   : Abinivesh M
 * ============================================================================
 *
 *  Responsibilities
 *  ----------------
 *   1. Sample pack voltage, current, temperature and vibration.
 *   2. Derive power and state of charge.
 *   3. Run the anti-theft state machine (vibration + geofence).
 *   4. Persist telemetry to microSD and buffer it when offline.
 *   5. Publish to Blynk and accept arm/disarm commands.
 *   6. Serve OTA firmware updates and a serial diagnostics CLI.
 *
 *  Design rules
 *  ------------
 *   - The sampling loop must never block on network or storage.
 *   - Every sensor read is validated before it is used or published.
 *   - Every failure mode degrades gracefully and is reported.
 * ============================================================================
 */

#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>
#include <TinyGPSPlus.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char *FW_VERSION = "1.0.0";

static const uint8_t  ADC_SAMPLES          = 15;    // median window
static const uint32_t SAMPLE_INTERVAL_MS   = 2000;  // telemetry cadence
static const uint32_t LOG_INTERVAL_MS      = 5000;  // SD cadence
static const uint32_t ALERT_COOLDOWN_MS    = 30000; // per-event rate limit
static const uint32_t WIFI_RETRY_MS        = 15000;
static const uint32_t CLOUD_RETRY_MS       = 5000;
static const uint32_t GPS_BAUD             = 9600;
static const size_t   OFFLINE_QUEUE_LEN    = 120;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
enum GuardState : uint8_t { GUARD_DISARMED = 0, GUARD_ARMED = 1, GUARD_ALERT = 2 };

struct Telemetry {
  float    packVoltage;   // V
  float    current;       // A (positive = discharge)
  float    power;         // W
  float    soc;           // %
  float    temperature;   // degC
  double   latitude;
  double   longitude;
  bool     gpsFix;
  uint8_t  satellites;
  uint16_t vibration;     // pulses since boot
  uint32_t uptimeMs;
};

struct GeoPoint {
  double lat;
  double lon;
  bool   valid;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static DHT            g_dht(PIN_DHT, DHT22);
static TinyGPSPlus    g_gps;
static HardwareSerial g_gpsPort(2);
static Preferences    g_prefs;
static BlynkTimer     g_timer;

static Telemetry  g_tel;
static GeoPoint   g_anchor      = {0.0, 0.0, false};
static GuardState g_guard       = GUARD_DISARMED;
static float      g_currentZeroMv = 1650.0f;   // ACS712 midpoint at 3.3 V
static uint32_t   g_alertCount  = 0;
static uint32_t   g_lastAlertMs = 0;
static uint32_t   g_sensorFaults = 0;
static bool       g_sdReady     = false;
static bool       g_cloudUp     = false;
static bool       g_sleepRequested = false;
static uint32_t   g_lastWifiMs   = 0;
static uint32_t   g_lastCloudMs  = 0;

// Vibration pulses counted in an ISR -> must be volatile
static volatile uint32_t g_vibPulses = 0;
static void IRAM_ATTR onVibration() { g_vibPulses++; }

// Offline ring buffer (head = oldest)
static Telemetry g_queue[OFFLINE_QUEUE_LEN];
static size_t    g_queueCount = 0;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static float medianOf(float *values, uint8_t n) {
  // Insertion sort: n is tiny (<= 15), so this is cheaper than qsort.
  for (uint8_t i = 1; i < n; i++) {
    float key = values[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
  return (n & 1) ? values[n / 2]
                 : 0.5f * (values[n / 2 - 1] + values[n / 2]);
}

static float medianMillivolts(uint8_t pin) {
  float samples[ADC_SAMPLES];
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) {
    samples[i] = (float)analogReadMilliVolts(pin);
    delayMicroseconds(150);
  }
  return medianOf(samples, ADC_SAMPLES);
}

static bool plausible(float v, float lo, float hi) {
  return !isnan(v) && v >= lo && v <= hi;
}

static double haversineMeters(const GeoPoint &a, const GeoPoint &b) {
  const double R = 6371000.0;
  const double dLat = radians(b.lat - a.lat);
  const double dLon = radians(b.lon - a.lon);
  const double s = sin(dLat / 2) * sin(dLat / 2) +
                   cos(radians(a.lat)) * cos(radians(b.lat)) *
                   sin(dLon / 2) * sin(dLon / 2);
  return 2.0 * R * atan2(sqrt(s), sqrt(1.0 - s));
}

// ---------------------------------------------------------------------------
// Sensor layer
// ---------------------------------------------------------------------------
namespace sensors {

  void calibrateCurrentZero() {
    // Only call with the load disconnected / vehicle idle.
    const uint16_t n = 200;
    uint32_t sum = 0;
    for (uint16_t i = 0; i < n; i++) {
      sum += analogReadMilliVolts(PIN_CURRENT);
      delay(2);
    }
    float avg = (float)sum / n;
    if (plausible(avg, 1200.0f, 2100.0f)) {
      g_currentZeroMv = avg;
      Serial.printf("[ADC ] zero offset calibrated: %.0f mV\n", avg);
    } else {
      Serial.printf("[ADC ] zero calibration rejected (%.0f mV)\n", avg);
    }
  }

  float packVoltage() {
    // Divider brings the pack into the 0-3.3 V ADC window.
    float mv = medianMillivolts(PIN_VOLT_SENSE);
    return (mv / 1000.0f) * DIVIDER_RATIO;
  }

  float current() {
    float mv = medianMillivolts(PIN_CURRENT);
    return (mv - g_currentZeroMv) / ACS712_MV_PER_AMP;
  }

  float temperature() {
    float t = g_dht.readTemperature();
    if (!plausible(t, -20.0f, 125.0f)) {
      g_sensorFaults++;
      Serial.println("[SENS] DHT read failed");
      return NAN;
    }
    return t;
  }

  uint16_t vibrationPulses() { return (uint16_t)g_vibPulses; }
}

// ---------------------------------------------------------------------------
// State of charge (13S Li-ion, open-circuit voltage curve)
// ---------------------------------------------------------------------------
namespace soc {

  struct Point { float volts; float percent; };

  static const Point CURVE[] = {
    {54.60f, 100.0f}, {53.30f, 90.0f}, {52.00f, 80.0f}, {50.70f, 70.0f},
    {49.40f, 55.0f},  {48.10f, 40.0f}, {46.80f, 25.0f}, {45.50f, 15.0f},
    {44.20f, 8.0f},   {42.90f, 3.0f},  {39.00f, 0.0f}
  };
  static const uint8_t CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

  float estimate(float packVoltage, float currentAmps) {
    // Voltage sags under load; compensate so the gauge does not dip on accel.
    float compensated = packVoltage + (currentAmps * LOAD_COMPENSATION_OHM);

    if (compensated >= CURVE[0].volts) return 100.0f;
    if (compensated <= CURVE[CURVE_LEN - 1].volts) return 0.0f;

    for (uint8_t i = 0; i < CURVE_LEN - 1; i++) {
      const Point &hi = CURVE[i];
      const Point &lo = CURVE[i + 1];
      if (compensated <= hi.volts && compensated >= lo.volts) {
        float span = hi.volts - lo.volts;
        float ratio = (compensated - lo.volts) / span;
        return lo.percent + ratio * (hi.percent - lo.percent);
      }
    }
    return 0.0f;
  }
}

// ---------------------------------------------------------------------------
// Storage layer
// ---------------------------------------------------------------------------
namespace storage {

  bool begin() {
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) {
      Serial.println("[SD  ] card not present - logging disabled");
      return false;
    }
    if (SD.cardType() == CARD_NONE) {
      Serial.println("[SD  ] unsupported card");
      return false;
    }
    if (!SD.exists("/logs")) SD.mkdir("/logs");
    Serial.printf("[SD  ] ready, %llu MB\n",
                  (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
    return true;
  }

  static void writeHeader(File &f) {
    f.println("uptime_ms,voltage_v,current_a,power_w,soc_pct,temp_c,"
              "lat,lon,fix,sats,vibration,guard");
  }

  void append(const Telemetry &t) {
    if (!g_sdReady) return;

    // One file per UTC day when the GPS knows the date, else per boot.
    char path[40];
    if (g_gps.date.isValid() && g_gps.time.isValid()) {
      snprintf(path, sizeof(path), "/logs/ev_%04d%02d%02d.csv",
               g_gps.date.year(), g_gps.date.month(), g_gps.date.day());
    } else {
      snprintf(path, sizeof(path), "/logs/ev_boot_%lu.csv",
               (unsigned long)(millis() / 1000));
    }

    bool isNew = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) {
      g_sdReady = false;
      Serial.println("[SD  ] open failed - logging disabled");
      return;
    }
    if (isNew) writeHeader(f);

    f.printf("%lu,%.2f,%.3f,%.1f,%.1f,%.2f,%.6f,%.6f,%d,%u,%u,%u\n",
             (unsigned long)t.uptimeMs, t.packVoltage, t.current, t.power,
             t.soc, t.temperature, t.latitude, t.longitude,
             t.gpsFix ? 1 : 0, t.satellites, t.vibration, (unsigned)g_guard);
    f.close();
  }
}

// ---------------------------------------------------------------------------
// Theft guard
// ---------------------------------------------------------------------------
namespace guard {

  static void raise(const char *reason) {
    uint32_t now = millis();
    if (now - g_lastAlertMs < ALERT_COOLDOWN_MS) return;

    g_lastAlertMs = now;
    g_alertCount++;
    g_guard = GUARD_ALERT;

    digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? LOW : HIGH); // immobilise
    digitalWrite(PIN_BUZZER, HIGH);

    Serial.printf("[GUARD] ALERT: %s (total %lu)\n", reason,
                  (unsigned long)g_alertCount);

    if (Blynk.connected()) {
      Blynk.logEvent("theft", reason);
      Blynk.virtualWrite(V6, (int)g_guard);
      Blynk.virtualWrite(V7, (int)g_alertCount);
    }
    storage::append(g_tel);
  }

  void arm() {
    g_guard = GUARD_ARMED;
    digitalWrite(PIN_BUZZER, LOW);
    digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW); // release
    if (g_tel.gpsFix) {
      g_anchor = {g_tel.latitude, g_tel.longitude, true};
    } else {
      g_anchor.valid = false;
    }
    g_lastAlertMs = millis() - ALERT_COOLDOWN_MS; // allow an immediate alert
    Serial.printf("[GUARD] armed @ %.6f,%.6f radius=%um\n",
                  g_anchor.lat, g_anchor.lon, (unsigned)GEOFENCE_RADIUS_M);
  }

  void disarm() {
    g_guard = GUARD_DISARMED;
    digitalWrite(PIN_BUZZER, LOW);
    digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
    Serial.println("[GUARD] disarmed");
  }

  void update(const Telemetry &t) {
    if (g_guard == GUARD_DISARMED) return;

    // 1. Vibration: a burst of pulses is a tamper signal, not a single edge.
    static uint16_t lastPulses = 0;
    uint16_t delta = t.vibration - lastPulses;
    lastPulses = t.vibration;
    if (delta >= VIBRATION_THRESHOLD) {
      raise("vibration burst");
      return;
    }

    // 2. Geofence: only meaningful with a fresh fix.
    if (g_anchor.valid && t.gpsFix) {
      GeoPoint now = {t.latitude, t.longitude, true};
      double d = haversineMeters(g_anchor, now);
      if (d > GEOFENCE_RADIUS_M) {
        raise("geofence breach");
        return;
      }
    }

    // 3. Thermal runaway pre-warning.
    if (plausible(t.temperature, -50.0f, 200.0f) &&
        t.temperature > TEMP_ALERT_C && Blynk.connected()) {
      static uint32_t lastThermal = 0;
      if (millis() - lastThermal > 60000) {
        lastThermal = millis();
        Blynk.logEvent("thermal", "Pack temperature above limit");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Cloud layer
// ---------------------------------------------------------------------------
namespace cloud {

  static void push(const Telemetry &t) {
    if (!Blynk.connected()) return;
    Blynk.virtualWrite(V0, t.packVoltage);
    Blynk.virtualWrite(V1, t.current);
    Blynk.virtualWrite(V2, t.power);
    Blynk.virtualWrite(V3, t.soc);
    Blynk.virtualWrite(V4, t.temperature);
    Blynk.virtualWrite(V5, t.gpsFix
                              ? String(t.latitude, 6) + "," + String(t.longitude, 6)
                              : String("no fix"));
    Blynk.virtualWrite(V6, (int)g_guard);
    Blynk.virtualWrite(V7, (int)g_alertCount);
    Blynk.virtualWrite(V8, String(g_sdReady ? "SD ok" : "SD off") + " / " +
                              String(g_queueCount) + " queued");
    Blynk.virtualWrite(V9, WiFi.RSSI());
  }

  static void queue(const Telemetry &t) {
    if (g_queueCount < OFFLINE_QUEUE_LEN) {
      g_queue[g_queueCount++] = t;
    } else {
      // Drop the oldest sample so the newest telemetry always survives.
      memmove(&g_queue[0], &g_queue[1], sizeof(Telemetry) * (OFFLINE_QUEUE_LEN - 1));
      g_queue[OFFLINE_QUEUE_LEN - 1] = t;
    }
  }

  static void flushQueue() {
    if (!Blynk.connected()) return;
    // Bounded flush so a long outage cannot starve the sampling loop.
    for (size_t i = 0; i < g_queueCount && i < 20; i++) push(g_queue[i]);
    if (g_queueCount > 20) {
      memmove(&g_queue[0], &g_queue[20], sizeof(Telemetry) * (g_queueCount - 20));
      g_queueCount -= 20;
    } else {
      g_queueCount = 0;
    }
  }

  void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Blynk.config(BLYNK_AUTH_TOKEN, BLYNK_SERVER, 80);
    Serial.printf("[WIFI] connecting to %s\n", WIFI_SSID);
  }

  void loop() {
    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      g_cloudUp = false;
      if (now - g_lastWifiMs > WIFI_RETRY_MS) {
        g_lastWifiMs = now;
        Serial.println("[WIFI] retry");
        WiFi.reconnect();
      }
      return;
    }

    if (!Blynk.connected()) {
      g_cloudUp = false;
      if (now - g_lastCloudMs > CLOUD_RETRY_MS) {
        g_lastCloudMs = now;
        if (Blynk.connect(2000)) {
          Serial.printf("[CLOUD] connected, rssi=%d dBm\n", WiFi.RSSI());
        }
      }
      return;
    }

    if (!g_cloudUp) {
      g_cloudUp = true;
      flushQueue();
    }
    Blynk.run();
  }
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
namespace ota {

  void begin() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { Serial.println("[OTA ] update starting"); });
    ArduinoOTA.onEnd([]() { Serial.println("\n[OTA ] complete"); });
    ArduinoOTA.onError([](ota_error_t e) {
      Serial.printf("[OTA ] error %u\n", (unsigned)e);
    });
    ArduinoOTA.begin();
    Serial.printf("[OTA ] ready as %s.local\n", OTA_HOSTNAME);
  }
}

// ---------------------------------------------------------------------------
// Sampling task (scheduled by BlynkTimer)
// ---------------------------------------------------------------------------
static void sampleTelemetry() {
  float v = sensors::packVoltage();
  if (!plausible(v, PACK_MIN_V, PACK_MAX_V)) {
    g_sensorFaults++;
    Serial.printf("[SENS] voltage out of range: %.2f V\n", v);
    v = g_tel.packVoltage; // hold last good value
  }

  float a = sensors::current();
  if (!plausible(a, -CURRENT_MAX_A, CURRENT_MAX_A)) {
    g_sensorFaults++;
    a = 0.0f;
  }

  float t = sensors::temperature();

  g_tel.packVoltage = v;
  g_tel.current     = a;
  g_tel.power       = v * a;
  g_tel.soc         = soc::estimate(v, a);
  g_tel.temperature = isnan(t) ? g_tel.temperature : t;
  g_tel.gpsFix      = g_gps.location.isValid() && g_gps.location.age() < 5000;
  g_tel.latitude    = g_tel.gpsFix ? g_gps.location.lat() : 0.0;
  g_tel.longitude   = g_tel.gpsFix ? g_gps.location.lng() : 0.0;
  g_tel.satellites  = g_gps.satellites.isValid() ? (uint8_t)g_gps.satellites.value() : 0;
  g_tel.vibration   = sensors::vibrationPulses();
  g_tel.uptimeMs    = millis();

  guard::update(g_tel);
  cloud::push(g_tel);
  if (!Blynk.connected()) cloud::queue(g_tel);
}

static void logTelemetry() {
  storage::append(g_tel);
}

// ---------------------------------------------------------------------------
// Serial CLI
// ---------------------------------------------------------------------------
static void printStatus() {
  Serial.println(F("----------- STATUS -----------"));
  Serial.printf("firmware   : %s\n", FW_VERSION);
  Serial.printf("uptime     : %lu s\n", (unsigned long)(millis() / 1000));
  Serial.printf("pack       : %.2f V  %.3f A  %.1f W\n",
                g_tel.packVoltage, g_tel.current, g_tel.power);
  Serial.printf("soc / temp : %.1f %%  %.2f C\n", g_tel.soc, g_tel.temperature);
  Serial.printf("gps        : %s  %.6f, %.6f  sats=%u\n",
                g_tel.gpsFix ? "fix" : "no fix",
                g_tel.latitude, g_tel.longitude, g_tel.satellites);
  Serial.printf("guard      : %s  alerts=%lu\n",
                g_guard == GUARD_ALERT ? "ALERT"
                : g_guard == GUARD_ARMED ? "ARMED" : "DISARMED",
                (unsigned long)g_alertCount);
  Serial.printf("storage    : %s  queued=%u\n",
                g_sdReady ? "ready" : "unavailable", (unsigned)g_queueCount);
  Serial.printf("link       : %s  rssi=%d\n",
                Blynk.connected() ? "cloud" : "offline", WiFi.RSSI());
  Serial.printf("faults     : %lu\n", (unsigned long)g_sensorFaults);
  Serial.println(F("------------------------------"));
}

static void handleCommand(const String &raw) {
  String cmd = raw;
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "help") {
    Serial.println(F("commands: help status arm disarm cal sim sleep reboot"));
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "arm") {
    guard::arm();
  } else if (cmd == "disarm") {
    guard::disarm();
  } else if (cmd == "cal") {
    sensors::calibrateCurrentZero();
  } else if (cmd == "sim") {
    Serial.println("[SIM ] injecting theft event");
    g_lastAlertMs = millis() - ALERT_COOLDOWN_MS;
    guard::raise("simulated tamper");
  } else if (cmd == "sleep") {
    g_sleepRequested = true;
  } else if (cmd == "reboot") {
    ESP.restart();
  } else if (cmd.length()) {
    Serial.printf("unknown command: %s\n", cmd.c_str());
  }
}

static void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) handleCommand(line);
      line = "";
    } else if (line.length() < 64) {
      line += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Power management
// ---------------------------------------------------------------------------
static void enterParkedSleep() {
  Serial.println("[PWR ] entering parked deep sleep");
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  digitalWrite(PIN_BUZZER, LOW);
  Blynk.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_VIBRATION, 0);
  esp_sleep_enable_timer_wakeup((uint64_t)PARKED_WAKE_S * 1000000ULL);
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
// Blynk command handlers
// ---------------------------------------------------------------------------
BLYNK_WRITE(V10) {
  int v = param.asInt();
  if (v == 1) guard::arm();
  else        guard::disarm();
  Blynk.virtualWrite(V6, (int)g_guard);
}

BLYNK_WRITE(V11) {
  if (param.asInt() == 1) g_sleepRequested = true;
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V10);
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("[BOOT] Smart EV Monitor v%s\n", FW_VERSION);

  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_VIBRATION, INPUT_PULLUP);
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW); // released
  digitalWrite(PIN_BUZZER, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  g_dht.begin();
  g_gpsPort.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  attachInterrupt(digitalPinToInterrupt(PIN_VIBRATION), onVibration, FALLING);

  g_sdReady = storage::begin();
  sensors::calibrateCurrentZero();

  // Persist lifetime alert counter across reboots.
  g_prefs.begin("evmon", false);
  g_alertCount = g_prefs.getULong("alerts", 0);

  g_timer.setInterval(SAMPLE_INTERVAL_MS, sampleTelemetry);
  g_timer.setInterval(LOG_INTERVAL_MS, logTelemetry);

  cloud::begin();
  ota::begin();

  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  Serial.println(F("[BOOT] ready - type 'help'"));
}

void loop() {
  pollSerial();

  // Feed the GPS parser as often as possible; it is non-blocking.
  while (g_gpsPort.available()) g_gps.encode(g_gpsPort.read());

  cloud::loop();
  ArduinoOTA.handle();
  g_timer.run();

  if (g_sleepRequested) {
    g_prefs.putULong("alerts", g_alertCount);
    enterParkedSleep();
  }

  esp_task_wdt_reset();
}
