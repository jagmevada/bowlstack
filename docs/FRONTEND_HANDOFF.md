# Bowlstack — front-end handoff

**Self-contained.** Everything needed to build the UI against Supabase without
reading the firmware repo. No embedded knowledge is required.

---

## 1. What the system is

Serving stations in a kitchen each hold a stack of large steel food bowls. A
device on each station counts how many bowls remain and reports it. The
**kitchen in-charge** watches this to see remaining stock per item, cut waste,
and get early warning of a shortage — then tells the service counter in-charge
to act.

- **32 devices**, `BWL-001` … `BWL-032`.
- Each reports **0–4 bowls**.
- Devices are powered **only during meal service** and are dark otherwise. This
  is normal, and the UI must not present it as failure.

---

## 2. Connection

Standard Supabase client with the **anon key** and an **authenticated** session.
All UI access happens as the `authenticated` role.

```ts
import { createClient } from '@supabase/supabase-js'
const supabase = createClient(SUPABASE_URL, SUPABASE_ANON_KEY)
// sign in -> the session's `authenticated` role is what RLS grants read access to
```

> Anonymous (unauthenticated) requests can read **nothing**. That role exists
> only for the devices, which have write-only access. If a query returns empty
> where you expect rows, check the session first.

---

## 3. The one thing to read: `device_overview`

A view joining registry and live state. **Read this, not the raw tables.**

```ts
const { data } = await supabase.from('device_overview').select('*')
```

| Column | Type | Meaning |
| --- | --- | --- |
| `device_id` | text | `BWL-001` … `BWL-032`. Stable identity — survives board replacement |
| `area` | text | `D` Darshanarthi, `T` Tiffin, `M` Mahtma. `null` until deployed |
| `item_slot` | int | 1–5, physical label on the station. `null` until deployed |
| `label` | text | free text you set |
| `location` | text | free text you set |
| `timezone` | text | IANA zone, drives service-hour logic |
| `reported` | bool | has this device *ever* reported |
| `updated_at` | timestamptz | last report |
| `stale_for` | interval | `now() - updated_at` |
| `in_service` | bool | is it currently within a meal-service window |
| `offline` | bool | **should be reporting and is not** — the alarm |
| `awaiting_deployment` | bool | registered, never heard from — not a fault |
| `data_is_stale` | bool | outside service hours: numbers are last-known, not live |
| `stack_count` | int | **0–4 bowls — the primary number** |
| `stack_status` | text | `ok` / `discontiguous` / `degraded` |
| `levels` | text[] | 4 entries, bottom-up: `present` / `absent` / `unknown` |
| `sensors_online` | int | 0–4 |
| `battery_mv` | int | millivolts at the cell — raw measurement, for diagnosis |
| `battery_level` | text | `good` / `medium` / `low` / `critical`, or **`null`** |
| `charging` | bool | |
| `uptime_s` | int | seconds since the device booted |
| `firmware` | text | e.g. `0.2.0` |
| `mac` | text | which physical board is in this installation |

---

## 4. Semantics you must get right

These distinctions are the difference between a useful dashboard and one that
cries wolf.

### `offline` vs `awaiting_deployment` vs `data_is_stale`

| State | Meaning | UI treatment |
| --- | --- | --- |
| `awaiting_deployment` | registered but never installed | grey / hide from the stock view |
| `data_is_stale` | outside service hours; last known values | show, marked "as of <time>" |
| `offline` | **should be reporting and is not** | **alarm** |

`offline` is already service-hour aware — it is only true when the device ought
to be awake. **Do not compute your own staleness alarm from `updated_at`**: at
16 dark hours a day that would false-alarm on every healthy device and bury the
one that genuinely failed.

### `stack_status` — trust the count only when `ok`

| Value | Meaning | UI treatment |
| --- | --- | --- |
| `ok` | count is trustworthy | show the number |
| `degraded` | a dead sensor leaves the count ambiguous | show the number **with a warning** — it is a lower bound |
| `discontiguous` | physically impossible reading | **do not show a count** — flag a fault |

`discontiguous` means a bowl was detected *above* an empty level. Bowls rest on
each other and cannot float, so this indicates a failed sensor, a misaligned
mount, or an obstruction. Showing "2 bowls" there would be worse than showing
an error.

### Battery is a band, not a percentage

**There is no `battery_pct`, deliberately.** The device computes one internally
from a measured Li-ion discharge curve, but does not publish it: a
resting-voltage estimate moves several points with load, temperature, cell age
and per-unit ADC calibration, so a number on screen would imply a precision the
measurement does not have.

| `battery_level` | SoC | Suggested treatment |
| --- | --- | --- |
| `good` | > 70% | normal |
| `medium` | > 35% | normal |
| `low` | > 10% | warn |
| `critical` | ≤ 10% | alert — charge or swap |
| `null` | — | "no battery", **never** a flat-battery icon |

**The band is hysteretic**, so those percentages are the *falling* edges. A
discharging cell leaves `medium` below 35%, but a charging one does not re-enter
it until 40%. This is deliberate — without it a cell resting on a boundary
alternates bands indefinitely on measurement noise, and every alternation is a
row in `status_events`.

Two consequences for the UI:

- **Do not infer a percentage from the band**, in either direction. The band
  edges are not fixed points.
- A band that has not changed while `battery_mv` clearly has is **correct**, not
  a stale reading. Render the band; if you need the trend, use `battery_mv`.

`null` means **no cell detected**, which is not the same as flat. The device
also rejects implausible readings — anything a lithium cell cannot produce
means the *measurement* is broken, and it reports `null` rather than a
confident value.

`battery_mv` carries the raw cell voltage if you ever need the underlying
number, e.g. to spot a wiring fault a band would disguise.

### `levels` is bottom-up

`levels[0]` is `f1`, the **bottom** bowl. A valid stack is always contiguous
from index 0. Rendering it as a vertical column is the intuitive view.

---

## 5. Writing configuration

`authenticated` may update exactly these columns on `devices`:

```
area, item_slot, label, location, timezone
```

```ts
await supabase.from('devices')
  .update({ area: 'D', item_slot: 3, label: 'Dal counter' })
  .eq('device_id', 'BWL-001')
```

Constraints the UI should enforce before submitting:

- `area` ∈ `D` | `T` | `M`
- `item_slot` ∈ 1–5
- **`(area, item_slot)` is unique** — assigning a taken position returns a
  unique-violation. Show which device currently holds it.

> **`device_id` is not updatable, by design.** It is the installation's identity
> and the key every history row hangs off.

---

## 6. History

```ts
const { data } = await supabase.from('status_events')
  .select('recorded_at, reason, stack_count, stack_status, levels, battery_level, charging')
  .eq('device_id', 'BWL-001')
  .gte('recorded_at', since)
  .order('recorded_at', { ascending: false })
```

Rows exist **only on real change**, not per report — so consecutive rows are
genuine transitions, and the gaps between them are steady state. Good for a
step chart; wrong for assuming regular sampling.

A device sends at most **one round of writes every 5 s**. Changes occurring
inside a window are batched into the next one, each keeping its own
`recorded_at` — so several rows can share an arrival instant while describing
moments up to 5 s apart. Order by `recorded_at`, never by `id` or `received_at`.

| Column | Notes |
| --- | --- |
| `recorded_at` | when it **happened** — backdated for events buffered offline |
| `received_at` | when the server got it |
| `reason` | `boot` / `change` / `periodic` |
| `seq`, `boot_id` | per-boot sequence; **a gap in `seq` means events were dropped** |

`recorded_at` is reconstructed from a device-reported age, because the device
has no clock. It can be meaningfully earlier than `received_at` after a network
outage — that is correct, not a bug.

---

## 7. Real-time (optional)

```ts
supabase.channel('bowlstack')
  .on('postgres_changes',
      { event: 'UPDATE', schema: 'public', table: 'device_status' },
      payload => { /* refresh */ })
  .subscribe()
```

> Enable the publication only if you need it. Each device updates every 60 s, so
> 32 devices produce ~46k broadcast messages/day. Polling `device_overview`
> every 15–30 s is usually enough for a kitchen dashboard.

---

## 8. The views to build

### Stock view — the primary screen

Bowl counts grouped by **area**, so remaining stock per item is visible at a
glance across all three areas. Group by `area`, order by `item_slot`.

Slots are physical positions. **What food sits in slot 3 changes with the meal**
— breakfast, lunch and dinner rotate through Dal/Kadhi, Rice, Curry, Roti and so
on. The slot→food mapping is **front-end configuration and is not modelled in
the database yet** — it is yours to design.

### Health view

Battery, charging, `sensors_online`, `firmware`, `offline`,
`awaiting_deployment`. This is what lets the kitchen in-charge tell the service
counter in-charge which station needs attention — so sort by severity, not by
device ID.

### Configuration page

Assign `area`, `item_slot`, `label` per device. Also where the slot→food mapping
per meal will live.

---

## 9. Reference

Meal windows (fleet defaults, per-device overrides possible):

| Meal | Window |
| --- | --- |
| breakfast | 06:00–09:00 |
| lunch | 11:30–14:00 |
| dinner | 18:30–21:00 |

Evaluated in each device's `timezone` with a 10-minute margin at both ends.

A stack is **0–4** bowls. A device replaced in the field keeps its `device_id`;
only `mac` changes.
