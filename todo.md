# TODO

Deferred work on the `touch-ui` branch (Waveshare ESP32-S3-Touch-LCD-2 port).
Rationale for each is in [docs/waveshare_port.md](docs/waveshare_port.md).

---

## Resume here

**State as of 2026-08-17, after an overnight optimisation pass.** Bowlstack is
being ported from the discrete ESP32 board to a **Waveshare ESP32-S3-Touch-LCD-2**
— 2" 240×320 IPS, CST816D touch, on-board Li-ion charging. The touchscreen
**replaces** the five discrete status LEDs. Work is on branch `touch-ui`; the
discrete build on `main` is untouched.

### First thing in the morning

1. **Tap the screen.** A bug was found and fixed overnight where initialising
   the sensors killed the touch controller (see below). The chip is proven
   reachable again, but only a finger closes that loop.
2. Swipe to the scope page and watch the perf line — it should read around
   **10–12 fps at ~11% busy**.
3. Tap the WiFi icon. The network list is **real** now.

### What the night produced

| | before | after |
| --- | --- | --- |
| stock page | ~8 fps, full repaint every frame | 1–4 fps, **10% busy**, 6–9 ms frames |
| scope page | 14 fps, **79% busy**, 49,000 px/frame | 10–12 fps, **11% busy**, 2,400 px/frame |
| preview LVGL pool | 51k/55k used, 11% frag (hangs) | 51k/183k, 1% frag |

Low fps on the stock page is the **goal**, not a regression: it redraws when the
bowl count changes and is idle otherwise. That is the CPU the sensors, WiFi and
Supabase need.

- **Real WiFi** — async scan, bounded 12 s join, live status. Verified against
  `Amba`: 15–18 networks found, joined, `ip 192.168.1.19 rssi -61`.
- **Sensor stack ported** — `sensor_array` + `bowl_logic` + `trimmed_window`
  compile and run on the new pins. With nothing attached every level reports
  offline/unknown, which is correct.
- **Idle timeout** — returns to the stock page after 60 s untouched.
- **Boot logo**, touch outlines removed, QR bezel fixed.

### Found by adversarial review, still open

Two independent reviews ran against this branch overnight. Most findings are
fixed and committed; these three are recorded rather than fixed, because each
needs a decision rather than a patch.

**`SensorArray::poll()` is documented non-blocking and is not.** For every
`Offline` sensor it calls `maybeRecover()` → `initSensor()`, which contains a
hard `delay(10)` plus a driver probe. With nothing attached all four are offline
and share one deadline, so the loop takes a synchronised **40–60 ms hit every
15 s**. Worse in the lab: `initSensor` sets a 500 ms timeout and Pololu's
`init()` spins twice on a status register without yielding, so **one half-alive
clone that ACKs but never calibrates turns a single `poll()` into a ~1 s
freeze**, and four into ~4 s of dead UI and dead touch.

> This is the single-loop architecture `docs/firmware.md` §2 exists to escape.
> The proper fix is the FreeRTOS split that `docs/waveshare_port.md` §4 already
> specifies — `uiTask` at prio 1, below `sensorTask` at prio 3 — and it should
> happen when the sensors are actually wired, not before.

**The scan rebuild costs a 124 ms frame.** Measured. Ten network rows are
deleted and recreated whenever a scan returns. Fine at a 20 s cadence, visible
as a hitch if anyone is swiping at that moment.

**`buildPages()` would strand change-detection state if called twice.**
`ui_screens`' `prev_` and `ui_status`' caches are not reset, so a rebuilt widget
tree would show build-time placeholders until the state happened to change. Only
one call site exists today.

### Numbers, measured on hardware

| | |
| --- | --- |
| busy, whole loop (LVGL + WiFi + sensors) | **9–10%** |
| worst frame, steady | 1–8 ms |
| worst frame, scan rebuild | 124 ms |
| scope page | 2,400 px/frame, 1 flush |
| LVGL pool, device | 37k / 59k, 2% frag |
| ESP heap with WiFi up | ~171 KB free |

> `busy%` now brackets the **whole loop iteration**. Until the last commits it
> wrapped `lv_timer_handler()` alone, so the earlier "0–1% busy" figures
> excluded the sensors and the radio — the two things the number was being used
> to argue there was headroom for.

### Wiring, when you have the parts

| Signal | GPIO | Header |
| --- | --- | --- |
| Sensor I2C SDA | 21 | P1-7 |
| Sensor I2C SCL | 16 | P1-4 |
| XSHUT `f1`–`f4` | 2, 4, 13, 12 | P1-1, P1-2, P2-7, P2-8 |
| Sensor 3V3 / GND | — | **P2-1 / P2-2** |

> **P1 carries only GND and 5 V** — sensor power must come off P2.
> **Never let anything call `Wire.begin()` on this board.** Port 0 belongs to
> the touch controller; that was the overnight bug.

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

### 3b. "No battery" reads as a FULL battery on this board

Observed on hardware, 2026-08-17, with **no cell connected** and the board on
USB alone:

```
battery: pin 1392 mV -> cell 4176 mV
```

That is not a measurement error — with no cell present the ETA6098 regulates the
`BAT` node to its charge voltage, and the divider faithfully reports it. But
4176 mV lands squarely in the `good` band, and **neither existing guard catches
it**: it is far above `BATTERY_ABSENT_BELOW_MV` (2500) and below
`BATTERY_IMPLAUSIBLE_ABOVE_MV` (4400).

So on this board, "no battery fitted" currently publishes as "battery good" —
the same class of failure as the floating pin that once read 6365 mV as
"100% (good)", and precisely what `FRONTEND_HANDOFF.md` promises cannot happen
("`null` means **no cell detected**").

- [ ] Decide the detection strategy. A resting voltage alone cannot distinguish
      a full cell from a charger holding an empty socket — they are the same
      node at the same voltage.
- [ ] Options worth weighing: read charge state once the §1 mod exists (charging
      + pinned at 4.2 V + no droop under load ⇒ no cell); or watch for the
      total absence of the millivolt-scale wander a real cell always shows; or
      accept it and document that `battery_level` is meaningful only on
      battery power.

> Worth settling **before** the fleet ships, because the failure is silent and
> in the reassuring direction.

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

> Both discrete environments were **verified to still build** with the port
> changes in place — `esp32dev` and `esp32dev-debug` both SUCCESS — by injecting
> a stub copy of `secret.h.example` from outside the tree with
> `PLATFORMIO_BUILD_FLAGS="-I <stubdir>"`. So the missing header is the only
> thing stopping them here; `build_src_filter` did not break them.

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
- [x] ~~**Image retention**~~ — **PARKED, deliberately.** Implemented, verified
      harmless, then switched off. Decided 2026-08-17.

      A faint `2` from an earlier build did survive several power cycles, and it
      was not a stale buffer (a framebuffer cannot survive a reboot, and the
      ST7789's GRAM is overwritten by `fillScreen()` at every boot). But the
      panel is **IPS-TFT, not OLED**, and that distinction decides the priority:

      | | mechanism | permanent? |
      | --- | --- | --- |
      | OLED burn-in | organic emitters age differentially | **yes** |
      | LCD image persistence | residual DC charge in the liquid crystal | **no** — self-recovers |

      What was seen is the second, which is why it faded on its own. There is no
      cumulative damage to defend against, so the ongoing complexity is not
      worth it on this hardware.

      The mechanism stays in the tree, off: `ui::pixelShiftTick()` is
      implemented and the calls in `sim_main.cpp` and `bringup_display.cpp` are
      commented out, one line each. Gallery **page 6** still demonstrates it at
      1 Hz with a toggle.

      Re-open this only if the panel is ever swapped for an OLED, where the same
      pattern of use would cause real, permanent damage.

      > Verified on hardware while it was live: whole-pixel shifting **softens
      > nothing**. 1 px hairlines stayed crisp white at all 8 offsets, no
      > greying. Integer `translate_x/y` relocates rendered values without
      > resampling them — worth knowing if this is ever revived.
- [ ] **Dadabhagwan Foundation logo as the boot splash.** The colour-bar test
      pattern is now off by default (`-DBRINGUP_COLOR_BARS=1` brings it back)
      and a plain "Bowlstack" text splash stands in its place. Draw the logo
      with raw LovyanGFX in `panelSelfTest()`, *before* LVGL initialises, so
      the boot has no dark gap. Source art wanted as a 1-bit or RGB565 C array
      — at 240×320 an RGB565 full-screen image is 150 KB of flash, so a
      smaller centred logo on black is the better trade.

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
- **The enclosure has an electrical job too, not just a mechanical one.**
  Observed 2026-08-17: holding the bare board by the edge of the panel makes the
  display flicker — body capacitance coupling into an ungrounded panel edge.
  It is a handling artefact of a bare board, but a kitchen is exactly where
  someone grabs a device by its screen, and the units run on a floating battery
  with no earth reference. Worth designing for: a bezel that stops fingers
  reaching the panel edge, and a ground plane or shield tied to the board's GND
  behind it.
