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

The firmware runs as four FreeRTOS tasks, not one Arduino loop.

| Task | Core | Prio | Stack | Owns |
| --- | --- | --- | --- | --- |
| `sensor` | **1** | 3 | 4 KB | I2C, filtering, presence, stack count |
| `net` | 0 | 2 | 8 KB | WiFi join, portal, reconnection |
| `telemetry` | 0 | 2 | 12 KB | TLS, PATCH, event batches |
| `debug` | 1 | 1 | 4 KB | plotter / heartbeat |

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
tasks: stack free  sensor=2044/4096 net=6508/8192 telemetry=6648/12288 debug=1824/4096
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
| Charger STAT | 27 | has an internal pull-up, unlike 34–39 |
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

Each reading is the **mean of 16 conversions**: a single ESP32 sample carries
tens of millivolts of noise, which the steep end of the discharge curve turns
into several percent of apparent charge — enough to flap a battery band on
sampling noise alone.

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

Four coarse bands, because a percentage from a resting-voltage curve is far
less precise than its decimals suggest — load, temperature and cell age all
shift it:

| Band | SoC |
| --- | --- |
| `good` | > 70% |
| `medium` | > 35% |
| `low` | > 10% |
| `critical` | ≤ 10% |
| `unknown` | no cell detected |

`battery_pct` is reported as **`null` when no cell is detected**, never 0 — the
two are indistinguishable upstream otherwise.

The stated 3.0 V cutoff and the curve's 2.750 V floor are not in conflict: 3.0 V
lands at roughly 5% on this curve, so the cutoff keeps a small reserve rather
than running the cell to the knee.

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
