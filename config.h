/**
 * ============================================================================
 *  config.h - compile-time configuration for the Smart EV Monitor
 * ============================================================================
 *
 *  Copy this file to config.h (it is tracked here so the project builds out of
 *  the box) and replace the credentials with your own before flashing.
 *
 *  SECURITY: never commit real Wi-Fi passwords, Blynk tokens or OTA passwords
 *  to a public repository. For a production deployment keep config.h in
 *  .gitignore and commit a sanitised config.example.h instead.
 * ============================================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// Blynk 2.0 (must be defined before <BlynkSimpleEsp32.h> is included)
// ---------------------------------------------------------------------------
#define BLYNK_TEMPLATE_ID   "TMPL0000000"
#define BLYNK_TEMPLATE_NAME "Smart EV Monitor"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_AUTH_TOKEN"
#define BLYNK_SERVER        "blynk.cloud"

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASS     "YOUR_WIFI_PASSWORD"
#define OTA_HOSTNAME  "smart-ev-monitor"
#define OTA_PASSWORD  "change-this-ota-password"

// ---------------------------------------------------------------------------
// Pin map (ESP32 DevKit V1)
// ---------------------------------------------------------------------------
#define PIN_VOLT_SENSE  34   // ADC1_CH6, input only
#define PIN_CURRENT     35   // ADC1_CH7, input only
#define PIN_DHT          4
#define PIN_VIBRATION   27   // RTC GPIO, used for ext0 deep-sleep wake
#define PIN_GPS_RX      16
#define PIN_GPS_TX      17
#define PIN_SD_CS        5
#define PIN_SD_SCK      18
#define PIN_SD_MISO     19
#define PIN_SD_MOSI     23
#define PIN_RELAY       26
#define PIN_BUZZER      25
#define PIN_LED          2

// Relay modules are usually active-low. Set to false for active-high boards.
#define RELAY_ACTIVE_LOW  true

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------
// Divider: 100k over 10k gives 11:1. Measure with a multimeter and adjust.
#define DIVIDER_RATIO        11.0f

// ACS712-30A: 66 mV/A. The 20A variant is 100 mV/A.
#define ACS712_MV_PER_AMP    66.0f

// Internal resistance estimate used to compensate voltage sag under load.
#define LOAD_COMPENSATION_OHM 0.035f

// ---------------------------------------------------------------------------
// Operating limits
// ---------------------------------------------------------------------------
#define PACK_MIN_V        30.0f   // below this the pack is disconnected/faulty
#define PACK_MAX_V        60.0f   // 13S Li-ion fully charged is 54.6 V
#define CURRENT_MAX_A     40.0f   // sensor saturates around +/-30 A
#define TEMP_ALERT_C      55.0f   // pack/controller thermal warning

// ---------------------------------------------------------------------------
// Security
// ---------------------------------------------------------------------------
#define VIBRATION_THRESHOLD   4      // pulses inside one sample window
#define GEOFENCE_RADIUS_M     150.0  // metres from the position at arm time

// ---------------------------------------------------------------------------
// Power management
// ---------------------------------------------------------------------------
#define PARKED_WAKE_S         300ULL // periodic wake while parked (seconds)

#endif // CONFIG_H
