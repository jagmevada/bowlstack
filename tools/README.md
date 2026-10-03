# tools

Host-side utilities. Nothing here runs on the ESP32.

---

## `fleet_sim.py` — fleet simulator (buffer platforms, kg)

Writes plausible telemetry for the **buffer** fleet so the front-end can be built
against a populated database instead of one prototype on a bench.

Every BWL-xxx unit is now a 200 kg load-cell buffer platform
(`devices.kind = 'buffer'`), not a ToF bowl stack. The simulator writes exactly
what the LDC-001 panel's buffer channel writes for a real one.

**A platform has no battery.** Power belongs to the area's **hub**, one ESP32 per
area (D/M/T) on mains with a small backup cell, and each hub has its own
`device_status` row (`HUB-D`, `HUB-M`, `HUB-T`, `devices.kind = 'hub'`). A
platform reports **node health** instead. The simulator writes two of the hubs,
`HUB-M` and `HUB-T`. `HUB-D` is the LDC-001 panel's own row.

### Why it exists

A stock dashboard is only meaningful with several areas populated, a health view
only with something unhealthy in it, and a history chart only with history. One
device on a desk gives none of those.

### Start it only AFTER `cutover_buffers.sql` AND `migrate_hubs.sql`

The rollout order is:

1. `supabase/migrate_buffer.sql`. This is additive. Every BWL row is still `stack`.
2. Push the web.
3. `supabase/cutover_buffers.sql`. This re-kinds every BWL row to `buffer`.
4. `supabase/migrate_hubs.sql`. This creates the `HUB-D/M/T` rows and the
   node-health columns.
5. **Then** start this simulator.

Before step 3, the database rejects this simulator's writes to `stack` rows. The
`weight_samples` kind guard refuses them, and so does the device_status kind
guard. Before step 1 the new columns do not exist at all. Before step 4 the hub
rows do not exist (their PATCH matches 0 rows) and `supply_mv`, `checksum_errors`,
`responding` and `no_load_g` are unknown columns, so every platform PATCH fails
too. Each of these shows up as an `ERROR` line with a pointer back here, and the
run exits non-zero.

> **Stop any older copy first.** A simulator from before the hubs still sends a
> battery on every platform. Stop it before step 4, then start this one.

The `esp32dev-fleet` image also writes BWL-002…024, as bowl stacks. It must not
be running. After the cutover its writes are rejected anyway.

### Which ids it writes, and which it must never write

| Ids | Written? | Why |
| --- | --- | --- |
| **BWL-004 … BWL-024** | **yes, the default** | the simulated buffer positions |
| **HUB-M, HUB-T** | **yes, always** | the simulated hubs for areas M and T |
| HUB-D | **never** | the LDC-001 panel is area D's hub and PATCHes it itself (`patchHub()` in `scale_telemetry.cpp`) |
| BWL-001 … BWL-003 | **never** | the LDC-001 panel's buffer bank (B1…B3) owns them. A second writer would interleave two `boot_id`s on one row |
| BWL-025 … BWL-032 | **never** | reserved, see below |
| LDC-xxx | **never** | counter scales (`kind = 'scale'`). LDC-001 is real hardware |

`--ids` can narrow the platforms, e.g. `--ids BWL-004,BWL-010`. It **refuses**
BWL-001…003, BWL-025…032 and anything that is not `BWL-NNN`. No flag overrides
the refusal. If that is ever genuinely needed, edit `PANEL_IDS` /
`RESERVED_IDS` deliberately. The hubs are the constant `HUB_IDS`, not something
`--ids` can name, and both are written whatever `--ids` says.

> **Why the reserved ids matter.** `tg_device_status_stamp()` sets
> `reported := true` on every UPDATE, unconditionally. That is deliberate, so a
> device cannot pin the flag and hide an outage. It also means a single write
> permanently retires a reserved unit from `awaiting_deployment`, the state the
> front-end must tell apart from `offline`. It is a `BEFORE UPDATE` trigger, so
> no `UPDATE` can undo it. `reset_spares.sql` repairs it by deleting and
> re-creating the rows. This has happened once already.

> **BWL-002 and BWL-003 now have no writer** unless the panel has B2/B3 fitted.
> If an earlier version of this simulator wrote to them, their rows now go
> `offline`, and that is correct.

### What it writes: the buffer contract

Two calls, the same ones the firmware makes, and nothing else:

- `PATCH /device_status?device_id=eq.BWL-xxx` every round (20 s heartbeat):
  - `weight_state`.
  - `weight_g`: **food** = gross − bowls × 2500 g, never gross.
  - `gross_g`: everything on the platform.
  - `bowls`: 0…4.
  - `bowls_confirmed`.
  - `cells_online`: 0 or 1.
  - `net_counts`.
  - `counts_per_gram`: 20.7.
  - `battery_mv`, `battery_level`, `charging`, `external_power`: always
    **explicit** `null`. Power is the hub's. A PATCH that leaves a column out
    keeps its old value, and these rows still hold the battery earlier versions
    wrote.
  - Node health:
    - `supply_mv`: the 5 V rail measured at the node. About 5000 mV, minus
      20 mV per hop along the area's daisy chain, ±10 mV.
    - `checksum_errors`: frames that failed their checksum since the node powered
      up. Mostly 0, occasionally 1. Back to 0 on every reboot.
    - `responding`: `false` exactly while the platform is in `no_cells`.
    - `no_load_g`: the platform's reading the last time it was settled,
      calibrated and empty (0 bowls), ideally about 0 g.
  - `firmware`: `sim-kg 2.0`, so simulated rows say so.
  - `mac`, `boot_id`, `uptime_s`.
  - `manual_fill_pct` / `manual_fill_age_s`: always `null`.
- `POST /weight_samples`: the same weight and bowl columns, `battery_mv` /
  `battery_level` as `null`, plus `device_id, boot_id, seq, age_ms, reason`.
  No node health, because that lives on `device_status` only. A row is appended:
  - on boot,
  - on a state change,
  - on a `bowls` or `bowls_confirmed` change,
  - on a move of ≥ 250 g since the last *enqueued* sample (5 s floor),
  - every 120 s otherwise.

  A `23505` reply means "already stored" and is not an error, as in the firmware.

**Null rules:**

- `weight_g`, `gross_g` and `bowls` are numbers **only when `weight_state = 'ok'`**.
- `bowls_confirmed` is null exactly when `bowls` is.
- `net_counts` is null when there is no reading (`no_cells`, `settling`).
- `supply_mv` is null while `responding` is false. It is measured at the node,
  so a silent node has none to give.
- `no_load_g` is null until the first empty platform since boot. It is always
  null on an uncalibrated unit, because without a calibration there are no grams.

**Never sent:**

- `stack_count`, `stack_status`, `levels`, `sensors_ok` and `sensors_online`.
- Anything to `status_events`.

### What it writes: the hub contract

`PATCH /device_status?device_id=eq.HUB-M` (and `HUB-T`) every round. It sends
exactly these fields and nothing else:

- `boot_id` (from `os.urandom`, like the platforms), `uptime_s`, `mac`.
- `firmware`: `sim-hub 1.0`.
- `battery_mv`, `battery_level`: the backup cell. About 4150 mV and `good` on
  mains. It drains while mains is away.
- `charging` and `external_power`: both `true` on mains and both `false` on the
  backup cell. On a hub `charging` means "on the charger", which is what the
  panel's `patchHub()` sends.

A hub sends no weight, bowls, gross, `stack_*` or node-health field. It never
posts `weight_samples` or `status_events`.

`check_payload()` asserts all of this, plus the ranges, on **every** payload
before it is sent or printed. That covers both contracts. A platform must carry
explicit battery nulls and valid node health. A hub must carry exactly the hub
fields. `HUB-D` is refused outright. A violation stops the run instead of
turning into a 400 halfway through a backfill.

### What it deliberately does not do

**It does not bypass the schema.** Every write goes through PostgREST with the
**anon key**, against the same policies, column grants, CHECK constraints and
triggers as a real device. If this script can write a row, a device can. If the
schema rejects it, the device would have been rejected too.

A `service_role` key would prove nothing, because `BYPASSRLS` makes every policy
decorative. The script decodes the key's JWT `role` claim (or reads the
`sb_secret_` prefix of a new-style key) and **refuses** a service_role key. The
old check searched the raw key for the text `service_role` and could never
match, because the claim is base64.

It does not write to `devices`. That registry is human-managed, and `anon`
correctly has no grant on it.

### Setup

```
supabase/register_devices.sql   -- BWL-001 .. BWL-032
supabase/assign_devices.sql     -- permanent location/food_slot assignment
supabase/seed_meal_mapping.sql  -- sample menus, so slots show dish names
supabase/migrate_buffer.sql     -- then the web push, then:
supabase/cutover_buffers.sql    -- BWL-% become kind 'buffer'
supabase/migrate_hubs.sql       -- HUB-D/M/T rows + node-health columns
supabase/reset_spares.sql       -- restores awaiting_deployment, if ever needed
```

`assign_devices.sql` matters for front-end work. With `location` and `food_slot`
NULL, the stock view has nothing to group by.

> **Several buffers share a dish position.** Darshanarthi runs three per slot, so
> stock is the **sum**. Read `slot_overview` / `slot_quantity`, not
> `device_overview`.

Credentials come from the environment, or from `include/secret.h` (gitignored)
as a convenience:

```
BOWLSTACK_SUPABASE_URL   e.g. https://<project>.supabase.co
BOWLSTACK_ANON_KEY
```

`--dry-run` reads neither and opens no connection.

### Usage

Needs `requests` for real runs (`python -m pip install requests`). The
PlatformIO Python at `~/.platformio/penv/Scripts/python.exe` already has it.

```bash
python tools/fleet_sim.py --once --dry-run   # payloads only, no network
python tools/fleet_sim.py --once             # one heartbeat, from power-up
python tools/fleet_sim.py --backfill 5       # 5 days of service history
python tools/fleet_sim.py --live             # continuous, Ctrl-C to stop
```

| Flag | Effect |
| --- | --- |
| `--interval N` | seconds per round in `--live`. The default is 20, the firmware heartbeat. At 40 or more, rows flap `offline` because `offline_after()` is 40 s, and the script warns. The old 60 s default did exactly this. |
| `--always` | in `--live`, report outside service hours too |
| `--ids A,B,…` | simulate only these BWL ids (default BWL-004…024; protected ids refused) |
| `--seed N` | RNG seed. Each id, hubs included, is the same "board" on every run: MAC, serving rate, starting stack, zero drift. `boot_id` is **not** seeded. |
| `--dry-run` | print payloads, send nothing |

`--devices` is gone. Use `--ids`.

Start with `--dry-run`, then `--once`, then `--backfill`.

**Every run is a power-up.** `boot_id` comes from `os.urandom`. When it came from
the seeded RNG, a repeated run reused old `boot_id`s: every history batch hit
`23505`, and the stale-write guard silently skipped PATCHes. So `--once` shows
every buffer just booted, with `bowls_confirmed = false`. `--live` and
`--backfill` show counts being confirmed as bowls are loaded and unloaded.

### How history is placed without a clock

`weight_samples.recorded_at` is computed **server-side** as `now() - age_ms`,
because a device has no RTC and the schema refuses to accept a timestamp from
one. Backfill therefore sends an **age**, not a time, and the server supplies the
reference instant.

- **7 days is the hard limit.** `age_ms` is clamped at 604800000 server-side.
  `--backfill 9` becomes 7 and says so. A service window that started more than
  7 days ago is skipped. Clamping it would pile the whole window onto one
  instant.
- **Windows are local time.** Earlier versions placed breakfast at 06:00 UTC.
- **`received_at` is always now.** Backfilled rows were genuinely received now
  and happened earlier. That is exactly the shape of a device replaying a buffer
  after an outage. Order by `recorded_at`, never `received_at` or `id`.

A 7-day backfill is about 42,000 rows (21 buffers: mostly the 2-minute heartbeat,
plus a row per bowl event and state change). It is posted in chunks of 500.

### What it simulates

- **Bowls.** Up to four per platform. Each is 14–18 kg of food plus the 2.5 kg
  bowl.
- **Whole bowls only.** A buffer never drains: a FULL bowl is carried to the
  counter every 6–12 minutes (each unit its own rate), a 16.5–20.5 kg step the
  real tracker counts as an unload. The serving — the gradual drain — happens at
  the counter, which this simulator does not write.
- **Restocking.** With one bowl or fewer left, 1–3 full bowls are loaded.
  Restocking is slow enough that a platform sometimes empties. An empty
  platform reads 0 g, `ok`, `bowls = 0`, and means *refill me*.
- **Noise.** ±30 g on every reading.
- **The bowl count after a reboot** is the remembered one, `bowls_confirmed = false`.
  It is confirmed again by the next load or unload, or by an empty platform. A
  cell that drops out and comes back also goes unconfirmed, because the tracker
  was blind meanwhile.
- **Bad states**, with `weight_g`, `gross_g` and `bowls` null:
  - `settling` for a few seconds at every boot.
  - Occasional self-clearing `no_cells` and `over_range`.
  - **BWL-019 permanently `uncalibrated`**, so one slot is always partial
    (`≥ X`) to build against.
- **Supply drop along the chain.** Each area's platforms are one daisy chain, in
  `assign_devices.sql` order, fed from the hub end. The far end of D's
  fourteen-node chain (BWL-022) reads about 4720 mV. M and T read about
  4900–4980 mV.
- **Zero drift.** Every cell's zero random-walks by a few grams an hour, and the
  drift is in every reading, not only in `no_load_g`. **BWL-014 has drifted to
  about +600 g**, so the dashboard's drift warning has something to fire on. An
  empty BWL-014 therefore claims about 0.6 kg of food rather than 0, which is
  the harm the warning exists to catch. It starts drifted instead of ramping up,
  because every run starts from the seed.
- **Hub power.** Mains is present almost always. Now and then a cut of 2–6
  minutes puts a hub on its backup cell (`charging = false`), which drains
  about 10 mV a minute and recharges when mains returns. That is roughly one cut
  per hub every two hours live, and at most one per hub per day in a backfill.
  The band uses the *same* SoC curve and hysteresis thresholds as
  `include/battery_soc.h`. A few-minute cut does not move it out of `good`.
- **Backfill writes no hub history.** Hubs have none. They are replayed only so
  that the final PATCH leaves them where the replay ended.

### Reading it back

```sql
select * from public.slot_overview   order by location, food_slot;  -- stock per dish
select * from public.device_overview order by location, food_slot;  -- per device
```

Or from the front-end, per
[docs/FRONTEND_HANDOFF.md](../docs/FRONTEND_HANDOFF.md). That is the
self-contained contract and does not require reading any firmware.
