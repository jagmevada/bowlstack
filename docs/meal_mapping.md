# Device assignment and meal food mapping

Two things change at completely different rates, so they are stored separately.

| | Changes | Where |
| --- | --- | --- |
| **Which station a device is on** | when hardware is moved or replaced | `devices.location`, `devices.food_slot` |
| **What that station serves** | three times a day | `meal_food_mapping` |

**Devices never store food names.** A device stores a slot number. Resolution is:

```
device → location + food_slot → meal_food_mapping(location, meal_date, meal_type, food_slot) → food_name
```

> **A food slot is a dish position, not a device.** Several stacks can serve one
> slot — Darshanarthi slot 1 has three. Remaining stock for a dish is the **sum**
> across them, which is why `(location, food_slot)` is deliberately **not**
> unique and why `slot_overview` exists. Reading one device and calling it "Rice
> remaining" under-reports by 3× on exactly the busiest positions.

---

## 1. Apply order

```
supabase/schema.sql                            -- fresh installs only; drops everything
supabase/migrations/001_location_food_slot.sql -- REQUIRED, idempotent
supabase/register_devices.sql                  -- BWL-001 .. BWL-032
supabase/assign_devices.sql                    -- the permanent assignment
supabase/seed_meal_mapping.sql                 -- sample menus, for the test bed
supabase/reset_spares.sql                      -- restores awaiting_deployment
```

**On the existing database, start at the migration** — `schema.sql` drops
everything, including the 4300 backfilled events.

The new objects live only in the migration, not copied into `schema.sql`. Two
definitions of one table drift, and the copy that stays right is the one nobody
runs. The migration is idempotent, so it is correct on a fresh database and an
existing one alike.

It runs **before** registration because it is pure DDL and needs no rows — and
because `register_devices.sql` is the file most likely to be re-run later, when
hardware is added. Going second would leave it naming columns that no longer
exist.

## 2. The assignment

| Location | Slots | Devices |
| --- | --- | --- |
| `D` Darshanarthi | 1–5 | BWL-001…012 (3 per slot), BWL-021/022 on slot 5 |
| `M` Mahatma | 1–5 | BWL-013…016, BWL-023 |
| `T` Tiffin | 1–5 | BWL-017…020, BWL-024 |
| `R` Reserved | — | BWL-025…032, `food_slot` NULL |

24 deployed across **15 dish positions**, 8 reserved. `food_slot` accepts 1–8;
only 1–5 are in use, leaving headroom.

---

## 3. TypeScript

```ts
export type Location  = 'D' | 'M' | 'T' | 'R'
export type MealType  = 'Breakfast' | 'Lunch' | 'Dinner'
export type FoodSlot  = 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8
export type StackStatus = 'ok' | 'discontiguous' | 'degraded'
export type LevelState  = 'present' | 'absent' | 'unknown'
export type BatteryLevel = 'good' | 'medium' | 'low' | 'critical'

export const LOCATION_NAMES: Record<Location, string> = {
  D: 'Darshanarthi', M: 'Mahatma', T: 'Tiffin', R: 'Reserved',
}

/** public.devices — permanent assignment. Never holds a food name. */
export interface Device {
  device_id: string                 // 'BWL-001'. Identity; survives board swaps
  location: Location | null         // null until deployed
  food_slot: FoodSlot | null        // null for reserved units
  label: string | null              // names the POSITION, never the dish
  timezone: string                  // IANA, drives service-hour logic
}

/** public.meal_food_mapping — what a slot serves, per meal per day. */
export interface MealFoodMapping {
  id: number                        // surrogate handle for update/delete
  location: Location
  meal_type: MealType
  meal_date: string                 // 'YYYY-MM-DD', local service date
  food_slot: FoodSlot
  food_name: string                 // non-blank, enforced by CHECK
  created_at: string
  updated_at: string
}

/** Row of public.meal_mapping_preload(...) — see §5. */
export interface MealMappingPreloadRow {
  food_slot: FoodSlot
  food_name: string
  source_date: string
  /** false => inherited from an earlier day and NOT yet saved for this date. */
  is_saved: boolean
}

/** public.slot_overview — per-DISH stock. The primary dashboard number. */
export interface SlotOverview {
  location: Location
  food_slot: FoodSlot
  current_food: string | null       // null outside service hours or if unmapped
  current_meal: MealType | null
  devices: number                   // stacks serving this slot
  devices_reported: number
  /** devices x 4. Derive progress bars from this, never a hardcoded max. */
  bowls_capacity: number
  /**
   * Sum over devices whose stack_status is 'ok' — the trustworthy total.
   * `null` means NO device reported, which is not zero bowls. Render "no data",
   * not an empty counter: one sends someone to refill, the other to investigate.
   */
  bowls_trusted: number | null
  /** Sum over ALL reporting devices, trustworthy or not. Diagnostic only. */
  bowls_reported: number | null
  any_fault: boolean                // some device is discontiguous
  any_degraded: boolean             // some count is a lower bound
  any_battery_warn: boolean
  any_offline: boolean
  oldest_update: string | null
}

/** public.device_overview — per-device detail, for the health view. */
export interface DeviceOverview {
  device_id: string
  location: Location | null
  food_slot: FoodSlot | null
  label: string | null
  timezone: string
  current_food: string | null
  current_meal: MealType | null
  reported: boolean
  updated_at: string | null
  stale_for: string | null
  in_service: boolean
  offline: boolean                  // should be reporting and is not — the alarm
  awaiting_deployment: boolean      // registered, never heard from — not a fault
  data_is_stale: boolean            // outside service hours; last-known values
  stack_count: number | null
  stack_status: StackStatus | null
  levels: LevelState[] | null       // 4 entries, bottom-up f1..f4
  sensors_online: number | null
  battery_mv: number | null
  battery_level: BatteryLevel | null
  charging: boolean | null
  uptime_s: number | null
  firmware: string | null
  mac: string | null
}
```

---

## 4. Queries

All as the **`authenticated`** role. Anonymous requests read nothing — that role
exists only for devices, which are write-only and never see the menu.

**Stock view — the primary screen**

```ts
const { data } = await supabase
  .from('slot_overview')
  .select('*')
  .in('location', ['D', 'M', 'T'])   // exclude reserved
  .order('location').order('food_slot')
```

Show `bowls_trusted` against `bowls_capacity`. If `any_fault`, show a fault
instead of a number. If `any_degraded`, show the number with a warning — it is a
lower bound.

Worked example: Darshanarthi slot 1 has three counters (BWL-001/002/003), so a
full slot reads `bowls_trusted: 12`, `bowls_capacity: 12`, `devices: 3`. If one
counter drops off the network mid-service you get `bowls_trusted: 8`,
`devices_reported: 2` — show "8 bowls, 1 of 3 stacks not reporting" rather than a
bare 8, which would read as a shortage that isn't there.

**Health view**

```ts
const { data } = await supabase
  .from('device_overview')
  .select('*')
  .order('location', { nullsFirst: false }).order('food_slot')
```

**Read a saved mapping**

```ts
const { data } = await supabase
  .from('meal_food_mapping')
  .select('*')
  .eq('location', loc).eq('meal_type', meal).eq('meal_date', date)
  .order('food_slot')
```

**Save the admin form** — upsert on the natural key, so editing an existing meal
updates rather than colliding:

```ts
await supabase.from('meal_food_mapping').upsert(
  rows.filter(r => r.food_name.trim() !== '').map(r => ({
    location: loc, meal_type: meal, meal_date: date,
    food_slot: r.food_slot, food_name: r.food_name.trim(),
  })),
  { onConflict: 'location,meal_date,meal_type,food_slot' },
)
```

> Filter blanks client-side. A CHECK rejects an empty `food_name`, so submitting
> untouched slots fails the whole batch. **Clearing** a slot is a `delete`, not an
> empty string — "no dish here" and "a dish with no name" are different, and only
> one of them should render.

```ts
await supabase.from('meal_food_mapping').delete()
  .eq('location', loc).eq('meal_type', meal)
  .eq('meal_date', date).eq('food_slot', slot)
```

**Assign a device** (rare — hardware moves only):

```ts
await supabase.from('devices')
  .update({ location: 'D', food_slot: 3 })
  .eq('device_id', 'BWL-007')
```

`authenticated` may update only `location`, `food_slot`, `label`, `timezone`.
`device_id` is not updatable — it is the installation's identity and the key every
history row hangs off.

---

## 5. Preload (Part 4)

Opening a meal inherits the most recent previous mapping for the same location and
meal type, so the admin edits differences instead of retyping five dishes.

```ts
const { data } = await supabase.rpc('meal_mapping_preload', {
  p_location: loc, p_meal_type: meal, p_meal_date: date,
})
```

| Situation | Returns |
| --- | --- |
| a mapping is already saved for this date | it, with `is_saved: true` |
| none saved, an earlier one exists | the most recent, `is_saved: false` |
| no history at all | empty — start with a blank form |

**`is_saved` is not decoration, and the UI must act on it.** A preloaded form is
pixel-identical to a saved one. Without the flag, an admin who opens tomorrow's
Lunch, agrees with every inherited dish, and navigates away would reasonably
believe the menu was recorded — and no row would exist. Show inherited values as a
draft, mark the source date, and require an explicit save.

`source_date` is that date, for the "carried over from 24 Jul" line.

---

## 6. Deferred

**Part 3, the admin UI**, is not built — deferred until front-end development
starts. Everything it needs is above: the table, the constraints, the preload
function, the interfaces and the queries.

Two decisions to settle when it does:

**Historical attribution already works, and is worth not breaking.** Because
`meal_food_mapping` is keyed by `meal_date`, a past bowl count can be joined to the
dish that was actually in that slot at the time:

```sql
select e.recorded_at, e.device_id, m.food_name, e.stack_count
  from public.status_events e
  join public.devices d using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = (e.recorded_at at time zone d.timezone)::date
        and m.meal_type = public.current_meal_type(d.timezone, e.recorded_at)
 where e.device_id = 'BWL-001'
 order by e.recorded_at desc;
```

That answers "how much dal did we get through last Tuesday". Storing only a
current mapping would have made it unanswerable, and not retroactively fixable —
which is why the date is in the key.

**A slot's stack count can change.** If a fourth stack joins Darshanarthi slot 1,
`slot_overview.devices` rises and so does the achievable total. A UI that hardcodes
"max 12 bowls" for that slot will be wrong. Derive the ceiling from
`devices × 4`.
