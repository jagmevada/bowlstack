# TODO

Deferred work on the `touch-ui` branch (Waveshare ESP32-S3-Touch-LCD-2 port).
Rationale for each is in [docs/waveshare_port.md](docs/waveshare_port.md).

---

## In the lab — hardware not yet in hand

### 1. Charger-sense mod — one resistor

The board gives no way to read charge state: the ETA6098's `STAT` output drives
the charge LED's cathode directly, and that net has two nodes with no module pin
on it.

```
ETA6098 STAT (LED1 cathode) --[10k]-- free GPIO (INPUT_PULLUP)
```

- [ ] Tack the 10k from the `STAT` node to a free GPIO (candidates: IO18 on
      P1-6, or IO9/IO14 on P2)
- [ ] Set `PIN_CHARGING` to that pin
- [ ] Set **`CHARGING_ACTIVE_LOW = true`**

> `STAT` is open-drain and pulls **LOW** while charging. This **inverts** the
> discrete board's convention, where the pin reads HIGH while charging because
> it senses the charger's 5 V rail through the input clamp. The config constant
> already exists and its comment already describes this TP4056-style case — it
> is a flag flip, not new code.

**Until then, publish `charging` as unknown, not `false`.** The codebase refuses
throughout to state what it cannot measure — a hard `false` is a claim, and this
device has no evidence for it. `device_status.charging` needs to become
tri-state (or gain a `chargingKnown` companion), and `supabase/schema.sql`'s
`charging` column is already nullable, so the server side may need nothing.

### 2. TCA9548A mux — the sensor bus

**The ESP32-S3 has two I2C controllers and touch+IMU already own one.** The
discrete build's two-bus blast-radius split cannot be reproduced; one hardware
port remains.

The mux is the answer, and it is the design `sensor_logic.md` §6 already
specifies for dual-sensor redundancy. It isolates **per sensor** rather than per
pair, so it is strictly better than two buses at the job two buses were doing.

- [ ] Order the TCA9548A
- [ ] Split logical level from physical sensor — `SENSORS[8]` plus
      `LEVEL_SENSORS[4][2]`; 1:1 at first, public API unchanged
- [ ] Add the `select(sensorIndex)` hook before each transaction group — a
      no-op in direct-bus mode, a channel write in mux mode
- [ ] Make XSHUT addressing conditional; the walk is unnecessary behind the mux

> **The mux isolates I2C only.** A sensor on a deselected channel keeps ranging
> — continuous mode runs inside the sensor, not over the bus. Optical exclusion
> must be driven by `stopContinuous`/`startContinuous`, never inferred from mux
> state.

**Interim wiring** (works, but uncontained — a locked bus is still *detected*
via the `0xFFFF` signature and `SENSOR_STALE_MS`, just not isolated):

| Signal | GPIO | Header |
| --- | --- | --- |
| Sensor I2C SDA | 21 | P1-7 |
| Sensor I2C SCL | 16 | P1-4 |
| XSHUT `f1` | 2 | P1-1 |
| XSHUT `f2` | 4 | P1-2 |
| XSHUT `f3` | 13 | P2-7 |
| XSHUT `f4` | 12 | P2-8 |
| 3V3 / GND | — | P2-1 / P2-2 |

GPIO21/16 is the camera SCCB pair: a complete I2C bus with 4.7k pull-ups
already fitted (`R4`, `R5`), idle unless a camera is plugged into `J1`.

> Sensor power **must** come off P2 — P1 carries only GND and 5 V.
> Avoid GPIO17 for I2C: `R6` is a 10k pull-*down* a pull-up would have to fight.

### 3. Sensor integration

- [ ] Point `config.h` at the pins above under `BOWLSTACK_BOARD_WAVESHARE_S3`
- [ ] Confirm the XSHUT walk works on S3 (all chosen pins are output-capable —
      the ESP32's input-only 34–39 problem does not exist here)
- [ ] Re-tune `PRESENT_BELOW_MM` / `ABSENT_ABOVE_MM` against real bowls
- [ ] Verify `OFFSET_MM[]` per part

### 4. Battery calibration

- [ ] Read the `pin` figure from the bring-up console
- [ ] Measure the cell at the MX1.25 header with a multimeter
- [ ] Set `-DBOWLSTACK_BATTERY_CAL=<cell/pin>` (nominal 3.0)

> Matters **more** on this board than the discrete one. The on-board divider is
> 200k/100k, so source impedance is 66.7 kΩ against the ~10 kΩ the SAR ADC
> wants. `C31` handles the dynamic case; static leakage offset across 66.7 kΩ
> does not average away and differs per chip.

---

## On this machine

- [ ] **`include/secret.h` does not exist here**, so `esp32dev` and
      `esp32dev-debug` cannot build on this PC at all — `src/telemetry.cpp:10`
      and `src/net.cpp:7` both include it. Copy `include/secret.h.example` and
      fill it in. (`ws-s3-bringup` is unaffected; it links no networking.)

---

## Firmware, no hardware needed

- [ ] `ui` module replacing `indicators` — core 1, prio 1, consuming
      `tasks::snapshot()` through the existing mutex, exactly as
      `indicatorTask` does
- [ ] A **mock state source** so the UI can be developed and demoed on a bare
      board with no sensors attached — this is the current blocker on progress,
      not a nicety
- [ ] Extend `tasks::printStackHeadroom()` to cover `ui`. 12 KB is an estimate,
      not a measurement, and stack exhaustion on ESP32 surfaces as a
      corrupt-looking crash far from its cause
- [ ] Compile `indicators` out under the board macro
- [ ] Screens: stock, health, setup

> The local UI must not contradict [FRONTEND_HANDOFF.md](docs/FRONTEND_HANDOFF.md) §4:
> `discontiguous` renders a **fault, not a count**; `degraded` shows the number
> marked as a lower bound; battery is a **band**, never a percentage, and `null`
> means "no cell", never a flat-battery icon; `levels[0]` is `f1`, the bottom
> bowl.

---

## Open questions

- Does the touch UI want an on-screen SSID picker? Commissioning still needs a
  phone for the captive portal. It is the one screen that would justify a
  text-entry widget.
- Enclosure and viewing angle. A 2" panel replaces LEDs that were readable
  across a kitchen; a screen you have to approach is a different affordance.
