# Wiring & Power

> ⚠️ **Danger:** a 48 V traction pack can deliver hundreds of amps into a fault.
> Work with the pack disconnected and pre-charged through a resistor. Use a
> fuse on every positive lead. Never probe a live pack with the ESP32 attached.

---

## 1. Block diagram

```
 48 V PACK ──┬── FUSE 5 A ── BUCK 48→5 V ── 5 V rail ── ESP32 VIN
             │                                            │
             ├── DIVIDER 100k/10k ──────────────── GPIO34 │
             │                                            │
             └── ACS712 (in series with load) ──── GPIO35 │
                                                          │
 DHT22 ── 10k pull-up ───────────────────────────── GPIO4  │
 SW-420 ── pull-up (internal) ──────────────────── GPIO27  │
 NEO-6M TX ─────────────────────────────────────── GPIO16  │
 NEO-6M RX ─────────────────────────────────────── GPIO17  │
 microSD (VSPI) ── CS 5, SCK 18, MISO 19, MOSI 23 ────────┘
 Relay IN ──────────────────────────────────────── GPIO26
 Buzzer +  ─────────────────────────────────────── GPIO25
```

---

## 2. Voltage divider

The pack must be scaled into the ESP32's 0–3.3 V ADC window.

```
Vout = Vin × R2 / (R1 + R2)
```

With `R1 = 100 kΩ` and `R2 = 10 kΩ`:

```
Vout = 54.6 × 10 / 110 = 4.96 V   ← too high for the ADC
```

Use `R1 = 100 kΩ`, `R2 = 10 kΩ` only for packs up to ~33 V. For a 48 V pack use
**`R1 = 470 kΩ`, `R2 = 33 kΩ`**:

```
Vout = 54.6 × 33 / 503 = 3.58 V   ← still above 3.3 V at full charge
```

The validated build uses **`R1 = 470 kΩ`, `R2 = 27 kΩ`** (ratio 18.4:1):

```
Vout = 54.6 × 27 / 497 = 2.97 V   ✓
Vout = 39.0 × 27 / 497 = 2.12 V   ✓ (empty pack)
```

Set the firmware constant accordingly:

```cpp
#define DIVIDER_RATIO 18.4f
```

Add a **3.3 V Zener** and a **100 nF** capacitor from the divider midpoint to
ground to clamp transients and filter switching noise.

Calibrate with a bench supply:

1. Apply a known voltage (e.g. 48.0 V).
2. Read `status` over serial.
3. Adjust `DIVIDER_RATIO = 48.0 / measured_voltage × current_ratio`.
4. Repeat at 40 V and 54 V; the error should stay under ±0.5 %.

---

## 3. Current sensing (ACS712-30A)

```
PACK + ── ACS712 IP+ ── IP- ── LOAD +      (sensor in series)
ACS712 VCC ── 5 V
ACS712 GND ── GND
ACS712 OUT ── GPIO35
```

- Sensitivity: **66 mV/A** (30 A variant), quiescent output **VCC/2 = 2.5 V**.
- The ESP32 ADC is 3.3 V-referred, so the 2.5 V midpoint is safe but leaves only
  ±0.8 V of headroom → roughly ±12 A measurable without a divider.
- For the full ±30 A range, divide the output with a **10 kΩ / 20 kΩ** pair and
  set `ACS712_MV_PER_AMP` to `66 × (2/3) = 44`.

Zero calibration runs at boot with the load disconnected (`cal` command or
automatic). Never calibrate while current is flowing.

---

## 4. Vibration sensor (SW-420)

```
SW-420 VCC ── 3.3 V
SW-420 GND ── GND
SW-420 DO  ── GPIO27 (INPUT_PULLUP, ISR on FALLING)
```

The module's comparator output is open-collector; the internal pull-up is
sufficient. Sensitivity is set by the on-board potentiometer — start at 50 % and
increase until a firm tap produces 4+ pulses in a 2 s window (the firmware
threshold is `VIBRATION_THRESHOLD`).

Mount the sensor rigidly to the frame, not to bodywork that flexes.

---

## 5. GPS (NEO-6M)

```
NEO-6M VCC ── 3.3 V (module has an LDO; 5 V also works)
NEO-6M GND ── GND
NEO-6M TX  ── GPIO16 (ESP32 RX2)
NEO-6M RX  ── GPIO17 (ESP32 TX2)
```

Place the antenna with a clear view of the sky — under a plastic fairing, not
under metal. Cold start takes 30–60 s; the first fix is discarded until
`location.age() < 5 s`.

---

## 6. microSD

```
SD VCC  ── 3.3 V
SD GND  ── GND
SD CS   ── GPIO5
SD SCK  ── GPIO18
SD MOSI ── GPIO23
SD MISO ── GPIO19
```

Use a card formatted **FAT32** with 32 KB clusters. The firmware creates
`/logs/` and one CSV per UTC day. A 8 GB card holds roughly 18 months of 5 s
samples.

---

## 7. Relay and buzzer

```
GPIO26 ── Relay IN        (module VCC 5 V, GND common)
GPIO25 ── Buzzer +        (buzzer − to GND, 100 Ω series for a passive buzzer)
```

The relay is wired **normally-closed** through the ignition line: energised =
vehicle can start, de-energised = immobilised. This means a dead ESP32 or a cut
wire also immobilises the vehicle — the safe direction for an anti-theft device.
Set `RELAY_ACTIVE_LOW` to match your module.

Add a **flyback diode** across the relay coil if your module does not already
include one.

---

## 8. Power budget

| Load | Typical | Peak |
| :--- | ---: | ---: |
| ESP32 (Wi-Fi active) | 120 mA | 240 mA |
| GPS NEO-6M | 45 mA | 60 mA |
| DHT22 | 1.5 mA | 2.5 mA |
| SD card | 5 mA | 100 mA |
| Relay coil | 70 mA | 70 mA |
| Buzzer | 0 mA | 30 mA |
| **Total @ 5 V** | **~242 mA** | **~502 mA** |

A 48 V → 5 V buck rated at **3 A** leaves ample margin. Add a **470 µF** bulk
capacitor on the 5 V rail and a **100 nF** decoupling capacitor at the ESP32.

Parked deep sleep (Wi-Fi off, GPS unpowered) measures **1.8 mA** average, so a
typical 20 Ah auxiliary battery lasts several weeks.

---

## 9. Enclosure notes (in progress)

- IP65 ABS box, 120 × 80 × 55 mm.
- Cable glands for pack, GPS antenna, and relay harness.
- Conformal coating on the PCB; desiccant sachet inside.
- Fuse holder accessible without opening the enclosure.
- Vibration sensor bolted to a metal bracket bonded to the frame.
