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

| Phase | State |
| --- | --- |
| **1 — sensing engine** | done. Four sensors across two I2C buses, XSHUT addressing, round-robin acquisition |
| **2 — presence logic** | done. Hysteresis thresholding, contiguity rule, bowl count with fault status, runtime sensor-health detection with backoff recovery |
| **3 — telemetry** | done. WiFi with captive-portal commissioning, Supabase uplink with offline buffering |
| **4 — task fabric** | done. FreeRTOS split so measurement never stalls on the network |
| **5 — front-end** | not started. See [docs/FRONTEND_HANDOFF.md](docs/FRONTEND_HANDOFF.md) |

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

## The two ideas worth knowing up front

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
