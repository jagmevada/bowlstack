# tools

Host-side utilities. Nothing here runs on the ESP32.

---

## `fleet_sim.py` — fleet simulator (buffer platforms, kg)

Writes plausible telemetry for the **buffer** fleet so the front-end can be built
against a populated database instead of one prototype on a bench.

Every BWL-xxx unit is now a 200 kg load-cell buffer platform
(`devices.kind = 'buffer'`), not a ToF bowl stack. The simulator writes exactly
what the LDC-001 panel's buffer channel writes for a real one.

### Why it exists

A stock dashboard is only meaningful with several areas populated, a health view
only with something unhealthy in it, and a history chart only with history. One
device on a desk gives none of those.

### Start it only AFTER `supabase/cutover_buffers.sql`

The rollout order is:

1. `supabase/migrate_buffer.sql`. This is additive. Every BWL row is still `stack`.
2. Push the web.
3. `supabase/cutover_buffers.sql`. This re-kinds every BWL row to `buffer`.
4. **Then** start this simulator.

Before step 3, the database rejects this simulator's writes to `stack` rows. The
`weight_samples` kind guard refuses them, and so does the device_status kind
guard. Before step 1 the new columns do not exist at all. Either way it shows up
as an `ERROR` line with a pointer back here, and the run exits non-zero.

The `esp32dev-fleet` image also writes BWL-002…024, as bowl stacks. It must not
be running. After the cutover its writes are rejected anyway.

### Which ids it writes, and which it must never write

| Ids | Written? | Why |
| --- | --- | --- |
| **BWL-004 … BWL-024** | **yes, the default** | the simulated buffer positions |
| BWL-001 … BWL-003 | **never** | the LDC-001 panel's buffer bank (B1…B3) owns them. A second writer would interleave two `boot_id`s on one row |
| BWL-025 … BWL-032 | **never** | reserved, see below |
| LDC-xxx | **never** | counter scales (`kind = 'scale'`). LDC-001 is real hardware |

`--ids` can narrow the set, e.g. `--ids BWL-004,BWL-010`. It **refuses**
BWL-001…003, BWL-025…032 and anything that is not `BWL-NNN`. No flag overrides
the refusal. If that is ever genuinely needed, edit `PANEL_IDS` /
`RESERVED_IDS` deliberately.

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
  - `battery_mv`, `battery_level`, `external_power`.
  - `charging`: always `null`.
  - `firmware`: `sim-kg 2.0`, so simulated rows say so.
  - `mac`, `boot_id`, `uptime_s`.
  - `manual_fill_pct` / `manual_fill_age_s`: always `null`.
- `POST /weight_samples`: the same weight, bowl and battery columns plus
  `device_id, boot_id, seq, age_ms, reason`. A row is appended:
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

**Never sent:**

- `stack_count`, `stack_status`, `levels`, `sensors_ok` and `sensors_online`.
- Anything to `status_events`.

`check_payload()` asserts all of this, plus the ranges, on **every** payload
before it is sent or printed. A violation stops the run instead of turning into a
400 halfway through a backfill.

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
| `--seed N` | RNG seed. Each id is the same "board" on every run: MAC, serving rate, starting stack. `boot_id` is **not** seeded. |
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
- **Battery** discharge through the service day, recharged overnight. It uses
  the *same* SoC curve and hysteresis thresholds as `include/battery_soc.h`.
  About a quarter of the units are on mains (`external_power = true`) and do not
  drain.

### Reading it back

```sql
select * from public.slot_overview   order by location, food_slot;  -- stock per dish
select * from public.device_overview order by location, food_slot;  -- per device
```

Or from the front-end, per
[docs/FRONTEND_HANDOFF.md](../docs/FRONTEND_HANDOFF.md). That is the
self-contained contract and does not require reading any firmware.
