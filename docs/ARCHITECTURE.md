# Architecture

This document describes the firmware's module boundaries, the data flow between
them, and how each module behaves when its hardware is unavailable.

---

## 1. Module map

| Module | File | Depends on | Owns |
| :--- | :--- | :--- | :--- |
| `sensors` | `src/main.cpp` | ADC, DHT22, GPIO ISR | Filtering, calibration, validation |
| `soc` | `src/main.cpp` | — | Voltage → state-of-charge curve |
| `guard` | `src/main.cpp` | sensors, relay, buzzer | Arming, alerts, immobiliser |
| `storage` | `src/main.cpp` | SD, SPI, GPS clock | CSV persistence |
| `cloud` | `src/main.cpp` | Wi-Fi, Blynk | Publish, queue, reconnect |
| `ota` | `src/main.cpp` | Wi-Fi, ArduinoOTA | Firmware updates |
| `cli` | `src/main.cpp` | Serial | Diagnostics and bench control |

The modules are separated by namespaces rather than classes because the
firmware is a single translation unit and the target has no heap budget for
polymorphism. Contracts are expressed through the `Telemetry` struct.

---

## 2. Data flow

```
                 +-------------------+
   ADC / DHT --->|  sensors::        |---> Telemetry (validated)
   ISR pulses    |  median + range   |          |
                 +-------------------+          |
                                                v
                                     +---------------------+
                                     |  soc::estimate      |
                                     +---------------------+
                                                |
                          +---------------------+---------------------+
                          |                     |                     |
                          v                     v                     v
                    guard::update        storage::append       cloud::push
                    (state machine)      (CSV on SD)           (Blynk V0-V9)
                          |                                           ^
                          v                                           |
                    relay / buzzer                       cloud::queue (offline)
```

Sampling runs on a 2 s `BlynkTimer` interval; logging runs on a 5 s interval.
Neither callback blocks on the network: `cloud::push` returns immediately when
`Blynk.connected()` is false, and the sample is appended to the ring buffer
instead.

---

## 3. Failure behaviour

| Failure | Detection | Behaviour |
| :--- | :--- | :--- |
| Pack voltage out of range | `plausible()` against `PACK_MIN_V`/`PACK_MAX_V` | Keep last good value, increment fault counter |
| DHT read failure | `isnan()` or range check | Hold last temperature, report `sensor_fault` |
| ACS712 drift | Zero offset outside 1200–2100 mV at boot | Reject calibration, keep previous offset |
| SD card missing | `SD.begin()` / `cardType()` | Logging disabled, telemetry continues |
| Wi-Fi loss | `WiFi.status()` | Retry every 15 s, buffer up to 120 samples |
| Cloud loss | `Blynk.connected()` | Retry every 5 s, flush 20 samples per cycle |
| GPS loss | `location.age() > 5 s` | Publish `no fix`, skip geofence evaluation |
| Watchdog stall | 30 s task watchdog | Controller resets and reboots |

---

## 4. Theft guard state machine

```
            arm()                       vibration >= threshold
 DISARMED ---------> ARMED -------------------------------------> ALERT
    ^                  |                                             |
    |                  |  geofence breach                            |
    |                  +-------------------------------------------->|
    |                                                                |
    +------------------- disarm() -----------------------------------+
```

Guarantees:

- **Latched:** an alert stays active until an explicit `disarm()`.
- **Rate-limited:** at most one alert per 30 s (`ALERT_COOLDOWN_MS`).
- **Auditable:** every alert increments a counter persisted in NVS and writes a
  CSV row with the current telemetry.
- **Fail-visible:** if the cloud is down the buzzer and relay still fire; the
  event is queued for the next connection.

---

## 5. State of charge

`SOC` is computed from a piecewise-linear open-circuit voltage curve for a 13S
Li-ion pack. Because terminal voltage sags with load, the estimator adds
`I × R_internal` back before the lookup (`LOAD_COMPENSATION_OHM`).

Limitations and the planned fix:

| Limitation | Impact | Planned fix |
| :--- | :--- | :--- |
| Voltage-only estimation | ±8 % error under load | Coulomb counting with a current integrator |
| No temperature compensation | Error below 0 °C | Temperature-dependent curve |
| No hysteresis | Gauge can oscillate near a breakpoint | 1 % hysteresis band + rest-time gate |

---

## 6. Timing budget

| Task | Period | Worst-case duration |
| :--- | :--- | :--- |
| GPS parse | continuous | < 1 ms per byte |
| Sensor sample | 2 s | 12 ms (15 ADC reads + DHT) |
| SD append | 5 s | 8 ms (typical), 60 ms (card erase) |
| Blynk publish | 2 s | 15 ms |
| OTA handle | continuous | 2 ms when idle |
| CLI poll | continuous | < 1 ms |

The main loop is therefore never blocked for more than ~60 ms, which keeps the
watchdog and the Blynk heartbeat satisfied.

---

## 7. Memory

| Region | Usage |
| :--- | :--- |
| Offline ring buffer | 120 × 40 B ≈ 4.8 KB (static) |
| Blynk + TLS | ~30 KB heap |
| TinyGPSPlus | ~1 KB |
| DHT | ~100 B |
| Stack headroom | > 40 KB |

The ring buffer is statically allocated so an offline period can never cause a
heap allocation failure mid-sample.
