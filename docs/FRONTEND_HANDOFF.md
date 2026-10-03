# Bowlstack — front-end handoff

**Self-contained.** Everything needed to build the UI against Supabase without
reading the firmware repo. No embedded knowledge is required.

---

## 1. What the system is

Serving stations in a kitchen each hold large steel food bowls waiting behind
the line (the **buffer**) and a vessel being served from (the **counter**).
Devices weigh both and report it. The **kitchen in-charge** watches this to see
remaining stock per item, cut waste, and get early warning of a shortage — then
tells the service counter in-charge to act.

Three kinds of device, told apart by `device_overview.kind`:

| `kind` | Ids | What it reports |
| --- | --- | --- |
| `buffer` | `BWL-001` … `BWL-032` | one 200 kg load cell under the waiting bowls: **food in grams** and **how many bowls** are on it |
| `scale` | `LDC-001` … `LDC-032` | three 20 kg cells under the counter vessel: food in grams |
| `hub` | `HUB-D`, `HUB-M`, `HUB-T` | one ESP32 per area, on mains with a small **backup battery**; it polls the area's scales and buffers and measures nothing itself. *Added by `migrate_hubs.sql`* |
| `stack` | (legacy) | the old ToF bowl counter: **0–4 bowls**, no grams |

**The battery is the hub's.** A hub's platforms — the `scale` and `buffer`
rows with the same `location` — carry no battery of their own; they carry
*node health* instead (§3). A legacy `stack` keeps its own battery.

Every BWL was a `stack` until `supabase/cutover_buffers.sql` re-kinded it. A
database that predates `migrate_loadcell.sql` has no `kind` column at all;
treat a missing `kind` as `stack`.

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
| `stack_count` | int | `stack` only: **0–4 bowls**. NULL on every other kind |
| `stack_status` | text | `stack` only: `ok` / `discontiguous` / `degraded` |
| `levels` | text[] | `stack` only: 4 entries, bottom-up: `present` / `absent` / `unknown` |
| `sensors_online` | int | `stack` only: 0–4 |
| `battery_mv` | int | `hub` and `stack` only: millivolts at the cell — raw measurement, for diagnosis. NULL on every `scale`/`buffer` |
| `battery_level` | text | `hub` and `stack` only: `good` / `medium` / `low` / `critical`, or **`null`** |
| `charging` | bool | on a `hub`, its mains: **true** on the charger, **false** on the backup battery (mains lost), **null** unknown. On a `stack`, null is "unreadable", not "no". NULL on every `scale`/`buffer` |
| `uptime_s` | int | seconds since the device booted |
| `firmware` | text | e.g. `0.2.0`, `V1.20 261003` |
| `mac` | text | which physical board is in this installation — one panel hosts LDC-001 *and* its buffers, so they share a MAC by design |
| `kind` | text | `stack` / `scale` / `buffer` / `hub` — see §1. Absent before `migrate_loadcell.sql` |
| `weight_g` | int | `scale`/`buffer`: grams of **food**. **NULL unless `weight_state = 'ok'`**; 0 is a real, empty platform |
| `weight_state` | text | `scale`/`buffer`: always sent — `ok`, `no_cells`, `cells_partial` (scale only), `over_range`, `settling`, `uncalibrated`, `untared` |
| `cells_online` | int | 0–3 for a scale, 0–1 for a buffer |
| `counts_per_gram` | numeric | calibration; ~107 for a scale, ~20.7 for a 200 kg buffer. NULL = never calibrated |
| `net_counts` | bigint | raw converter counts; NULL while there are no samples (0 is a reading) |
| `bowls` | smallint | `buffer` only: bowls on the platform, 0–4 in practice (0–8 allowed). NULL unless `ok`. *Appended by `migrate_buffer.sql`* |
| `bowls_confirmed` | bool | `buffer` only: **false = the count is remembered from before a power cycle**, not yet confirmed by a bowl moving or the platform reading empty. NULL exactly when `bowls` is |
| `gross_g` | int | `buffer` only: everything on the platform, `weight_g + bowls × 2500`. NULL unless `ok` |
| `supply_mv` | int | `scale`/`buffer`: millivolts the node's own ADC reads on its supply. *Appended by `migrate_hubs.sql`* |
| `crc_errors` | int | `scale`/`buffer`: checksum failures since the node powered up |
| `responding` | bool | `scale`/`buffer`: answered the hub's last health poll |
| `no_load_g` | int | `scale`/`buffer`: what the platform read the last time it was empty — ideally 0; anything else is drift in its zero |

**A hub is an ordinary row**: `kind = 'hub'`, `location` its area, `food_slot`
NULL by design (it serves an area, not a dish position), with `battery_*`,
`charging`, `firmware`, `uptime_s`, `mac`, `updated_at` and the server's
`offline` like any device. It has no reading and no history table.

**The four node-health columns are NULL when not measured**, and today's I2C
platforms measure only some of them (no supply ADC, no bus checksum) — RS485
nodes will report all four. Render NULL as a dash, never as 0. The trial
dashboard's thresholds: `responding = false` is a **fault**; `supply_mv` below
4500, `|no_load_g|` of 500 g or more, and 100+ `crc_errors` are **warnings**.

**A buffer's `weight_g` is always food** — the gross load less 2.5 kg of steel
per counted bowl — whatever the panel's own display switch shows. Read it as
the headline and put `bowls` and `gross_g` beside it; never subtract again.

**Before `migrate_buffer.sql` the last three columns do not exist** — the keys
are simply absent from each row, not NULL. A client that must work on both
databases treats an absent column as "not available", never as 0, and reads
history with `select('*')` rather than naming a column that may not be there
(a named missing column is a 400 that fails the whole query).

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

### `weight_state` first, the number second

A scale or buffer always sends its state and sends a number only when that
state is `ok` — the database enforces it (`(weight_state = 'ok') = (weight_g is
not null)`). So render the state, and the number only under `ok`:

| `weight_state` | Meaning | UI treatment |
| --- | --- | --- |
| `ok` | a weight | show it — **0 renders `0.0 kg`**, never a dash |
| `no_cells`, `over_range`, `cells_partial` | the reading cannot be trusted | fault (`over_range` on a buffer can also mean more bowls counted than are on it) |
| `uncalibrated`, `untared` | not set up | degraded — counts, not kilograms |
| `settling` | first readings arriving (a buffer) / power-up zero (a scale) | idle, not a fault |

A buffer has **one** cell; a scale has three. Draw the cell count per kind —
a buffer drawn with three marks reads "1 of 3", a fault it does not have.

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

**Only a `hub` or a legacy `stack` has a battery.** On a `scale` or `buffer`
the battery columns are NULL because they are not its columns — do not read
that as "no battery detected", and do not count platforms in a battery alarm.
On a hub, `charging = false` means mains is lost and the area is running on
the backup cell: worth a warning on its own, before the band drops.

On a device that has one, `null` means **no cell detected**, which is not the same as flat. The device
also rejects implausible readings — anything a lithium cell cannot produce
means the *measurement* is broken, and it reports `null` rather than a
confident value.

`battery_mv` carries the raw cell voltage if you ever need the underlying
number, e.g. to spot a wiring fault a band would disguise.

### `levels` is bottom-up

`levels[0]` is `f1`, the **bottom** bowl. A valid stack is always contiguous
from index 0. Rendering it as a vertical column is the intuitive view.

---

## 4b. Weight, and the Master Dashboard

A dish carries a **per-bowl weight** alongside its name, and
`public.slot_quantity` turns bowl counts into kilograms for the Master
Dashboard. Four rules matter more than the rest.

### Buffer and counter ADD; they are not rival estimates

A slot's food sits in two places. Bowls are **buffered** behind the line —
weighed by the BWL buffer platforms (and, while any legacy `stack` remains,
counted by it and converted to grams through the dish's per-bowl weight). Food
already **on the counter** is weighed directly by the LDC scales. They are
different food in different places, so:

```
weight_g  =  buffer_g  +  counter_g
buffer_g  =  weighed buffer food  +  stack bowls x kg/bowl   (the second term
                                       is dormant once every BWL is a buffer)
```

Each term is NULL when nothing measured it, and the sum is **NULL only when
both are** — never a 0 standing in for "unknown". After `migrate_buffer.sql`
this holds for `slot_overview.weight_g` too; before it, that column coalesced
an unknown buffer to 0, so an old client gated on `measured_weight_g` instead.

This is the correction that matters most, because the plausible alternative is
wrong in the dangerous direction. Preferring the measured figure where one
exists reads as "trust the instrument" and is what the view originally did — and
live, Darshanarthi held two buffered bowls at 18 kg beside an empty counter, so
Master reported **18.0 kg for a position holding 54.0 kg**. A scale reading zero
erased a hall's entire buffer. Under-ordering against that number is how a
service runs out.

For the same reason there is **no mismatch check** between the two. One existed;
it announced *"Darshanarthi's scale disagrees by 36.0 kg"* about two instruments
that were both perfectly correct. A genuine cross-check is a different
calculation — bowls *removed* from the buffer against grams *added* to the
counter — and needs the derivatives, not the levels.

### Grams, as an integer

`meal_food_mapping.bowl_weight_g` and `meal_menu_template.bowl_weight_g` store
**whole grams**, not kilograms. The dashboard sums products across up to five
devices and three areas per slot; doing that in binary floating point puts
visible drift on a screen that shows one decimal. Divide by 1000 exactly once,
at render. The column is bounded to 100 g … 50 000 g so a units mix-up
(kilograms typed into a grams field, or the reverse) is a 400 at the edge
rather than a dashboard reading 14 000 kg.

### A missing weight is NULL, never zero

The rule the whole schema is built on, and it bites hardest here. `0` would
render a full counter as "0.0 kg remaining" the moment someone typed a dish
name and forgot the weight — sending staff to refill something that is full.
`NULL` renders as "weight not set" and prompts. Never coalesce it.

### Grouped by slot number, across every area

`slot_quantity` has **one row per `food_slot`**, not per `(location,
food_slot)`. A slot number means the same dish position in all three halls, and
the kitchen cooks against the site-wide figure. This is the one place its
grouping differs from `slot_overview`, and the reason it is a separate view.

Its arithmetic is **sum-then-multiply**, not multiply-then-sum:

```
slot_total = SUM over areas ( bowls(area, slot) x weight(area, slot) )
```

The obvious formula — total bowls × one weight — is wrong whenever the halls
serve different dishes at the same position, which they do. If Darshanarthi
runs Curry at 4.5 kg while Tiffin runs Bhaji at 3.0 kg, there is no single
weight to multiply by. When the halls agree (the normal case) this reduces
exactly to `total_bowls × W`, so nothing is lost.

| Column | Meaning |
|---|---|
| `food_slot` | the grouping key |
| `dishes` | distinct dish names at this position, sorted. Usually one; more is not an error |
| `bowl_weight_g` | the per-bowl weight **only when every weighed area agrees**; NULL when they differ |
| `bowls_trusted` | bowls across all areas, `stack_status = 'ok'` only. NULL, not 0, when nothing reported |
| `est_weight_g` | the headline, in grams. NULL when no contributing area has a weight |
| `capacity_weight_g` | the same sum against capacity, so a bar needs no hardcoded ceiling |
| `est_is_partial` | **the total is a LOWER BOUND** — some area holds bowls that cannot be weighed. Requires all three: no weight, bowls actually present, **and a dish set at that position** |
| `areas_without_weight` | which areas, so the UI can name them instead of saying "somewhere" |
| `areas` | per-hall breakdown as jsonb: location, dish, `bowl_weight_g`, `bowls_trusted`, `bowls_capacity`, `devices`, plus **`weight_g`** — that hall's own mass, computed by the view so no screen re-derives it. NULL when the hall has no weight **or no reading**; never 0 |
| `menu_meal_type` / `menu_meal_date` | which meal the dishes and weights came from |
| `menu_is_live` | false when that meal is not the one currently running |
| `scales` / `scales_ok` | how many load cells serve this slot, and how many are reporting a usable weight |
| `measured_weight_g` | **counter only** — the sum of the scales. NULL when no scale here has a reading |
| `buffer_g` / `counter_g` | the two terms of the sum above, named so a screen can break the headline down without re-deriving it |
| `weight_g` | **the headline.** `buffer_g + counter_g`, NULL only when neither exists |
| `scale_issues` | jsonb, NULL when every scale here is healthy: which area, which device, which `weight_state`. LEFT joined one row per slot, so it cannot multiply anything |
| `buffers` / `buffers_ok` | *(migrate_buffer.sql)* buffer platforms at this slot, and how many report `ok` |
| `buffer_measured_g` | weighed buffer food alone (bigint), NULL when no buffer is `ok` |
| `buffer_bowls` | bowls on the `ok` buffers, NULL when none is `ok` |
| `buffer_unconfirmed` | true when any `ok` buffer's count is unconfirmed — render the count as `3?` |
| `weight_is_partial` | **the figure is a lower bound**: a buffer or scale here is not `ok` while the slot still has a figure, or `est_is_partial`. Never true on a NULL weight |

`areas[]` gains the same per hall: `buffers`, `buffers_ok`, `buffer_g`,
`buffer_bowls`, `buffer_unconfirmed`. A hall with buffers has no bowl
capacity — show its bowl count alone (`3`, `3?`), not `3/12`.

`slot_overview` (per hall per slot, the Stock screen's source) appends
`buffers`, `buffers_ok`, `buffer_measured_g`, `buffer_bowls`,
`buffer_unconfirmed` and `buffer_issues` (buffers not `ok`); its `buffer_g` is
the same buffer term and its `weight_g` follows the NULL rule above.

> **Lower bounds and never-installed units.** A device registered at a position
> that has *never reported* is awaiting deployment, not missing data: the LDC
> scales are all assigned and most have never spoken, so counting them as "not
> ok" would put a `≥` on every slot for good. The dashboard decides Stock's `≥`
> from the device rows — any `scale`/`buffer` that has `reported` and is not
> `ok` — and Master's from `weight_is_partial`, which must apply the same rule.

> **`scale_issues` is a separate CTE for a reason.** It began as
> `left join lateral unnest(scale_issues)` inside the grouped query, which
> duplicated each area row once per distinct bad state — devices 6→10, capacity
> 24→40, and **`bowls_trusted` 4→8**, a doubled stock figure produced by a
> *fault report*. Smoke assertion 31 is that regression.

There is deliberately **no site-wide total** — rice + dal + curry added
together is arithmetically fine and operationally meaningless. Quantity is only
meaningful per dish, so totals stop at the slot.

**Render `est_is_partial` or `weight_is_partial` as `≥`.** It is the same
notation `deviceStack()` already uses for a degraded stack's count, and reusing
it beats inventing a second vocabulary for "real but incomplete". A total that
silently drops an unweighed area reads as complete, and a kitchen under-orders
against it. Only `est_is_partial` earns a *Set weight* link — a buffer or scale
that is down is not fixed on the Menu tab.

**The per-bowl weight only matters while a `stack` exists.** A buffer weighs
its bowls, so once no `stack` device is left the dashboard hides the Menu
tab's kg/bowl field — hidden, not removed, because every save reads it back
and a missing field would write NULL over every stored `bowl_weight_g`.

**A hall with no dish at this position owes no weight.** Breakfast runs at
Darshanarthi only, so Mahatma held a bowl against no dish and the slot reported
`≥36.4 kg` with a *Set weight* link pointing at a hall with nothing to set a
weight for. `food_name is not null` is the third condition, and the `≥` was the
worse half of the bug: it claims more food than it shows, and there was none.

### Which slots appear

A slot earns a row **once at least one of its devices has ever reported**
(`having bool_or(coalesce(s.reported, false))`). That hides units that are
registered and assigned but never powered on — the backups parked at slot 5 —
so the screen carries no permanent "no data" row to learn to ignore.

The gate is deliberately *has ever reported*, not *is reporting now*. A slot
whose devices all die mid-service **keeps its row**, its last known figure and
its offline flags. Vanishing at the moment something breaks is the one
behaviour a stock screen must not have.

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

### Load cells and buffers: `weight_samples`, not `status_events`

A scale or a buffer cannot append to `status_events` — five of that table's NOT
NULL columns are bowl-shaped, so it could only do so by fabricating a bowl
count. Its history is its own table, with the same shape of contract:

```ts
const { data } = await supabase.from('weight_samples')
  .select('*')   // a buffer's bowls/gross_g exist only after migrate_buffer.sql
  .eq('device_id', 'BWL-001')
  .gte('recorded_at', since)
  .order('recorded_at', { ascending: false })
  .limit(1000)
```

Unlike `status_events` this is **not** change-only: a row lands on a 250 g move
(5 s floor), a state change, and otherwise every two minutes — and for a buffer
also on any change of `bowls` or `bowls_confirmed` — so a station that is simply
sitting there still draws a line. `weight_g` is NULL wherever `weight_state`
is not `ok`; plot those as gaps, never as zero. A buffer's rows also carry
`bowls`, `bowls_confirmed` and `gross_g` under the same NULL rules as
`device_overview`; a scale's rows leave them NULL.

> Change-detection compares against the last **enqueued** sample, not the last
> **posted** one. Comparing against the posted value while the sampler ran every
> 250 ms produced nine rows for one bowl — seq 6 through 14 inside two seconds,
> observed live. There is a 5 s floor underneath it as well.

A worked example of using these fields honestly — the trial dashboard's
reading-status band derives *silence* from a change-only log like this: a long
gap that ends in a `reason = 'boot'` row means the device was powered off for
most of it (paint nothing); the live tail is trusted only up to
`device_overview.updated_at`, since the heartbeat PATCHes it even when no event
is appended; and "offline right now" comes solely from the server's `offline`
flag, never from a client-side gap heuristic — a healthy device produces no
events for hours while perfectly alive.

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

### Master view — quantity per dish position, in kilograms

Reads `slot_quantity`. One card per slot number across all areas, itemised by
serving hall, with that slot's own total and no site-wide one. See §4b — in
particular, render `est_is_partial` / `weight_is_partial` as `≥` and never
coalesce a NULL weight to zero.

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

### Master view — consumption rate and time-to-empty

`public.slot_burn_rate`, one row per `food_slot`. This is what the load-cell
product exists for: **which dish is going fastest, and will it last the meal.**

| Column | Meaning |
|---|---|
| `total_g` / `buffer_g` / `counter_g` | the level now, same sum as `slot_quantity` |
| `g_per_hour` | consumption rate. NULL when there is not enough evidence |
| `runs_out_at` | projected empty, NULL when `g_per_hour` is NULL or ≤ 0 |
| `short_before_close` | true when `runs_out_at` precedes the end of the window — **the alarm**, and the cue to start a second production run |
| `is_partial` | the level is a lower bound, so the projection is optimistic |
| `dishes`, `consumed_g_window` | what, and how much has gone this service |

`public.slot_stock_series` is the same figure over time, for the chart — one
row per **hall** per slot per point (`location, food_slot, at_ts, buffer_g,
counter_g, total_g`), so select `location` and key the curve by it, or three
halls interleave into one zig-zag. At each point it takes every instrument's
newest sample **in any state** and counts its weight only if that sample is
`ok`, so a buffer or scale that drops out leaves a gap rather than a frozen
last weight. `public.device_burn_rate` is the per-device working, for `scale`
and `buffer` devices.

> **Rate is measured on buffer + counter together**, so carrying bowls from the
> stack onto the counter reads as *flat* rather than as a serving. Measuring
> either alone reports a phantom.
>
> **Two estimator mistakes, both found by testing rather than by reading.**
> Summing per-interval *falls* above a 250 g threshold rejected all real
> consumption: at a 2-minute cadence a station draining 4 kg/h falls 133 g per
> interval. Lowering the threshold is worse — summing only falls is *biased*,
> because noise pushes half a flat station's intervals downward and those all
> count while the matching rises are discarded. It is endpoints within
> refill-delimited segments now.
>
> And `g_per_hour / 60` is **grams** per minute. Labelling that kg/min put
> "38.60 kg/min" on the screen.

### Health view

Battery, charging, `sensors_online`, `firmware`, `offline`,
`awaiting_deployment`. For a scale or a buffer the sensor line is **weight and
cell count** (three cells, or one), not a stack of four levels — `kind` picks
which, and a buffer rendered with `f1..f4` is the usual symptom of forgetting
it. This is what lets the kitchen in-charge tell the service
counter in-charge which station needs attention — so sort by severity, not by
device ID. (The trial dashboard renders this as a symbolic roster — one glyph
line per device — with the sentences on each device's own page.)

Since `migrate_hubs.sql`: battery and charging belong on the **hub** rows
(charging / on battery / charging unknown — the trial dashboard lists the hubs
first, in a section of their own); a platform's row shows its **node health**
instead — e.g. `5.02 V · 0 CRC · responding · no-load +12 g`, leaving out the
parts it does not measure — and its device page names the hub it hangs off.
A database without the migration has no hub rows and none of the four
columns: everything else renders as before, and a platform is still never
"no battery detected".

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

A legacy stack is **0–4** bowls. A buffer holds up to 4 bowls of 14–18 kg food
each plus 2.5 kg of steel per bowl; `weight_g` spans −5 000 … 250 000 g on a
buffer, and the counter keeps a 100 kg sanity limit. A device replaced in the
field keeps its `device_id`; only `mac` changes.

**Rollout, and why the dashboard reads both shapes.** `migrate_buffer.sql`
(additive — new columns, every BWL still `stack`) → the dashboard deploy →
`cutover_buffers.sql` (every `BWL-%` re-kinded to `buffer`, its `stack_*`
NULLed in the same transaction) → the kg fleet simulator. The dashboard ships
between the two SQL steps, so it must render a pre-migration database exactly
as before and a post-cut-over one correctly; the smoke suite runs both.
