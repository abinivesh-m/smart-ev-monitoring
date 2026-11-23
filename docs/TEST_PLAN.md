# Test Plan & Results

All tests were executed on the bench prototype (ESP32 DevKit V1, ACS712-30A,
bench PSU, electronic load, NEO-6M with a roof antenna). Where a test requires
the vehicle, the method is marked **field**.

---

## 1. Test matrix

| ID | Area | Test | Method | Pass criteria | Result |
| :--- | :--- | :--- | :--- | :--- | :--- |
| T-01 | Boot | Cold boot, no SD card | Power on without card | Boots, logs `card not present`, telemetry still publishes | ✅ Pass |
| T-02 | Boot | Cold boot, no Wi-Fi | Power on with AP off | Boots, buffers samples, reconnects automatically | ✅ Pass |
| T-03 | ADC | Voltage accuracy | Bench PSU 36/42/48/54 V vs Fluke 117 | Error < ±0.5 % | ✅ ±0.4 % |
| T-04 | ADC | Divider linearity | Sweep 30–60 V in 2 V steps | R² > 0.999 | ✅ 0.9996 |
| T-05 | ADC | Current accuracy | Electronic load 0/2/5/10 A vs clamp meter | Error < ±0.3 A | ✅ ±0.21 A |
| T-06 | ADC | Current zero drift | 30 min soak, no load | Drift < 20 mA | ✅ 11 mA |
| T-07 | DHT | Temperature accuracy | Ice bath + room + warm air vs reference thermometer | Error < ±1.0 °C | ✅ ±0.6 °C |
| T-08 | DHT | Disconnect fault | Unplug data line at runtime | `sensor_fault`, last value held, no reboot | ✅ Pass |
| T-09 | SOC | Discharge tracking | 4 h constant 2 A discharge, 54.6 → 39.0 V | Monotonic, no oscillation | ✅ Pass |
| T-10 | SOC | Load sag rejection | Apply 8 A step at 50 % SOC | Reported SOC drop < 2 % | ✅ 1.3 % |
| T-11 | Guard | Vibration alert latency | Tap test, 10 trials | Alert < 500 ms | ✅ 180–340 ms |
| T-12 | Guard | False positive rate | 2 h parked, heavy rain on cover | 0 alerts | ✅ 0 |
| T-13 | Guard | Geofence trigger | GPS simulator, 150 m radius, 1 m/s | Alert between 150–200 m | ✅ 168 m |
| T-14 | Guard | Disarm authority | Blynk `V10 = 0` during alert | Relay releases, buzzer stops, state = 0 | ✅ Pass |
| T-15 | Guard | Alert rate limit | Sustained vibration 60 s | Exactly 2 alerts (30 s cooldown) | ✅ Pass |
| T-16 | Storage | 24 h soak | 5 s interval logging | 17,280 rows, no corruption | ✅ Pass |
| T-17 | Storage | Card removal | Pull card while logging | Logging disabled, telemetry continues | ✅ Pass |
| T-18 | Cloud | Offline buffer | Wi-Fi off for 10 min | All buffered samples delivered on reconnect | ✅ Pass |
| T-19 | Cloud | Publish latency | Measure Blynk write round trip | < 1 s on LAN | ✅ 320 ms avg |
| T-20 | OTA | Firmware update | Flash over Wi-Fi | Completes, telemetry resumes | ✅ 42 s |
| T-21 | Power | Deep sleep current | USB power meter in parked mode | < 3 mA | ✅ 1.8 mA |
| T-22 | Power | Wake on vibration | Tap while asleep | Wakes and publishes within 3 s | ✅ 2.4 s |
| T-23 | Reliability | 72 h soak | Continuous run, all peripherals | No reset, no memory leak | ✅ Pass |
| T-24 | Field | Ride test | 2 weeks, mixed riding | No false theft alerts, GPS track complete | ⏳ Planned |

---

## 2. Accuracy calculations

### Voltage

```
Reference 48.00 V → reported 48.19 V
Error = (48.19 - 48.00) / 48.00 = +0.40 %
```

Across the sweep the worst-case error was **+0.18 V (+0.40 %)** at 45 V, which is
inside the ±0.5 % target and well inside the ±2 % needed for a usable SOC gauge.

### Current

```
Reference 5.00 A → reported 5.21 A
Error = +0.21 A (+4.2 % of reading, 0.7 % of full scale)
```

The absolute error is what matters for protection thresholds; the relative error
is dominated by the ACS712's own ±1.5 % specification.

### Geofence

```
Anchor 12.971600, 77.594600
Trigger 12.973100, 77.594600  → 167.8 m
Configured radius: 150 m
Overshoot: 17.8 m (one GPS update at 1 m/s)
```

Overshoot is bounded by the GPS update rate, not the maths; the haversine
implementation is accurate to centimetres at this scale.

---

## 3. How to reproduce

```bash
# 1. Flash the firmware
pio run -t upload

# 2. Open the monitor and verify boot
pio device monitor

# 3. Inject a theft event without touching hardware
> sim
[GUARD] ALERT: simulated tamper (total 1)

# 4. Check state and counters
> status

# 5. Re-zero the current sensor with the load disconnected
> cal
[ADC ] zero offset calibrated: 1648 mV
```

For the voltage sweep, use a bench supply with a current limit of 100 mA and
increase in 2 V steps, recording `status` output at each step.

---

## 4. Known issues

| Issue | Severity | Status |
| :--- | :--- | :--- |
| SOC drifts ±8 % under sustained load | Medium | Coulomb counting planned (v1.1) |
| GPS cold start 30–60 s | Low | Documented; assist-GPS not available on NEO-6M |
| SD card erase stalls the loop for ~60 ms | Low | Acceptable; batching planned |
| No physical disarm key | Medium | Planned for v1.1 |
| Enclosure not yet IP-rated | High | In progress |
