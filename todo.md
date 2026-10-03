# TODO

---

## Resume here — buffer area: bowl count → load-cell kilograms (ACTIVE, 2026-10-03)

**What:** retire the lidar bowl-count (BWL levels 1–4) in every layer and replace it
with one universal load-scale model. Every platform is its own unit with its own id —
LDC-001 is the counter (kind `scale`), BWL-001..003 are buffer scales (new kind
`buffer`) — and several may share one ESP32. Firmware + panel, upload, Supabase, web
and `tools/fleet_sim.py` all change. Full approved plan, with decisions and
verification: `C:\Users\jagme\.claude\plans\u-r-aware-right-rustling-lemon.md`
(owner's machine).

**The bowl rule:** a buffer bowl's dry mass is 2.5 kg. A settled jump of ≥ 10 kg held
5 s is bowls loaded (or unloaded); how many = round(jump ÷ typical full bowl), the
typical figure learned from single loads (default 15 kg). Food = gross − bowls × 2.5 kg.
The count survives a power cycle but reads *unconfirmed* until the next event, or until
the platform reads empty (< 1 kg → 0).

**Panel:** big total = counter + every buffer; one row each, `C1 10.1kg`,
`B1 74.2kg 3 bw`; a partial total reads `≥ X` with a chip naming what is missing;
A/B/C cell rows leave the main page (Diagnose keeps them).

| Phase | | State |
| --- | --- | --- |
| 0 | cleanup; V1.10 field-trial code committed as a baseline (`9909e20`) | done |
| 1 | `include/load_scale.h`, `src/loadcell/load_scale.cpp` — Arduino-free core (filter + step detection, bowl tracker, zero/calibrate, weight_state) with host tests: `bash tools/host_test/run.sh` | 61/61 pass |
| 2a | buffer bank (`src/loadcell/buffer_bank.cpp`, replaces `buffer_scale.*`): B1..B3 from a config table (`:buf` console), bus clear + verified bring-up, B1 calibration imported — V1.11, `0e3a152` | done; 1000/1000 reads, 5/5 resets up (one on attempt 2 — post-reset fault contained, cause not found) |
| 2b | panel: total = counter + buffers, rows `C1`/`B1..B3`, A/B/C off the main page; Settings › Buffers (zero / calibrate / bowls); sim screenshots (`BOWLSTACK_SIM_SHOT`) — V1.13, `b01a82e` | done in sim + console; **not yet seen on the glass**; bowls-on-empty bug found on bench and fixed (65/65 host tests) |
| 2c | on-glass check by the user; real bowl test (≥10 kg load/unload, power cycle with bowls on); recalibrate B1 with ≥20 kg | next |
| 3 | Supabase `migrate_buffer.sql` / rollback (kind `buffer`, 250 kg rail, kind guard, views) | |
| 4 | upload: per-device channel refactor of `scale_telemetry` (counter JSON must stay byte-identical) | |
| 5 | cut-over of BWL-001 (stop other writers, re-kind, enable upload) | |
| 6 | web front end (kg for buffers); **ask before pushing — a push deploys** | |
| 7 | `tools/fleet_sim.py` to kg; re-kind BWL-002..024 together | |
| 8–9 | docs; remove dormant bowl code | |

**Open:** the counter's stored factor was found cleared during the field trial
(`bowlscale/cpg = 0.0`, boot says "no calibration stored") — restore with Settings ›
Scale › Restore default.

---

## Previous — `loadcell` branch

**Three NAU7802 behind a TCA9548A, feeding the same UI. RUNNING AND
CALIBRATED.** All three cells read, all three respond to load, the dashboard
shows kilograms. Nothing is published upstream yet.

**Connectivity is now wired rather than demonstrated.** The station joins the
strongest known network at boot without a finger on the glass, remembers a
network commissioned from the panel across power cycles, and reconnects on its
own; the battery band comes from the hysteresed `battery::Monitor` the discrete
product ships rather than from three bare comparisons; and charge state is
reported unknown because the firmware says so. See the top of
`src/bringup/bringup_wifi.cpp` for why WiFiManager is *not* part of that.

### Measured on the board

| | |
| --- | --- |
| cells | 9–10 SPS each against 10 configured; p-p 120–180 counts |
| **sensitivity** | **106.857 counts/g** — 18,700 counts for a 175 g reference |
| dashboard | **13 fps, ui 62%**; `ui 2%` when the reading is steady |
| free heap | ~126 kB; LVGL pool 52k/89k, 1–3% fragmentation |
| scale task | 2.8 kB of stack still free of 4 kB |

**The sensitivity agrees with the datasheet to half a per cent**, which is what
makes it trustworthy rather than merely fitted. A YZC-133 is 1.0 mV/V, the
internal LDO excites at 3.0 V, so 20 kg gives 3.0 mV; full scale at gain 128 is
±11.7 mV, so 20 kg on one cell is 2.15 M counts = 107.4 counts/g. A load split
between two cells gives that same figure on the **sum** whatever the split,
which is why one constant describes the assembly.

It also settles the range question: a signed 24-bit conversion reaches **~75 kg
on a single cell**, so the ADC is not the limit — the cell's own 20 kg rating is,
and it arrives first by a factor of four. The over-range flag stays as a
backstop rather than as something normal use will meet.

### Two faults this cost, both now fixed

**Cell A was a dry solder joint.** It presented three different ways across
reboots — converting but reading ~0 and ignoring load, converting normally, and
not acknowledging at 0x2A at all — which reads like three faults and was one.
The figure that identifies it is **peak-to-peak**: a live 350 Ω bridge at gain
128 wanders by hundreds of counts between conversions and an open input does
not move at all, while the trimmed mean the dashboard shows is *designed* to
look calm and hides the difference. p-p is now on the console line and in
`Nau7802::selfTest()`.

**The page rendered at 3 fps because every label was content-sized.** A label
that re-measures to a different width marks its flex parent's layout dirty, so
one changed digit re-ran the layout, moved its siblings and re-invalidated
them — twenty times a second, because the number is live ADC counts. The
instrumented perf line is what settled it: `ui 86% (flush 3% touch 0%)` with
11,574 px and a 250 ms frame is not the bus, not the touch chip, and cannot be
blitting cost. Fixed widths with right-aligned digits took it to 11 fps.

### Driving it

The board is on **COM10** (`USB VID:PID=303A:1001`) — native USB-Serial-JTAG,
not a CP210x. The COM6 in older docs is a *Bluetooth* port on this machine.

```
pio run -e ws-s3-loadcell -t upload --upload-port COM10
```

Every action exists in two places, and they call the same function:

| console | screen | does |
|---|---|---|
| `t` | Settings → Scale → Tare | zero every cell |
| `1` / `2` / `3` | **TARE 0** inside each cell panel | zero one cell |
| `c` | — | calibrate against the **stored** mass |
| — | Settings → Scale → Calibrate | keypad: type **any** mass, then OK |
| `x` | Settings → Scale → Clear calibration | back to counts |
| `w` | Settings → Scale → Average | step 8→16→32→64→128 |
| `?` | — | help |

**Calibrating with any mass you have:** empty the platform → `t` (or the Tare
row) → put the mass on centred → **Settings → Scale → Calibrate** → type the
grams → OK. The page shows the live deflection in counts while you type, which
is how you catch the two ways this goes wrong: nothing actually on the platform,
or the tare taken while the mass was already there. Both give a deflection near
zero and both are invisible if you only look at what you typed.

The factor **and the mass** land in NVS and survive reflashing — the keypad
opens pre-filled with whatever this unit was last calibrated against, and `c` on
the console uses that same stored mass. `BOWLSTACK_CAL_MASS_G` is now only the
value a never-calibrated board starts from. **Clear calibration removes the NVS
key** rather than writing a zero, so the build default applies again at the next
boot.

Calibration refuses a mass under 20 g, and refuses a deflection under 5,000
counts — the second is the real guard, since it scales with whatever the cells
actually produce. A sum that does not go *positive* is refused outright, which
is the check for a cell wired backwards, where the two would subtract instead of
add.

### Converter rate

**10 SPS**, set by `BOWLSTACK_SPS` (10/20/40/80/320). **The rate is the filter** —
a NAU7802 has no separate low-pass; the sigma-delta's decimation filter is it,
and its length is the output rate. 10 SPS buys two things over the 80 this
started at:

- **~2.8× less noise per sample** from eight times the integration. Measured
  peak-to-peak fell from 400–700 counts to 120–180.
- **Mains rejection.** A sinc filter at 10 Hz has nulls at every multiple of
  10 Hz, which includes 50 and 60. Hum on a metre of unshielded bridge cable is
  rejected by the converter itself. Firmware averaging cannot do this — to notch
  50 Hz it would have to average an exact multiple of the mains period, and the
  sample clock does not track it.

The readout publishes at **10 Hz**, the fastest this product wants, so the
converter feeds it exactly one fresh sample per displayed value. `n/s` on the
dashboard and the Device page is the **measured** rate and now reads 9–10.

### The averaging window

`Settings → Scale → Average`, or `w` on the console. Persisted in NVS; the build
default is `BOWLSTACK_AVG_WINDOW`. Noise falls with the **square root** of the
window and latency rises **linearly**, so the useful range runs out quickly:

| N | settling | jitter in the last digit |
|---|---|---|
| 8 | ~100 ms | ±0.7 g |
| 16 | ~200 ms | ±0.5 g |
| 32 | ~400 ms | ±0.3 g |
| 64 | ~810 ms | ±0.2 g |
| 128 | ~1.6 s | ±0.17 g |

Sweep it with the reference mass on and watch the last digit against how long
the number takes to settle after the mass lands. It also moves the frame rate:
a slower-changing number repaints less, which is what took the dashboard from
11 fps to 13.

**Step detection keeps a long window from feeling slow.** A plain moving average
at N=128 takes a full 12.8 s to work through a bowl being placed or removed —
the old samples are not noise, they describe a platform that no longer exists.
So when three consecutive samples land more than 5 g from the current average,
the window is *discarded* and the reading restarts from the new level: it jumps
at once, then re-settles at whatever averaging is selected. Quiet when nothing
is happening, immediate when something is. The console logs each one
(`cell A step +18342 counts -- filter restarted`).

This is deliberately **not** a PID. There is no loop and nothing to actuate —
the reading lags because it is averaging history, not because it is chasing a
setpoint, and derivative gain on a noisy load cell would amplify exactly the
noise the window exists to remove.

### The Device page

`Menu → Device`. Raw conversions with nothing done to them, the filtered net
beside them, peak-to-peak, the tare being subtracted, the delivered sample rate,
and the two settings that turn one into the other. Read top to bottom it
explains the dashboard's number completely — which the dashboard, being one
smoothed tared figure, deliberately does not.

### Known open

- ~~**Nothing is published upstream.**~~ **Done.** LDC-001 reports to Supabase:
  `device_status` for current state on a 20 s heartbeat, `weight_samples` for
  the history on change and every two minutes. Verified end to end on the real
  board against the live database — `weight_state ok, 817 g, 3 cells,
  104.331 counts/g`, arriving within 20 s. The transport is `src/uplink.cpp`,
  shared verbatim with the discrete product; only the payload writer
  (`src/loadcell/scale_telemetry.cpp`) differs.

  What is NOT done: the load cell cannot appear in `status_events` and never
  will — five of that table's NOT NULL columns are bowl-shaped. Its history has
  its own table, which is why `slot_burn_rate` exists.
- **The WiFi page's QR advertises an access point nothing raises.** The page
  scrolls to *"or set up from your phone:"* and a QR encoding
  `WIFI:T:WPA;S:Bowlstack-LDC-001;P:bowlstack;;` — and no code anywhere in this
  image calls `softAP()`. Scanning it gets *"network not found"*, which reads as
  a broken device. It is the honesty rule applied to an affordance rather than
  to a reading, and there are only two fixes: raise a real SoftAP **with a page
  behind it** (an AP that joins you to nothing is worse than no QR), or stop
  advertising one. Deliberately left alone for now because the fix is a UI
  decision, not a wiring one. Note that linking `net.cpp` would *not* have fixed
  it either: WiFiManager raises an **open** AP named `LDC-001`, not a WPA one
  named `Bowlstack-LDC-001`.
- **A board on USB with no cell fitted reports a healthy battery.** The ETA6098
  holds the BAT node at its charge voltage, so the divider faithfully reports
  ~4.17 V and `battery::Monitor` classifies `good`. Nothing in software
  distinguishes that from a real full cell — it needs the charger-sense mod
  below, where *charging, pinned at 4.2 V, no droop under load* is the tell.
  Adopting the hysteresed classifier did **not** fix this and must not be
  described as having done so.
- **Charge state is still unreadable**, and now says so because the firmware
  says so rather than because a fixture happened to. `demoOverrideCharging()` is
  fed `board::CHARGER_STATUS_READABLE`; the one-resistor mod is what flips it.
- **The gram split assumes matched cells.** One factor is applied to the sum,
  which makes the TOTAL right regardless of where the load sits, and the
  per-cell shares right only if the two cells have equal sensitivity. Per-corner
  calibration is the fix if that turns out to matter.
- **`src/ui/ui_screens.cpp` is dead weight here** except for `unknownState()` —
  its bowl page is no longer built. Worth moving that one function out and
  dropping the file if this branch outlives the port.
- **`buildPages()` would strand change-detection state if called twice** —
  inherited from `touch-ui`, still true, still only one call site.
- ~~**Bus B still has no pull-ups.**~~ **Gone with the bus.** The cells moved
  behind a TCA9548A on GPIO21/16 in 3e58f87 — the camera's SCCB pair, with R4/R5
  4.7 kΩ already fitted — so there is no bit-banged bus and no internal-pull-up
  compromise left. The whole trunk runs at 400 kHz. Each downstream **stub**
  still needs its own pair, because a mux is a switch and not a buffer; see
  `include/i2cmux.h`.
- **ui 74% is still high for 11 fps.** What remains after the layout fix is
  genuine software glyph rendering — ~13,000 px of 4 bpp antialiased 48 px
  digits, eleven times a second, on a core with no 2D acceleration. The levers
  left are a smaller total, fewer digits changing (grams round harder than raw
  counts do), or a partial-digit redraw, and none is obviously worth it yet.
- **The split assumes matched cells** — press one corner and watch that cell
  move alone. The total is honest wherever the load sits; the per-cell shares
  are only right if the two sensitivities match.

---

## Inherited from `touch-ui`

Deferred work on the Waveshare ESP32-S3-Touch-LCD-2 port. Rationale for each is
in [docs/waveshare_port.md](docs/waveshare_port.md). Everything below predates
the load-cell work and describes the ToF build.

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
