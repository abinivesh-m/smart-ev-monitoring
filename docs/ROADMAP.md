# Roadmap — from prototype to product

Current status: **75 % complete**. The firmware and cloud pipeline are done and
validated on the bench; the remaining work is hardware integration, field
validation, and certification.

---

## Phase 1 — Prototype ✅ Complete

| Item | Deliverable | Status |
| :--- | :--- | :--- |
| Controller bring-up | ESP32 + toolchain, OTA, watchdog | ✅ |
| Sensor layer | Voltage, current, temperature, vibration | ✅ |
| SOC estimator | 13S Li-ion curve with load compensation | ✅ |
| Theft guard | Vibration + geofence + immobiliser relay | ✅ |
| Cloud | Blynk dashboard, events, arm/disarm | ✅ |
| Storage | microSD CSV logging, offline ring buffer | ✅ |
| Diagnostics | Serial CLI, fault counters | ✅ |
| Bench validation | 23 of 24 test cases passing | ✅ |

---

## Phase 2 — Hardware integration 🚧 In progress (≈ 10 % of remaining work)

| Item | Acceptance criteria | ETA |
| :--- | :--- | :--- |
| Enclosure | IP65, cable glands, desiccant, serviceable fuse | 1 week |
| Harness | Automotive connector, 5 A fuse, strain relief | 1 week |
| Mounting | Vibration sensor rigidly bonded to frame | 3 days |
| Thermal | No component above 70 °C at 40 °C ambient | 3 days |

---

## Phase 3 — Field validation ⏳ Planned (≈ 8 %)

| Item | Acceptance criteria | ETA |
| :--- | :--- | :--- |
| 2-week ride test | Zero false theft alerts, complete GPS track | 2 weeks |
| Range test | Telemetry holds over 500 m from home AP | 2 days |
| Rain test | IP65 maintained, no ingress | 1 day |
| Recovery test | Power cut mid-ride recovers cleanly | 1 day |

---

## Phase 4 — Firmware v1.1 ⏳ Planned (≈ 5 %)

| Feature | Reason |
| :--- | :--- |
| Coulomb counting | Remove the ±8 % SOC error under load |
| IMU tilt fusion (MPU6050) | Detect wheel removal and towing |
| Physical disarm key | Remove the single point of failure in the cloud path |
| Log rotation | Bound SD usage over months |
| OTA rollback | Recover from a bad image without physical access |

---

## Phase 5 — Certification ⏳ Planned (≈ 2 %)

| Item | Reason |
| :--- | :--- |
| EMC pre-scan | Buck converter and relay switching emissions |
| Reverse-polarity protection | Field wiring mistakes |
| Fuse coordination study | Ensure the harness fuse clears before the pack fuse |

---

## Definition of done

The project is 100 % complete when:

1. The unit runs for 14 days on a vehicle with no resets and no false alerts.
2. Every test in `docs/TEST_PLAN.md` passes, including T-24.
3. The enclosure is IP65 and the harness is fused and strain-relieved.
4. A second unit can be built from the repository and the BOM without help.
5. The README, wiring guide, and test plan match the shipped hardware.

---

## What is intentionally out of scope

- Cellular backhaul (the unit is Wi-Fi only; a 4G modem is a separate product).
- Battery management (the unit monitors, it does not balance or protect cells).
- GPS anti-spoofing.
- Mobile app (Blynk provides the client).
