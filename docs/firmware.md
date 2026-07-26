# Firmware

Codebase layout, hardware wiring, task architecture and build. For the
measurement behaviour see [sensor_logic.md](sensor_logic.md); for the server
side see [supabase.md](supabase.md).

---

## 1. Module layout

Each layer depends only on those above it. `config` depends on nothing;
`trimmed_window` touches no hardware; nothing above `sensor_array` touches a
driver.

| Module | Responsibility |
| --- | --- |
| `config` | pins and tuning constants only |
| `version` | device identity, firmware version |
| `trimmed_window` | windowed trimmed mean + stdev; pure computation |
| `sensor_array` | owns the VL53L0X drivers, exposes `Reading` values |
| `bowl_logic` | `Reading` → presence → stack count |
| `device_status` | the full report: identity, power, health, count |
| `net` | WiFi join, captive portal, reconnection |
| `telemetry` | Supabase uplink |
| `debug_plot` | serial-plotter test harness, compiled out of production |
| `tasks` | the FreeRTOS fabric running all of the above |

`Reading reading(uint8_t level)` is the stable seam. Consumers see **logical
levels**, never drivers — which is what keeps the planned mux and dual-sensor
upgrades confined to `SensorArray`.

---

## 2. Task architecture

The firmware runs as five FreeRTOS tasks plus the Arduino loop, not one loop.

| Task | Core | Prio | Stack | Owns |
| --- | --- | --- | --- | --- |
| `sensor` | **1** | 3 | 4 KB | I2C, filtering, presence, stack count |
| `net` | 0 | 2 | 8 KB | WiFi join, portal, reconnection |
| `telemetry` | 0 | 2 | 12 KB | TLS, PATCH, event batches |
| `debug` | 1 | 1 | 4 KB | plotter / heartbeat (debug build only) |
| `leds` | 1 | 1 | 2.5 KB | front-panel indicators (production) |
| Arduino `loop` | 1 | 1 | — | power line, stack headroom |

### Why this exists

Every stall this project suffered came from one loop doing measurement and
networking in turn:

| Blocking source | Duration with no sensor polled |
| --- | --- |
| captive portal (blocking) | **138 s** |
| WiFiManager save path | **62 s** |
| portal page scan | 2–10 s |
| HTTP timeout | 8 s |

Cooperative scheduling could not fix these — the blocking lives inside
third-party libraries. **Core 1 carries measurement**: the WiFi driver and the
LwIP/TLS stacks run on core 0, so a scan, a handshake or a portal save is not
merely lower priority, it is on a different processor and cannot preempt ranging
at all.

This was confirmed on hardware: a log captured the net task blocked mid-join for
12 s while the plotter kept streaming at 9.78 Hz from the other core.

> **The isolation is not absolute.** The Arduino WiFi *event* task is pinned to
> core 1 by this core's defaults at priority 19 — far above `sensorTask` — so it
> does preempt ranging. That is tolerable because it only dispatches connection
> events and returns in microseconds, unlike the driver and TLS work it hands
> off. The accurate claim is "nothing on core 0 can touch measurement", not
> "nothing WiFi-related runs on core 1".

### Concurrency rules

- **`sensorTask` owns `SensorArray` and `BowlLogic` exclusively.** No other task
  touches a driver. Keeping them single-owner is what makes the concurrency
  reviewable — making `SensorArray` thread-safe would be more code *and* harder
  to reason about.
- State is published as immutable `DeviceStatus` and `PlotFrame` **snapshots**
  under a mutex; readers get copies.
- Changes reach telemetry through a **queue**, so telemetry's ring buffer and
  HTTP client stay single-threaded and need no locking of their own.
- The queue send is **non-blocking**. Measurement must never wait on the
  network, even for queue space; a full queue logs and drops.

### Stack sizing

Sized from measured high-water marks, reported by `tasks::printStackHeadroom()`
every 60 s:

```
tasks: stack free  sensor=2988/4096 net=6508/8192 telemetry=11464/12288
                   debug=3552/4096 leds=1868/2560 bytes
```

> `uxTaskGetStackHighWaterMark` returns **bytes** on ESP-IDF, not words —
> `StackType_t` is `uint8_t`. Scaling by 4 the way vanilla FreeRTOS requires
> reported 12 KB free on a 4 KB stack, which would have hidden a task heading
> for overflow behind a comfortable number.

`telemetry` needs the most because TLS alone consumed ~5 KB on first use.
**`net`'s figure is unvalidated** — the boots measured so far joined directly,
so the captive portal (a web server *plus* a DNS server) has never run since the
task split.

Stack exhaustion on ESP32 surfaces as a corrupt-looking crash far from the
cause, which is why the margin is printed rather than assumed.

---

## 3. Hardware configuration

Two independent hardware I2C buses, two sensors each. Splitting the bus halves
the blast radius: a slave that locks one bus cannot take down all four sensors.

| Signal | GPIO | Notes |
| --- | --- | --- |
| I2C0 SDA | 17 | serves `f1` + `f2` |
| I2C0 SCL | 16 | **not** 16/17 — verified against the board |
| I2C1 SDA | 21 | serves `f3` + `f4` |
| I2C1 SCL | 22 | |
| XSHUT `f1` | 32 | |
| XSHUT `f2` | 33 | |
| XSHUT `f3` | 23 | chosen for harness length |
| XSHUT `f4` | 26 | |
| Battery ADC | 35 | ADC1 — mandatory, ADC2 is unusable with WiFi |
| Charger sense | 27 | 10 k series from the charger 5 V rail, `INPUT_PULLDOWN` |
| LED `f1` | 4 | **active low** — common anode |
| LED `f2` | 13 | |
| LED `f3` | 14 | |
| LED `f4` | 18 | |
| LED health | 19 | |

SDA/SCL are shared within a bus; XSHUT is unique per sensor. All sensors take
**VIN → 3V3** and **GND → GND**; the breakout `GPIO1` interrupt pin is unused,
since the firmware polls.

> **GPIO 34–39 cannot drive XSHUT.** They are input-only on the ESP32 — no
> output driver at all — so they cannot pull the line low. They are fine for the
> battery ADC and charger input, which only read.

### Status LEDs

Five indicators, **all common anode — the GPIO is active LOW**, so driving a pin
HIGH turns its LED *off*.

| LED | Shows |
| --- | --- |
| 1–4 | thresholded presence of `f1`–`f4` — lit when a bowl is there |
| 5 | device health |

Health patterns:

| Pattern | Meaning |
| --- | --- |
| solid on | nominal |
| **1 Hz** blink | a sensor is misbehaving — offline, or `stack_status` not `ok` |
| **2 Hz** blink | WiFi disconnected |

WiFi loss blinks faster and **takes precedence** when both are true. One LED
cannot show two things: a sensor fault still reaches the server tagged as a
fault, whereas a dropped link means nobody downstream sees anything at all.

Level LEDs follow the **debounced presence decision**, the same value the bowl
count is built from — not the raw distance. An LED flickering while the count
held steady would misrepresent what the device believes.

> **Pin choice is more constrained than it looks.** GPIO 12 is the MTDI
> strapping pin: held HIGH at boot it misconfigures flash voltage and can brick
> the module — fatal for an active-low LED, whose idle state is HIGH. GPIO 0, 2
> and 15 are boot straps that an LED pulling toward VCC would disturb; 1 and 3
> are the serial console; 6–11 are flash; 34–39 cannot drive at all. 4, 13, 14,
> 18 and 19 are clear of all of it.

> LEDs are driven **off before** their pins become outputs. Until configured a
> pin floats, and the common anode's pull to VCC lights the LED — every
> indicator would glow through boot, reading as "all bowls present, everything
> healthy" before anything had been measured.

The LEDs run in their own task (`leds`, core 1, prio 1). They are **production
behaviour**, unlike `debug_plot` which compiles to nothing in the shipping
image — the front-panel status must not vanish with the test harness.

### Battery

Sensed through a **10k + 10k divider** on GPIO35 (ADC1), so a full 4.2 V cell
reads 2.10 V at the pin.

> The ratio is set by where the ADC is *accurate*, not by what fits. At
> `ADC_11db` the nominal full scale is 3.3 V, but the datasheet's recommended
> input range is **150–2450 mV** — beyond that the converter goes non-linear
> and saturates early. Halving puts the entire useful battery span
> (2.75–4.2 V → 1.38–2.10 V) inside that window. A 5k/10k divider would put a
> *fully charged* cell at 2.80 V, i.e. error exactly where the reading is used
> to judge the battery healthy.

Equal 10k resistors also give a 5 kΩ source impedance, within the ~10 kΩ the
ADC needs, while drawing ~210 µA — about 1.7 mAh/day at 8 h service, negligible
against a 3.4 Ah cell. Larger resistors would save current but push source
impedance out of spec.

#### Sampling and filtering

Three stages, each doing something the others cannot:

| Stage | What it removes |
| --- | --- |
| **mean of 16 conversions** per read | per-conversion noise — a single ESP32 sample carries tens of millivolts |
| **read at 10 Hz** (`BATTERY_SAMPLE_INTERVAL_MS`) | — |
| **EMA, α = 0.20** across reads (~500 ms τ) | the slower wander that survives oversampling |

The 10 Hz rate is not arbitrary. `device_status::sample()` runs from the sensor
task every 2 ms, and 16 conversions at ~100 µs is ~1.6 ms of work — measuring on
every call meant **~8000 conversions/second**, consuming most of the measurement
task to track a quantity that moves over hours. At 10 Hz it is 160/s for the
same answer.

> The interval is also what makes the EMA a filter rather than decoration.
> Averaging samples taken 60 µs apart largely averages the *same* noise
> excursion; 100 ms apart they are independent. Measured residual before
> filtering was ±23 mV — about ±3% of SoC where the curve is steep — and the EMA
> cuts that roughly threefold.

Voltage is converted through a **measured discharge curve**
(`include/battery_soc.h`), not a straight line between 3.0 V and 4.2 V. Li-ion
is markedly non-linear: a real cell sits above 3.6 V for the first ~60% of its
capacity then falls away sharply, so a linear fit reads roughly 20 points
optimistic in mid-range and collapses without warning near the end.

> **The supplied table was headed "SoC(%)" but its first column is depth of
> discharge.** Row 1 is `0.0 → 4.159 V` (a full cell, nothing drawn); the last
> is `100.0 → 2.750 V` (empty). Used literally it would have reported a flat
> battery as 100% — an inversion that looks entirely plausible on a bench where
> the cell is always near full. The header stores `SoC = 100 − column`.

#### Bands, and why every threshold is doubled

Four coarse bands, because a percentage from a resting-voltage curve is far
less precise than its decimals suggest — load, temperature, cell age and
per-unit ADC calibration all shift it.

Every boundary is a **Schmitt trigger**: one threshold to enter a band from
below, a lower one to fall back out of it.

| Band | Enter rising at | Leave falling at |
| --- | --- | --- |
| `good` | ≥ 75% | < 70% |
| `medium` | ≥ 40% | < 35% |
| `low` | ≥ 15% | < 10% |
| `critical` | — | — |
| `unknown` | cell ≥ 2700 mV *and* ≤ 4300 mV | cell < 2500 mV *or* > 4400 mV |

The **falling** thresholds are the nominal edges — 10 / 35 / 70 — because those
are the numbers documented to the front-end and a battery in service is
discharging.

The **first** classification after a cell appears uses the falling edges too.
There is no history to preserve on a first look, so hysteresis has nothing to do.
Without this a device booting with a 72% cell would report `medium` and stay
there for the whole discharge, since it could never rise to the 75% entry
threshold.

> **This exists because of a measured failure.** A charging cell sat at
> 3577–3623 mV, which the curve maps to 35–41%, with the low/medium boundary at
> exactly 35. It alternated `low` / `medium` for minutes:
>
> ```
> cell 3583 mV : 35% -> low
> cell 3619 mV : 41% -> medium
> cell 3579 mV : 35% -> low
> ```
>
> The cell was fine and the ADC was working. Every alternation was a published
> state change and a Supabase write. A single-threshold classifier oscillates
> whenever its input rests on the threshold — and for a battery band that is
> most of its life, because the whole point of a band is that the cell spends a
> long time near each edge.

Filtering and hysteresis are **both** required. The filter alone only narrows
the noise; a quiet input parked exactly on a threshold still flips on the last
surviving millivolt. Hysteresis alone works but needs a band wide enough to
swallow raw noise, costing real resolution. Together each stays modest — 5
points, about 5σ of the filtered signal.

> The gap is 5 rather than 3 because the EMA output is **correlated** sample to
> sample: excursions persist rather than averaging away within a crossing.

Hysteresis is applied **after** filtering, never before. Feeding a latched value
back into a filter builds a system whose output depends on its own history in a
way that is very hard to reason about.

The band is decided once, in `battery::Monitor`, and then **carried** through
`DeviceStatus`, the event queue and both JSON payloads. Nothing downstream
re-derives it. That is load-bearing rather than tidiness: the band is the output
of a state machine, so recomputing it from a percentage would discard exactly
the history that stops it oscillating. There is deliberately no stateless
`soc → Level` function left in the codebase.

**Only the band is published.** The percentage is computed and printed locally —
it is what you calibrate against — but sending a number would invite the UI to
render precision the measurement does not have. It also carries a **2-point
deadband**, so a stationary cell shows a stationary figure. `battery_mv` goes up
alongside the band because that is a *measurement*, and an implausible value
there identifies a wiring fault the band would disguise.

Presence is bounded at **both** ends, hysteretically. Below
`BATTERY_ABSENT_BELOW_MV` the input is open; above
`BATTERY_IMPLAUSIBLE_ABOVE_MV` no lithium cell can produce it, so the
measurement is broken. Both report `unknown` rather than a number.

> The upper bound exists because of a real failure: the SoC curve clamps
> anything above its top point to 100%, so a floating pin read **6365 mV as
> "100% (good)"** — a disconnected sensor presenting as a healthy full battery.

Presence carries a **500 ms dwell** on top of those thresholds, and is decided
on the **raw** reading rather than the filtered one. Both details are
load-bearing, and each was a bug first.

**Why a dwell, not just thresholds.** Amplitude hysteresis cannot survive a cell
being inserted: during the contact bounce the reading genuinely *is* 0 mV and
4150 mV in alternate samples, so it crosses both thresholds legitimately and no
threshold pair can reject it. Only requiring the reading to stay present for a
continuous interval can. This is the same `HoldDebounce` the charger sense uses —
hysteresis in the time domain rather than the amplitude domain, which is the
right tool for anything mechanical.

**Why presence comes from the raw value.** Presence *gates* the filter, so
deciding it from the filter's own output couples the two and lets the absent
period leak into the present one. The first implementation filtered first, and
an adversarial review caught the consequence:

> With no cell fitted the EMA decays toward 0 mV, and the seed flag latched true
> forever so it never re-seeded. Fitting a full 4150 mV cell into a *running*
> unit had the EMA climb from ~0, cross the presence threshold partway up, and
> get classified on a voltage the cell never had — `critical` → `low` → `medium`
> → `good` over 1.4 s. **Four published band changes and a `critical` battery
> alert for a cell at 99%**, which the front-end renders as "charge or swap".
>
> The mirror case was worse. Recovering from the floating-pin fault, the EMA
> decayed from 6365 mV and re-entered the plausible window at 4259 mV, which the
> curve clamps to 100% — publishing a confident **FULL** battery for a
> half-charged cell.

Deciding presence on the raw sample makes the two independent: the filter never
sees a sample from the wrong regime, so it has nothing to unlearn. The filter
state is discarded whenever the cell goes absent, so the next cell is seeded from
its own first sample. Verified by simulation against the real curve — each of
those scenarios now produces exactly **one** transition, straight to the correct
band.

The stated 3.0 V cutoff and the curve's 2.750 V floor are not in conflict: 3.0 V
lands at roughly 5% on this curve, so the cutoff keeps a small reserve rather
than running the cell to the knee.

### Charger sense

GPIO27, active high, with the **internal pull-down** enabled:

```
charger 5V ──[10k]── GPIO27  (INPUT_PULLDOWN)
```

| State | Pin | Mechanism |
| --- | --- | --- |
| charging | ~3.3 V | the input clamp conducts at `(5 − 3.3) / 10k` = **170 µA** |
| idle | 0 V | internal ~45 kΩ pull-down |

The internal pull-down is appropriate *here specifically* because it never sets
the charging-state voltage — the clamp does — so its loose tolerance stays out
of the measurement. It only has to beat leakage when nothing is connected. The
same pull-down against a 4.7 kΩ series resistor would have been acting as half a
divider and sat the pin near 4.5 V, which is why the resistor value and the pull
choice are a matched pair, not independent decisions.

Read as a **majority vote over 5 samples** to guard transition edges and harness
coupling — and that is *all* the vote does. The samples are microseconds apart,
so a contact that is genuinely bouncing gives the same wrong answer to all five.

A **2 s time-domain debounce** (`CHARGING_DEBOUNCE_MS`, evaluated at the same
10 Hz) handles that: a new state must hold for the full interval before it is
accepted, and any bounce restarts the dwell. This is the time-domain equivalent
of the voltage hysteresis above, and it exists for the same reason — plugging a
charger in is a mechanical event lasting tens of milliseconds, and every bounce
during it would otherwise be a published state change.

> The first observation is **adopted**, not debounced away from a default.
> Otherwise a device that boots on charge spends 2 s claiming it is not
> charging, then reports a transition that never happened.

### Power console line

Printed every `POWER_REPORT_MS` (10 s) in **both** builds, from `loop()` rather
than `debug_plot` — it is the only power telemetry visible without a network. A
cell discharges over hours and the band has four steps, so faster is console
noise rather than information:

```
battery: pin 2080 mV -> cell 4102 mV : 95% -> good  charging=no
```

The pin voltage appears next to the cell voltage deliberately: it is what you
compare against a multimeter to derive `BOWLSTACK_BATTERY_CAL`, and an
implausible value there points at the divider rather than the battery.

### XSHUT addressing

Every VL53L0X powers up at **0x29**, and the assigned address lives in RAM
(register `0x8A`) — lost on every sensor reset. Addressing therefore runs at
**every boot**, not once at provisioning:

1. Drive **all** XSHUT lines low, hold ~10 ms. This forces every sensor back to
   0x29 regardless of prior state.
2. Release one at a time — set the pin to `INPUT`, since the breakouts pull
   XSHUT up to 3.3 V and the bare sensor's pin is 2.8 V logic. Wait ~10 ms for
   `t_boot`, `init()`, then `setAddress()`.

**Step 1 is what makes this idempotent.** An ESP32-only reset — watchdog, soft
reboot, serial upload — does not power-cycle the sensors, so they still hold
their assigned addresses while fresh code looks for 0x29. Without the forced
reset that presents as a dead bus.

If a sensor fails `init()` it is **parked back in reset** before the next is
released. Leaving it awake would strand it at 0x29, colliding with the next
sensor up.

> `recover()` rebuilds the driver object before re-initialising. The Pololu
> library caches the assigned address privately and `init()` probes through that
> cache, so a re-init after an XSHUT reset would interrogate an address nothing
> answers on — failing for exactly the sensors it exists to restore.

---

## 4. Network behaviour

Boot sequence, in `net::begin()`:

1. **Scan once.** A single radio cannot associate with several APs at
   once — each `WiFi.begin()` supersedes the last — but a scan examines every
   SSID on every channel in one operation.
2. **Join the strongest visible known network.** Joining blind cost
   `JOIN_TIMEOUT_MS` per network that was not there: 12 s burned on a 5 GHz SSID
   the radio cannot even receive. Worst case fell from ~36 s to ~5 s.
3. **Otherwise open the captive portal**, non-blocking, pumped from `net::loop()`.

Three credential slots: two compiled into `secret.h`, one commissioned through
the portal.

> The commissioned network is stored in **our own `Preferences` namespace**, not
> the WiFi stack's, and `WiFi.persistent(false)` is set. With flash persistence
> on, every `WiFi.begin(ssid, pass)` rewrites the stack's stored record — so the
> boot-time joins erased whatever the portal had saved, losing it on the first
> reboot after commissioning. `WiFi.begin()` with no arguments also does **not**
> reload NVS on ESP32; it re-applies whatever is already in RAM.

While offline:

- Link state is checked **every loop iteration** — noticing a change costs a
  variable read, so only the retry is paced (10 s).
- Retries **rotate** through all three slots; always retrying the last-used one
  would miss a site whose original network returned.
- After **30 s** with nothing reachable the portal reopens.
- **Retries continue while the portal is open.** WiFiManager puts the radio in
  AP_STA, so commissioning and recovery run in parallel; without this, a 30 s
  portal trigger would turn a router reboot into a full portal-timeout outage.

`setConnectTimeout` and `setSaveConnectTimeout` are both bounded to 10 s.
`setConfigPortalBlocking(false)` only skips the `startConfigPortal()` wait loop —
the **save** path still runs `waitForConnectResult()` with Arduino's 60 s
default, and the library's own `if(connect)` guard around that wait is commented
out, so only a bound avoids it.

---

## 5. Device identity

`BOWLSTACK_DEVICE_ID` is a compile-time `#define`, overridable per unit without
editing source:

```ini
build_flags = -Wall -DBOWLSTACK_DEVICE_ID='"BWL-001"'
```

**Identity belongs to the installation, not the chip.** If a board fails and is
swapped, the replacement must keep reporting the same ID so the server sees
continuity — which is exactly why this is *not* derived from the eFuse MAC. The
MAC is reported alongside it for hardware traceability: the ID says which
installation, the MAC says which board is currently in it. A board swap needs no
server-side change at all; the `mac` column updates itself on the first report.

The default `BWL-000` is deliberately left **unregistered** in Supabase, so a
unit flashed without the build flag is rejected with `23503` rather than writing
into a real installation's row.

---

## 6. Build

PlatformIO, Arduino framework. Sensor driver `pololu/VL53L0X`, plus
`tzapu/WiFiManager` and `bblanchon/ArduinoJson`.

Two environments. Serial-plotter streaming is a **test harness**, compiled out
of production entirely — measured at 1,328 bytes, so it is absent rather than
merely disabled:

| Environment | `BOWLSTACK_DEBUG_PLOT` | Plot output |
| --- | --- | --- |
| `esp32dev-debug` | 1 | yes — bring-up |
| `esp32dev` | 0 | none — ship this |

```
pio run -e esp32dev-debug -t upload --upload-port COM3   # bring-up
pio run -e esp32dev       -t upload --upload-port COM3   # production
```

`default_envs` is `esp32dev-debug`, so a bare `pio run -t upload` builds the
plotting image. **Change it to `esp32dev` before shipping.** Boot enumeration
and error logging are ungated and remain in both.

### Serial plotter

Output targets the **Serial Plotter** VS Code extension
(`badlogicgames.serial-plotter`): lines starting with `>` carry `name:value`
pairs; everything else is ignored and appears only in the raw pane. Open with
`Serial Plotter: Open pane`, port at **115200**.

```
>f1:1090.75,f2:989.25,f3:1001.00,f4:1022.25,f1_ok:1,...,f1_p:0,...,count:0
```

`<name>` is distance, `<name>_ok` sensor validity, `<name>_p` thresholded
presence, `count` the stack count. Plotting distance against presence is what
makes the hysteresis visible.

Baud was measured, not guessed: at ~88 bytes/line and 10 Hz the payload is ~8%
of a 115200 link, and UART contributed **7.6 ms** against a ~2 s latency budget.
921600 was tried and reverted. Latency lives in `AVG_WINDOW` and
`DROPOUT_MISSES`.

> The plotter line is built into **one buffer and written once**. With several
> tasks sharing the UART, emitting it as a dozen `printf` calls let another task
> interleave mid-line — and a broken `>` line is a corrupt sample to the
> plotter, not merely untidy output.

> A serial port allows one process at a time — stop the plotter before
> uploading.

### Secrets

`include/secret.h` holds WiFi credentials and the Supabase URL and anon key. It
is gitignored via `include/secret*.h`; `include/secret.h.example` documents the
required keys without values.

**Never put the `service_role` key in firmware.** It carries `BYPASSRLS` and
makes every policy in the schema decorative.
