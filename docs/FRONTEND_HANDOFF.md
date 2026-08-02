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

The shipped dashboard gets its session with `auth.signInAnonymously()` —
**Authentication → Sign In / Providers → Allow anonymous sign-ins** must stay
ON in the project. An anonymous *session* carries the `authenticated` role
(this is unrelated to the anon *key* the devices hold); there are no staff
accounts. Email/password sign-in remains a supported fallback
([../web/README.md](../web/README.md) §3).

---

## 3. The one thing to read: `device_overview`

A view joining registry and live state. **Read this, not the raw tables.**

```ts
const { data } = await supabase.from('device_overview').select('*')
```

| Column | Type | Meaning |
| --- | --- | --- |
| `device_id` | text | `BWL-001` … `BWL-032`. Stable identity — survives board replacement |
| `location` | text | `D` Darshanarthi, `M` Mahatma, `T` Tiffin, `R` reserved. `null` until deployed |
| `food_slot` | int | 1–8 dish position on the station. **Not unique** — see below. `null` for reserved |
| `current_food` | text | what this slot is serving right now, or `null` outside service hours |
| `current_meal` | text | `Breakfast` / `Lunch` / `Dinner`, or `null` |
| `label` | text | free text you set |
| `timezone` | text | IANA zone, drives service-hour logic |
| `reported` | bool | has this device *ever* reported |
| `updated_at` | timestamptz | last report |
| `stale_for` | interval | `now() - updated_at` |
| `in_service` | bool | is it currently within a meal-service window |
| `offline` | bool | **should be reporting right now and is not** — the in-window alarm |
| `missed_last_service` | bool | slept through the most recently completed service window — the alarm that survives the dark hours |
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

### `offline` vs `missed_last_service` vs `awaiting_deployment` vs `data_is_stale`

| State | Meaning | UI treatment |
| --- | --- | --- |
| `awaiting_deployment` | registered but never installed | grey / hide from the stock view |
| `data_is_stale` | outside service hours; last known values | show, marked "as of <date time>" |
| `offline` | should be reporting **right now** and is not (died mid-window) | **alarm**; clears when the window closes |
| `missed_last_service` | reported nothing during the most recently **completed** window | **alarm**, and it persists between meals |

The two alarms are complementary. `offline` is gated on the service window, so
outside meals it is false for every device — which is correct, but it made a
unit dead for six days indistinguishable from a healthy unit between meals.
`missed_last_service` closes that gap: true when the device was not alive at
the *close* of the last completed window (`updated_at` < window end −
`offline_after()`), gated on deployment (a spare parked at `R` has no service
to miss), and it stays true until the device reports again. A healthy device
is at most ~40 s stale at close, so the threshold cannot flag a normal
shutdown — but a station switched off mid-service *is* flagged, deliberately:
field use showed a unit powered off mid-lunch reading healthy all afternoon. The dashboard treats either flag as "offline": the
last value is kept on screen but rendered red, because blanking it would send
someone to a station the screen just went silent about.

Known, deliberate limit: a site holiday flags every deployed device until the
next served meal — a closure looks exactly like a fleet outage, and during a
trial arguably is one.

**Do not compute your own staleness alarm from `updated_at`**: at 16 dark hours
a day that would false-alarm on every healthy device and bury the one that
genuinely failed. Both flags above are computed server-side, service-hour
aware, in each device's own timezone.

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
location, food_slot, label, timezone
```

```ts
await supabase.from('devices')
  .update({ location: 'D', food_slot: 3 })
  .eq('device_id', 'BWL-001')
```

Constraints the UI should enforce before submitting:

- `location` ∈ `D` | `M` | `T` | `R`
- `food_slot` ∈ 1–8 (only 1–5 currently deployed)
- **`(location, food_slot)` is NOT unique, deliberately.** Several stacks serve one
  dish position — Darshanarthi slot 1 has three. Do not treat a shared position as
  a conflict.

> `label` names the physical position, never the dish. What sits in slot 3 changes
> with the meal; that lives in `meal_food_mapping`. See
> [meal_mapping.md](meal_mapping.md).

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

> Enable the publication only if you need it. Each device updates **at least
> every 20 s** and immediately on any real change, so 24 deployed units over an
> ~8 h service day produce **~35k broadcast messages/day**.

> **Realtime cannot detect `offline`.** Going offline is the *absence* of an
> update, so no `postgres_changes` event ever fires for it — the flag is computed
> from `now()` at query time. You must poll `device_overview` to see it. The trial
> dashboard polls every 15 s for exactly this reason — during service. Outside
> every meal window it idles to one poll per **10 minutes**: powered-off devices
> cannot change the rows, so a fast poll there is pure egress. A hidden tab does
> not poll at all; returning to it refreshes immediately.

**How fast is offline?** `offline` goes true once a device that *should* be
reporting has been silent for `public.offline_after()` — currently **40 s**. With
the dashboard's 15 s poll that is **40–55 s** from the device actually dying.
Retune it with one `CREATE OR REPLACE` of `offline_after()` in the SQL editor;
it must stay above the firmware heartbeat plus one retry, or healthy devices
alarm between their own posts. `missed_last_service` needs no threshold at all —
it flips when a completed window passes with no report.

---

## 8. The views to build

### Stock view — the primary screen

Remaining stock per dish, across all three areas. **Read `slot_overview`, not
`device_overview`** — several stacks serve one dish position, so the number is the
sum across them. Group by `location`, order by `food_slot`.

Slots are physical positions. **What food sits in slot 3 changes with the meal**
— breakfast, lunch and dinner rotate through Dal/Kadhi, Rice, Curry, Roti and so
on. The slot→food mapping lives in `meal_food_mapping` (per date) with a weekly
plan in `meal_menu_template` (per weekday) — see
[meal_mapping.md](meal_mapping.md) for the full contract, including why the
views never read the template directly.

### Health view

Battery, charging, `sensors_online`, `firmware`, `offline`,
`awaiting_deployment`. This is what lets the kitchen in-charge tell the service
counter in-charge which station needs attention — so sort by severity, not by
device ID.

### Configuration page

Assign `location`, `food_slot`, `label` per device. Also where the slot→food mapping
per meal will live.

---

## 9. Reference

Meal windows (fleet defaults, per-device overrides possible):

| Meal | Window |
| --- | --- |
| breakfast | 06:00–09:00 |
| lunch | 11:30–14:00 |
| dinner | 18:30–21:00 |

Evaluated in each device's `timezone`. The edges are asymmetric: a **90-second
boot grace after opening** (power-on + WiFi join + first report) and a **sharp
close**. The old ±10-minute margin alarmed the whole fleet at both edges of
every meal — before opening (window "open", devices not yet booted) and after
close (devices legitimately off, still "expected").

> **Trial state:** the live project temporarily runs debug windows (breakfast
> 06:30–09:30, lunch 11:30–14:30, dinner **16:30**–21:30). The table above is
> the real schedule, to be restored before clean trial data.

A stack is **0–4** bowls. A device replaced in the field keeps its `device_id`;
only `mac` changes.
