# Bowlstack

Counts how many food bowls are stacked on a serving station, so a kitchen
in-charge can see how much food is left — reducing waste and giving early
warning when a station is running short.

---

## Documentation

| Document | Covers |
| --- | --- |
| [docs/sensor_logic.md](docs/sensor_logic.md) | how a distance reading becomes a bowl count — filtering, presence, contiguity, sensor health |
| [docs/firmware.md](docs/firmware.md) | modules, FreeRTOS tasks, wiring, network behaviour, build |
| [docs/supabase.md](docs/supabase.md) | schema, write model, security, setup |
| [docs/frontend.md](docs/frontend.md) | front-end planning and open design questions |
| [docs/FRONTEND_HANDOFF.md](docs/FRONTEND_HANDOFF.md) | **self-contained** contract for building the UI against Supabase |

---

## What it does

Each station reports, against its **device ID**, how many bowls are currently
stacked on it (0–4). Bowls are large tapered stainless steel vessels, stacked
one on another with a steel cover between them.

Everything mounts on a **6 ft vertical pipe**:

- **ESP32 + battery** at the top.
- **4× VL53L0X ToF sensors** down the pipe, spaced every **1.2 ft** — the height
  of one bowl — so each sits at the mid-height of its bowl position and aims
  horizontally at the wall.

Levels are numbered bottom-upward, `f1` through `f4`.

The count reaches Supabase over WiFi, where a front-end presents live stock per
serving area plus device health.

---

## Deployment

**32 units**, registered once as `BWL-001` … `BWL-032`. Each is deployed to a
**serving position** of two parts:

| Field | Meaning |
| --- | --- |
| `area` | `D` Darshanarthi, `T` Tiffin, `M` Mahtma |
| `item_slot` | 1–5, the physical label on the station |

A unit is installed in one area, physically labelled with one slot, and neither
changes for the life of the installation — only on failure or reassignment. Both
stay `NULL` until deployment; the front-end assigns them.

> **`item_slot` is a position, not a dish.** Which food occupies slot 3 changes
> with the meal. The slot number is the fixed physical label; the food mapping
> is front-end configuration and is deliberately not modelled in the database
> yet — see [docs/frontend.md](docs/frontend.md).

Devices are powered **only during meal service** — breakfast 06:00–09:00, lunch
11:30–14:00, dinner 18:30–21:00 — and dark the other ~16 hours. Absence of data
outside those windows is normal, and every liveness check is service-hour aware.

---

## Status

**Single-device prototype complete and verified end to end on hardware** —
sensors → bowl count → WiFi → Supabase, plus front-panel indicators and battery
monitoring.

| Phase | State |
| --- | --- |
| **1 — sensing engine** | done. Four sensors across two I2C buses, XSHUT addressing, round-robin acquisition |
| **2 — presence logic** | done. Hysteresis thresholding, contiguity rule, bowl count with fault status, runtime sensor-health detection with backoff recovery |
| **3 — telemetry** | done. WiFi with captive-portal commissioning, Supabase uplink with offline buffering |
| **4 — task fabric** | done. FreeRTOS split so measurement never stalls on the network |
| **5 — indicators & power** | done. Five status LEDs, measured Li-ion SoC curve, charger sense |
| **6 — fleet stress test** | next. Simulate 32 devices against Supabase |
| **7 — front-end** | not started. See [docs/FRONTEND_HANDOFF.md](docs/FRONTEND_HANDOFF.md) |

### Verified on hardware

| | Result |
| --- | --- |
| Sensors | 4/4 at 0x30–0x33, 10.0 Hz, stdev 1–5 mm at ~1 m |
| Bowl count | tracks blocking f1 → f1+f2 → f1..f3 → f1..f4 |
| Offline buffering | 4 events held 5 min 15 s, replayed with `recorded_at` spread across the 2.96 s in which they actually occurred — ordering preserved to 14 ms |
| Live latency | 11 ms end to end when online |
| Battery | 4102 mV against a 4.100 V multimeter reading, ±3 mV |
| Task isolation | plotter held 9.78 Hz while the net task was blocked 12 s mid-join |

**Deferred:** TCA9548A mux and dual-sensor redundancy (hardware not in hand);
per-device JWTs to replace the shared anon key.

---

## Quick start

**Server** — in the Supabase SQL editor, in order:

```
supabase/schema.sql            -- drops and rebuilds; idempotent
supabase/register_devices.sql  -- BWL-001 .. BWL-032
supabase/smoke_test.sql        -- 14 assertions; expect ALL PASS
```

> `schema.sql` **drops the `devices` registry too**, so re-running it always
> leaves the fleet unprovisioned. `register_devices.sql` is part of the same
> operation, not an optional extra — an unregistered device is refused with
> `23503` and backs off, which reads as a firmware fault until you check.

**Firmware** — PlatformIO:

```
pio run -e esp32dev-debug -t upload --upload-port COM3   # bring-up, with plotter
pio run -e esp32dev       -t upload --upload-port COM3   # production
```

Set the unit's identity in `platformio.ini`:

```ini
build_flags = -Wall -DBOWLSTACK_DEVICE_ID='"BWL-001"'
```

Credentials go in `include/secret.h` (gitignored) — copy
`include/secret.h.example`. **Never put the Supabase `service_role` key in
firmware**; it carries `BYPASSRLS` and makes every policy decorative.

---

## Hardware summary

| Signal | GPIO | Notes |
| --- | --- | --- |
| I2C0 SDA / SCL | 17 / 16 | `f1` + `f2` — **not** 16/17, verified on the board |
| I2C1 SDA / SCL | 21 / 22 | `f3` + `f4` |
| XSHUT `f1`–`f4` | 32, 33, 23, 26 | output-capable pins only |
| LEDs `f1`–`f4` | 4, 13, 14, 18 | **active low** (common anode) |
| LED health | 19 | solid ok · 1 Hz sensor fault · 2 Hz WiFi down |
| Battery ADC | 35 | 10k+10k divider, ADC1 |
| Charger sense | 27 | 10k series from 5 V, internal pull-down |

Full wiring rationale — including which pins are unusable and why — is in
[docs/firmware.md](docs/firmware.md).

## The ideas worth knowing up front

**A device is an installation, not a board.** `BOWLSTACK_DEVICE_ID` is a
compile-time constant, deliberately *not* derived from the chip's MAC. Replace a
failed ESP32, flash the same ID, and it writes to the same row with unbroken
history — the `mac` column simply updates. The default `BWL-000` is left
permanently unregistered, so a unit flashed without the build flag is rejected
loudly rather than corrupting a real installation's data.

**Bowls cannot float.** A stack is always contiguous from the bottom, and that
one physical fact both catches impossible sensor readings and lets a dead sensor
below an observed bowl be inferred rather than degrading the count. The full
treatment is in [docs/sensor_logic.md](docs/sensor_logic.md).

**Measurement never waits on the network.** Sensors own core 1; WiFi and TLS
live on core 0. This is not tuning — a single loop was previously blocked 138 s
in the captive portal and 62 s in a WiFiManager save, polling nothing
throughout. Cooperative scheduling could not fix it, because the blocking is
inside third-party libraries.

**The device never claims what it cannot measure.** No cell detected reports
`null`, not 0%. A sensor that has not concluded reports `unknown`, not "no
bowl". An impossible stack reports a fault, not a count. Each of these started
as a bug where the firmware sounded confident and was wrong — the pattern is
deliberate throughout.
