# Bowlstack

Counts how many food bowls are stacked on a serving station, so a kitchen
coordinator can see how much food is left — reducing waste and giving early
warning when a station is running short.

## Objective

Each station reports, against its **device ID**, how many bowls are currently
stacked on it (0–4). Bowls are large tapered stainless steel food vessels,
stacked one on top of another with a steel cover between them.

## Physical setup

Everything mounts on a **6 ft vertical pipe**:

- **ESP32 + battery** at the top of the pipe.
- **4x VL53L0X ToF sensors** down the pipe, starting at a ground offset and
  then spaced every **1.2 ft** — the height of one bowl. Each sensor therefore
  sits at the vertical mid-point of its bowl in the stack.

Sensors aim horizontally at the bowl wall:

Levels are numbered bottom-upward, `f1` lowest through `f4` highest:

| Level | Detects                  |
| ----- | ------------------------ |
| `f1`  | 1st bowl (bottom of stack) |
| `f2`  | 2nd bowl                 |
| `f3`  | 3rd bowl                 |
| `f4`  | 4th bowl (top of stack)  |

### Bowl geometry

- Height **1.2 ft**, base diameter **1.5 ft**, top diameter **2 ft**.
- Taper is therefore `atan(0.25 / 1.2)` = **11.8° from vertical**.
- Wall is aged stainless steel, roughly **25% diffuse / 50% specular**, rest
  absorbed (approximate, not measured).

### Mounting distance constraint

The specular component reflects a horizontal beam back **upward at 23.5°**
(twice the taper angle), rising `0.435 x D` by the time it returns to the
sensor plane. With sensors pitched 366 mm (1.2 ft) apart:

- **Mount within ~300 mm of the bowl wall.** The reflection then rises ~130 mm,
  well clear of the next sensor up.
- **Never mount near 841 mm** — that is exactly where one sensor's specular
  return lands on the sensor above it.

Within ~300 mm the 25° emission cones also do not overlap, so all four sensors
can range **concurrently** without interfering.

## Stack logic

Bowls rest on each other; none can float. So the only physically valid states
are contiguous from the bottom up:

| Bowls | Sensors reading present |
| ----- | ----------------------- |
| 0     | none                    |
| 1     | `f1`                    |
| 2     | `f1` + `f2`             |
| 3     | `f1` + `f2` + `f3`      |
| 4     | all four                |

**Any non-contiguous pattern is a fault**, not a count. If `f2` detects a bowl
while `f1` does not, something is wrong — a failed sensor, a misaligned mount,
or an obstruction — and it must be reported as an error rather than silently
counted.

Contiguity also works in the device's favour. A level whose sensor is dead but
which sits **below** an observed bowl is *inferred present* — physics proves a
bowl is there — so one dead sensor low in the stack costs nothing. Equally, an
Unknown **above** an observed Absent proves nothing either, because nothing can
rest on an empty level, so the count stays exact. Only an Unknown between the
top bowl and the first Absent leaves the count ambiguous (`DEGRADED`).

### Presence thresholding

Each level is thresholded with hysteresis (Schmitt trigger): present below
`PRESENT_BELOW_MM`, absent above `ABSENT_ABOVE_MM`, previous state held
between. A single threshold would chatter on measurement noise alone, and every
flap would be a spurious change report.

Both directions are debounced, and they must be debounced **separately**:

- **Disappearance** needs `DROPOUT_MISSES` consecutive out-of-range reads.
- **Appearance** needs `MIN_VALID_SAMPLES` in the window before a reading is
  trusted at all. Without this, losing a target took three misses but gaining
  one took a single hit — a forearm or tray crossing a beam for one 50 ms
  measurement would register as a bowl. It also guarantees the trim is active,
  since below `2*TRIM+2` samples trimming is skipped and a lone outlier would
  be reported with `stdev 0.00`, looking maximally confident.

### Sensor health

A sensor is `Offline`, `Warming`, or `Online`, and the distinction matters more
than it looks.

**`Warming` exists because "no measurement yet" is not "no bowl".** Warm-up ends
at the first definite *conclusion* — enough samples to trust a distance, or
enough consecutive misses to confirm nothing is there — not at the first
measurement. Otherwise a station booted with bowls already stacked would
publish a confident count of zero before its first result arrived.

**Runtime failure is detected, not absorbed.** When nothing acknowledges on the
bus the driver's `readReg` returns `0xFF` per byte, which also makes the
data-ready flag read true, so a dead module presents as an endless stream of
out-of-range results. Left unchecked that is indistinguishable from "no target"
and would produce a **wrong stack count reported as OK** — the worst failure
this device can make. Two independent detectors:

- `0xFFFF` from the range register is an I2C failure signature, never a
  distance (a live sensor with no target reads ~8190).
- No completed measurement within `SENSOR_STALE_MS` catches a sensor that still
  answers its registers but has stopped ranging.

Either demotes the sensor to `Offline`, whereupon recovery is retried on a
`RECOVER_RETRY_MS` backoff so a permanently dead module cannot spin the loop.

> **Not yet covered:** a sensor that answers normally but reports *wrong*
> distances. No single sensor can detect that — it is the reason for the
> planned dual-redundancy cross-check.

## Hardware configuration

Two independent hardware I2C buses, two sensors each. Splitting the bus halves
the blast radius: a slave that locks one bus cannot take down all four sensors.

| Signal     | GPIO | Notes                        |
| ---------- | ---- | ---------------------------- |
| I2C0 SDA   | 17   | serves `f1` + `f2`           |
| I2C0 SCL   | 16   | note: SDA/SCL are not 16/17  |
| I2C1 SDA   | 21   | serves `f3` + `f4`           |
| I2C1 SCL   | 22   |                              |
| XSHUT `f1` | 32   |                              |
| XSHUT `f2` | 33   |                              |
| XSHUT `f3` | 23   | chosen for harness length    |
| XSHUT `f4` | 26   |                              |

SDA/SCL are shared within a bus; XSHUT is unique per sensor. All four take
**VIN → 3V3** and **GND → GND**; the breakout `GPIO1` interrupt pin is unused,
since the firmware polls.

> **GPIO 34–39 cannot be used for XSHUT.** They are input-only on the ESP32 —
> no output driver — so they cannot pull XSHUT low.

### XSHUT addressing

Every VL53L0X powers up at address **0x29**, and the assigned address lives in
RAM (register 0x8A) — it is lost on every sensor reset. So addressing runs at
**every boot**, not once at provisioning:

1. Drive **all** XSHUT lines low, hold ~10 ms. This forces every sensor to
   reset back to 0x29 regardless of prior state.
2. Release one XSHUT at a time (set the pin to INPUT — the breakouts pull XSHUT
   up to 3.3 V, so never drive it high), wait ~10 ms for `t_boot`, `init()`,
   then `setAddress()`.

Step 1 is what makes this idempotent. An **ESP32-only reset** — watchdog, soft
reboot, serial upload — does not power-cycle the sensors, so they still hold
their assigned addresses while fresh code looks for 0x29. Without the forced
reset that presents as a dead bus.

If a sensor fails `init()`, it is **parked back in reset** before the next one
is released. Leaving it awake would strand it at 0x29, colliding with the next
sensor to come up.

## Measurement tuning

Two presets, selected by `TUNING_RESPONSIVE` in `src/main.cpp`. Phase 1 and
Phase 2 want opposite things: watching a live trace needs low latency, while
setting presence thresholds needs the quietest reading.

| | RESPONSIVE (default) | ACCURACY |
| --- | --- | --- |
| timing budget | 50 ms | 200 ms |
| average window | 6 | 6 |
| samples after trim | 4 | 4 |
| effective integration | 200 ms | 800 ms |
| group delay (lag) | ~137 ms | ~550 ms |
| stdev @ 1 m | ~2.5 mm | ~1.3 mm |
| output rate | 10 Hz | 5 Hz |

Noise scales as `1/sqrt(integration)`, so RESPONSIVE costs ~2x in noise —
irrelevant for deciding bowl present/absent, where the margin is hundreds of
mm. It still delivers 200 ms of integration, ST's high-accuracy figure.

> `AVG_WINDOW` must be read together with `TRIM`: trimming discards `2*TRIM`
> samples, so only `AVG_WINDOW - 2*TRIM` contribute. A window of 4 with
> `TRIM = 1` averages just **2** samples — half the intended integration, and a
> 2-point stdev estimate too noisy to judge anything by.

**Latency is not a UART problem.** At ~88 bytes per line and 10 Hz the payload
is a few percent of even a 115200 link. Perceived lag comes from the averaging
group delay and, more so, from `DROPOUT_MISSES` — how many consecutive
out-of-range reads are required before a removed target registers as gone.
That was originally tied to the window length, costing ~1.25 s.

Common to both presets:
- Noise is shot-noise limited, so what matters is `budget x samples`. Spending
  the time inside the sensor beats averaging short samples, since each extra
  measurement re-pays a fixed VCSEL/pre-range overhead.
- Measured stdev is **0.5–1.0 mm**, which is +/-1 LSB dither — the sensor
  reports whole millimetres, so this is the **quantization floor**. Enlarging
  the window buys nothing and only adds lag.
- The **trimmed mean** (drop high and low sample) matters here specifically:
  worn steel produces localized glints and dead spots, and a plain average
  would smear those into the result.
- `SIGNAL_RATE_LIMIT` is left at the 0.25 default. Raising it rejects
  low-confidence returns, but with ~50% of the light leaving at 23.5° only the
  diffuse component returns — too high a threshold reports a present bowl as
  "no target", which is the worse failure.

### Known limits

- Remaining error is **systematic, not random**. Per-part offset is typically
  several mm — roughly 10x the noise — and averaging cannot touch bias. The
  `OFFSET_MM[]` table is uncalibrated.
- The VL53L0X is unreliable below **~30–50 mm**, where ranging goes nonlinear.

## Status

**Phase 1 — sensing engine (current).** Bring up all four sensors across two
buses, scan round-robin, stream distances to the serial plotter for validation.

**Phase 2 — presence logic (done).** Hysteresis thresholding per level, the
contiguity rule, bowl count with fault status, runtime sensor-health detection
with backoff recovery, and the full device report (identity, firmware, MAC,
uptime, battery, charging, per-sensor health, per-level state, stack count).

**Phase 3 — telemetry (next).** POST to **Supabase**, keyed by device ID: every
**10 s** periodically, and **immediately** on change. The report cadence and
change detection already run — `device_status::sample()` builds the payload and
`device_status::differs()` answers "send now?"; only WiFi and the HTTP client
are missing. Credentials go in `include/secrets.h`, already gitignored.

**Final product — TCA9548A + dual redundancy.** See below. Hardware not yet in
hand; the current dual-bus build is the interim arrangement.

## Planned: TCA9548A and dual redundancy

The VL53L0X modules in use are Chinese clones and can fail unpredictably, so
the production build pairs **two sensors per bowl — 8 total**, filling all
eight channels of a **TCA9548A** I2C multiplexer. Suggested map:
`channel = level * 2 + replica`, giving f1 -> {0,1} ... f4 -> {6,7}.

With the mux, all eight sensors stay at 0x29, the XSHUT addressing walk
disappears, and a locked slave can be cut off by deselecting its channel — 2
pins total instead of 8.

### The mux does NOT provide optical mutual exclusion

**TCA9548A isolates I2C only.** A sensor on a deselected channel keeps ranging
autonomously, because continuous mode runs inside the sensor and not over the
bus. Two sensors aimed at the same bowl will interfere with each other's
returns regardless of which channel is selected. Mutual exclusion must be
managed explicitly through ranging state (`stopContinuous`/`startContinuous`),
never inferred from mux selection.

### Redundancy scheme: primary/standby with periodic cross-check

Two failure modes matter, and they need different defences:

- **Hard failure** — the sensor stops answering I2C. Trivial to detect.
- **Soft failure** — it answers but reports wrong distances. This silently
  corrupts bowl counts, and a single sensor cannot detect it at all.

Pure standby catches only the first; constant alternation catches both but
halves the rate. The probe scheme gets both cheaply:

```
A serves continuously (full rate, B stopped)
every ~60 s:  stop A -> start B -> sample -> compare -> restore A
  agree     -> both healthy
  disagree  -> flag degraded; neither reading is trustworthy
  A silent  -> B takes over permanently, raise fault
```

Bowls are static, so a 1-2 s probe per minute is invisible. It also proves the
standby is still alive, rather than discovering otherwise at failover.

This preserves the existing concurrency model: four bowls still range
simultaneously at the 366 mm pitch, and redundancy only changes *which*
physical sensor serves each level.

> **Mount each pair at the same height.** The 11.8 degree taper turns 20 mm of
> vertical offset into ~4 mm of distance difference — enough to read as a
> disagreement when both sensors are healthy. Give each *physical* sensor its
> own `OFFSET_MM` and cross-check calibrated values.

### Upgrade path in the code

`Reading reading(uint8_t level)` is the stable seam: bowl logic and telemetry
consume logical levels and never touch drivers, so both upgrades stay confined
to `SensorArray` internals.

1. Split logical level from physical sensor — `SENSORS[8]` plus a
   `LEVEL_SENSORS[4][2]` map. Currently 1:1; the public API does not change.
2. Add a `select(sensorIndex)` hook before each transaction group — a no-op in
   direct-bus mode, a mux channel write in mux mode.
3. Make addressing conditional — the XSHUT walk is not needed behind the mux.

## Device identity

Devices report against a unique ID, and 30+ units are planned after field
trial.

**Derive the ID from the eFuse MAC, not a compile-time `#define`.** A
compile-time ID means one binary per unit: every field reflash then depends on
knowing which build belongs to which device, and flashing the wrong one makes
two devices report the same ID — corrupting data silently instead of failing
loudly. That hazard scales with fleet size.

`ESP.getEfuseMac()` is unique per chip, needs no provisioning, and gives
**one binary for the whole fleet**:

```cpp
const char *deviceId();   // "bowlstack-a4cf12b3d9e8"
```

Map MAC -> friendly name/location in a Supabase table, where that mapping
belongs and can be edited without touching firmware. An NVS override can be
added later for human-friendly IDs, provisioned once over serial, still from a
single binary.

## Build

PlatformIO, Arduino framework. Sensor driver is `pololu/VL53L0X`.

Two environments. Serial-plotter streaming is a **test harness**, compiled out
of the production image entirely (measured: it costs 1,328 bytes, so it is
absent rather than merely disabled):

| Environment | `BOWLSTACK_DEBUG_PLOT` | Plot output |
| --- | --- | --- |
| `esp32dev-debug` | 1 | yes — use for bring-up |
| `esp32dev` | 0 | none — ship this |

```
pio run -e esp32dev-debug -t upload --upload-port COM3   # bring-up
pio run -e esp32dev       -t upload --upload-port COM3   # production
```

`default_envs` is set to `esp32dev-debug`, so a bare `pio run -t upload` builds
the plotting image. **Change that to `esp32dev` before shipping.** Boot
enumeration and error logging are not gated and remain in both images.

Serial output is formatted for the **Serial Plotter** VS Code extension
(badlogicgames.serial-plotter): lines starting with `>` carry
`name:value` pairs; everything else is ignored and shows only in the raw pane.
Open with `Serial Plotter: Open pane`, select the port at **115200**.

Baud was measured, not guessed: at ~88 bytes/line and 10 Hz the payload is ~8%
of a 115200 link, and UART contributed **7.6 ms** against a ~2 s latency
budget. 921600 was tried and reverted — it changes nothing perceptible.
Latency lives in `AVG_WINDOW` and `DROPOUT_MISSES`.

> A serial port allows only one process at a time — stop the plotter before
> uploading.
