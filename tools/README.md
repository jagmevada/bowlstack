# tools

Host-side utilities. Nothing here runs on the ESP32.

---

## `fleet_sim.py` — fleet simulator

Writes plausible telemetry for all 32 devices so the front-end can be built
against a populated database instead of one prototype on a bench.

### Why it exists

A stock dashboard is only meaningful with several areas populated, a health view
only with something unhealthy in it, and a history chart only with history. One
device on a desk gives none of those.

### What it deliberately does not do

**It does not bypass the schema.** Every write goes through PostgREST with the
**anon key**, against the same policies, column grants, CHECK constraints and
triggers as a real device — the same `PATCH /device_status` and
`POST /status_events`, and nothing else.

That is the whole point. If this script can write a row, a device can; if the
schema rejects it, the fleet would have been rejected too. A test using the
`service_role` key would prove nothing, because `BYPASSRLS` makes every policy
decorative — so the script **refuses** a key that looks like one.

It also does not write to `devices`. That registry is human-managed and `anon`
correctly has no grant on it.

### Setup

```
supabase/schema.sql             -- once
supabase/register_devices.sql   -- once, BWL-001 .. BWL-032
supabase/deploy_devices.sql     -- assigns 15 of them to serving positions
```

`deploy_devices.sql` matters for front-end work: with `area` and `item_slot`
NULL, the stock view has nothing to group by. It deploys 15 and leaves 17 as
spares — see that file for why the spares are the point rather than an
oversight.

Credentials come from the environment, or from `include/secret.h` (gitignored)
as a convenience:

```
BOWLSTACK_SUPABASE_URL   e.g. https://<project>.supabase.co
BOWLSTACK_ANON_KEY
```

### Usage

```bash
python tools/fleet_sim.py --once              # one round, all 32 devices
python tools/fleet_sim.py --backfill 5        # 5 days of meal-service history
python tools/fleet_sim.py --live              # continuous, Ctrl-C to stop
python tools/fleet_sim.py --once --dry-run    # print payloads, send nothing
```

| Flag | Effect |
| --- | --- |
| `--interval N` | seconds per round in `--live` (default 60) |
| `--always` | in `--live`, report outside service hours too |
| `--devices N` | simulate fewer than 32 |
| `--seed N` | RNG seed; runs are reproducible so two screenshots are comparable |
| `--dry-run` | print payloads, send nothing |

Start with `--dry-run`, then `--once`, then `--backfill`.

### How history is placed without a clock

`status_events.recorded_at` is computed **server-side** as `now() - age_ms`,
because a device has no RTC and the schema refuses to accept a timestamp from
one. Backfill therefore sends an **age**, not a time, and the server supplies the
reference instant.

Two consequences:

- **7 days is the hard limit.** `age_ms` is clamped at 604800000 server-side, so
  nothing can be placed further back. `--backfill 9` silently becomes 7 and says
  so.
- **`received_at` is always now.** Backfilled rows were genuinely received now
  and happened earlier — which is exactly the shape of a device replaying a
  buffer after an outage, and what the front-end must already handle. Order by
  `recorded_at`, never `received_at` or `id`.

### What it simulates

- **Bowl consumption** across each meal window, with occasional restocking.
  Levels stay contiguous from `f1`, because bowls rest on each other and cannot
  float.
- **Faults**, rare and self-clearing: `degraded` (a sensor stopped reporting) and
  `discontiguous` (a bowl detected above an empty level — physically impossible,
  so the device reports a fault and *no* count). Both exist so the health view
  and the "don't trust the number" path can actually be built and seen.
- **Battery discharge** through the service day, recharged overnight — through
  the *same* measured SoC curve and the *same* hysteresis thresholds as
  `include/battery_soc.h`. Duplicated deliberately: the front-end consumes bands,
  so simulated bands are only useful if they move the way real ones do,
  hysteresis included.
- **A new `boot_id` per service**, since a device is powered on for each meal.
  This is why `seq` restarts from zero, and why the idempotency key is
  `(device_id, boot_id, seq)` rather than `seq` alone.

`serve_tick()` decides a "change" using exactly the fields
`device_status::differs()` compares, so a simulated `change` event means the same
thing a real one does.

### Reading it back

```sql
select * from public.device_overview order by area, item_slot;
```

Or from the front-end, per
[docs/FRONTEND_HANDOFF.md](../docs/FRONTEND_HANDOFF.md) — which is the
self-contained contract and does not require reading any firmware.
