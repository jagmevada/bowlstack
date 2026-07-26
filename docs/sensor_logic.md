# Sensor logic

How a distance reading becomes a bowl count. Pure software behaviour — no
wiring, no schema, no transport. For the hardware see
[firmware.md](firmware.md); for what happens to the number see
[supabase.md](supabase.md).

---

## 1. The physical problem

Bowls are large tapered stainless steel vessels, **1.2 ft tall**, base diameter
**1.5 ft**, top diameter **2 ft**, stacked one on another with a steel cover
between them. Four ToF sensors sit on a vertical pipe at a **1.2 ft pitch**, one
at the mid-height of each bowl position, aiming horizontally at the bowl wall.

Levels are numbered bottom-upward:

| Level | Detects |
| --- | --- |
| `f1` | 1st bowl (bottom of stack) |
| `f2` | 2nd bowl |
| `f3` | 3rd bowl |
| `f4` | 4th bowl (top of stack) |

### The wall is a bad optical target

Aged stainless steel is roughly **25% diffuse / 50% specular**, the rest
absorbed. Only the diffuse component returns to the sensor; the specular half
leaves at an angle. Two consequences drive the tuning:

- `SIGNAL_RATE_LIMIT` stays at the driver default of **0.25**. Raising it
  rejects low-confidence returns — good against a cooperative target, but here
  it would report a present bowl as "no target", which is the worse failure.
- The **trimmed mean** matters more than usual: worn steel produces localized
  glints and dead spots, and a plain average smears those into the result.

### Mounting distance is constrained by the taper

The taper is `atan(0.25 / 1.2)` = **11.8° from vertical**, so the specular
component reflects a horizontal beam back **upward at 23.5°**, rising
`0.435 × D` by the time it returns to the sensor plane. With sensors pitched
366 mm apart:

- **Mount within ~300 mm of the wall** — the reflection rises ~130 mm, well
  clear of the sensor above.
- **Never mount near 841 mm** — that is exactly where one sensor's specular
  return lands on the next sensor up.

Within ~300 mm the 25° emission cones also do not overlap, so all four sensors
range **concurrently** without interfering.

---

## 2. From samples to a distance

Per sensor, in order:

1. **Range** in continuous mode at `TIMING_BUDGET_US`.
2. **Reject out-of-range.** A live sensor with nothing in view reports ~8190 mm.
   Anything at or above `OUT_OF_RANGE_MM` never enters the average — one would
   swamp a whole window of real readings.
3. **Push into a fixed window** of `AVG_WINDOW` samples.
4. **Trim and average.** Drop the `TRIM` highest and lowest, mean the rest,
   and report the sample standard deviation of what remains.
5. **Apply `OFFSET_MM[i]`**, the per-part systematic correction.

### Integration time is the accuracy knob

Ranging noise is shot-noise limited, so what matters is `budget × samples`, not
either alone. Spending the time **inside** the sensor beats averaging short
samples, because each extra measurement re-pays a fixed VCSEL/pre-range
overhead.

> **`AVG_WINDOW` must be read together with `TRIM`.** Trimming discards
> `2*TRIM` samples, so only `AVG_WINDOW - 2*TRIM` contribute. A window of 4 with
> `TRIM = 1` averages just **2** samples — half the intended integration, and a
> 2-point stdev estimate too noisy to judge anything by.

### Two tuning presets

Selected by `TUNING_RESPONSIVE` in `include/config.h`.

| | RESPONSIVE (default) | ACCURACY |
| --- | --- | --- |
| timing budget | 50 ms | 200 ms |
| average window | 6 | 6 |
| samples after trim | 4 | 4 |
| effective integration | 200 ms | 800 ms |
| group delay | ~137 ms | ~550 ms |
| stdev @ 1 m | ~2.5 mm | ~1.3 mm |
| output rate | 10 Hz | 5 Hz |

Noise scales as `1/sqrt(integration)`, so RESPONSIVE costs ~2× — irrelevant for
present/absent, where the margin is hundreds of millimetres.

### Where the accuracy floor is

Measured stdev at short range is **0.5–1.0 mm**, which is ±1 LSB dither: the
sensor reports whole millimetres, so this is the **quantization floor**.
Enlarging the window buys nothing and only adds lag.

The remaining error is **systematic, not random** — per-part offset runs several
millimetres, roughly 10× the noise, and averaging cannot touch bias. `OFFSET_MM[]`
is currently uncalibrated.

The VL53L0X is also unreliable below **~30–50 mm**, where ranging goes
nonlinear.

---

## 3. From a distance to presence

Each level is thresholded with **hysteresis** (a Schmitt trigger):

```
distance < PRESENT_BELOW_MM   ->  present
distance > ABSENT_ABOVE_MM    ->  absent
in between                    ->  hold previous state
```

A single threshold would chatter on measurement noise alone, and every flap
would be a spurious change report all the way to the server.

### Both directions are debounced, separately

This asymmetry is deliberate and was a real bug before it existed:

- **Disappearance** needs `DROPOUT_MISSES` consecutive out-of-range reads.
- **Appearance** needs `MIN_VALID_SAMPLES` in the window before a reading is
  trusted at all.

Without the second, losing a target took three misses but *gaining* one took a
single hit — a forearm or tray crossing a beam for one 50 ms measurement
registered as a bowl. It also guarantees the trim is active: below `2*TRIM+2`
samples trimming is skipped, and a lone outlier would be reported with
`stdev 0.00`, looking maximally confident.

---

## 4. From presence to a stack count

Bowls rest on each other and **cannot float**. That single physical fact does
all the work here.

| Bowls | Levels present |
| --- | --- |
| 0 | none |
| 1 | `f1` |
| 2 | `f1` + `f2` |
| 3 | `f1` + `f2` + `f3` |
| 4 | all four |

### Contiguity catches impossible readings

**Any non-contiguous pattern is a fault, not a count.** If `f2` reports a bowl
while `f1` does not, something is wrong — a failed sensor, a misaligned mount,
an obstruction — and it is reported as `DISCONTIGUOUS` rather than silently
counted.

### Contiguity also rescues dead sensors

- A level whose sensor is dead but which sits **below** an observed bowl is
  *inferred present* — physics proves a bowl is there. One dead sensor low in
  the stack costs nothing.
- An Unknown **above** an observed Absent proves nothing either, because nothing
  can rest on an empty level. The count stays exact.
- Only an Unknown **between** the top bowl and the first Absent leaves the count
  ambiguous — reported as `DEGRADED`.

This was verified exhaustively: all `3^4 = 81` level combinations were checked
against an independently derived model. An earlier version got 12 of them wrong,
reporting `DEGRADED` where the count was provably exact — including the everyday
case of an empty stack with one dead sensor, which could then never report OK
again.

### Status vocabulary

| `stack_status` | Meaning |
| --- | --- |
| `ok` | count is trustworthy |
| `discontiguous` | a present level sits above an absent one — physically impossible |
| `degraded` | an offline sensor leaves the count ambiguous |

---

## 5. Sensor health

A sensor is `Offline`, `Warming`, or `Online`. The middle state carries more
weight than it looks.

### `Warming` exists because "no measurement yet" is not "no bowl"

Warm-up ends at the first definite **conclusion** — enough samples to trust a
distance, or enough consecutive misses to confirm nothing is there — not at the
first measurement. Otherwise a station booted with bowls already stacked would
publish a confident count of zero before its first result arrived.

### Runtime failure is detected, not absorbed

When nothing acknowledges on the bus, the driver's `readReg` returns `0xFF` per
byte — which also makes the data-ready flag read true. So a dead module presents
as an endless stream of out-of-range results, **indistinguishable from "no
target"**, producing a wrong stack count reported as OK. That is the worst
failure this device can make.

Two independent detectors:

- **`0xFFFF` from the range register** is an I2C failure signature, never a
  distance — a live sensor with no target reads ~8190.
- **No completed measurement within `SENSOR_STALE_MS`** catches a sensor that
  still answers its registers but has stopped ranging.

Either demotes the sensor to `Offline`, after which recovery is retried on a
`RECOVER_RETRY_MS` backoff so a permanently dead module cannot spin the loop.

> **Not covered:** a sensor that answers normally but reports *wrong* distances.
> No single sensor can detect that — it is the entire reason for the planned
> dual-redundancy cross-check.

---

## 6. Planned: dual redundancy

The VL53L0X modules in use are clones and fail unpredictably, so production
pairs **two sensors per bowl (8 total)** behind a **TCA9548A** mux, with
`channel = level * 2 + replica`.

### The mux does NOT provide optical mutual exclusion

**TCA9548A isolates I2C only.** A sensor on a deselected channel keeps ranging
autonomously — continuous mode runs inside the sensor, not over the bus. Two
sensors aimed at the same bowl interfere regardless of channel selection.
Exclusion must be driven by `stopContinuous`/`startContinuous`, never inferred
from mux state.

### Scheme: primary/standby with a periodic cross-check

```
A serves continuously (full rate, B stopped)
every ~60 s:  stop A -> start B -> sample -> compare -> restore A
  agree     -> both healthy
  disagree  -> flag degraded; neither reading is trustworthy
  A silent  -> B takes over permanently, raise fault
```

Pure standby catches only hard failures; constant alternation catches soft ones
too but halves the rate. The probe gets both, and also proves the standby is
alive rather than discovering otherwise at failover. Bowls are static, so a
1–2 s probe per minute is invisible.

> **Mount each pair at the same height.** The 11.8° taper turns 20 mm of
> vertical offset into ~4 mm of distance difference — enough to read as
> disagreement when both sensors are healthy. Give each *physical* sensor its
> own `OFFSET_MM` and cross-check calibrated values.

### The code seam

`Reading reading(uint8_t level)` is the stable interface: bowl logic and
telemetry consume logical **levels** and never touch drivers, so both upgrades
stay inside `SensorArray`.

1. Split logical level from physical sensor — `SENSORS[8]` plus
   `LEVEL_SENSORS[4][2]`. Currently 1:1; the public API does not change.
2. Add a `select(sensorIndex)` hook before each transaction group — a no-op in
   direct-bus mode, a mux channel write in mux mode.
3. Make addressing conditional — the XSHUT walk is unnecessary behind the mux.
