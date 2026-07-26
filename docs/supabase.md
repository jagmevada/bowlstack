# Supabase backend

Schema, write model, security and setup. For what produces the numbers see
[sensor_logic.md](sensor_logic.md); for reading them from a UI see
[FRONTEND_HANDOFF.md](FRONTEND_HANDOFF.md).

---

## 1. Setup order

```
supabase/schema.sql            -- drops and rebuilds everything; idempotent
supabase/register_devices.sql  -- registers BWL-001 .. BWL-032
supabase/smoke_test.sql        -- 14 assertions; run BEFORE flashing any device
supabase/diagnose.sql          -- privilege/trigger state when something is wrong
```

> **`schema.sql` drops the `devices` registry too.** Re-running it always
> leaves the fleet empty and every device unprovisioned — `register_devices.sql`
> is not optional afterwards, it is part of the same operation. A device whose
> row is missing is refused with `23503` and backs off, which looks like a
> firmware fault until you check the registry.

`smoke_test.sql` returns one table of PASS/FAIL rows plus a verdict. Several
assertions are *supposed* to fail — a device must not be able to read your
data — so each is wrapped in an exception handler.

> Two things the Supabase web editor does that shaped these files: it displays
> **only the last statement's output**, so every script returns a single
> unioned result set; and a bare failing statement aborts the transaction, so
> expected-failure assertions must catch rather than raise.

---

## 2. Tables

### `devices` — registry

Human-managed. **No device ever writes here.** Registering a row fires a trigger
that also creates its `device_status` row, which is what lets the firmware use a
plain `UPDATE` instead of an upsert.

| Column | Notes |
| --- | --- |
| `device_id` | PK, `^[A-Za-z0-9_-]{3,32}$` — the installation's identity |
| `area` | `D` Darshanarthi, `T` Tiffin, `M` Mahtma |
| `item_slot` | 1–5, the physical label on the station |
| `label`, `location` | free text, set from the front-end |
| `timezone` | IANA zone, default `Asia/Kolkata` |
| `last_mac`, `created_at` | |

A partial unique index enforces one device per `(area, item_slot)`, partial so
unassigned spares coexist.

### `device_status` — current state, one row per device

Updated in place. Columns are nullable because the row exists before the device
has ever spoken; `reported` distinguishes "never heard from" from "reported zero
bowls", which a NULL `stack_count` alone could not.

Carries `boot_id`, `uptime_s`, `stack_count`, `stack_status`, `levels[]`,
`sensors_ok[]`, `sensors_online`, `battery_mv`, `battery_level`, `charging`,
`firmware`, `mac`, `updated_at`.

> **`battery_level` is a band, not a percentage** — `good` / `medium` / `low` /
> `critical`, or NULL for no cell. The device computes a percentage internally
> from a measured discharge curve but does not publish it: a resting-voltage
> estimate moves several points with load, temperature, cell age and per-unit
> ADC calibration, so a number would imply precision the measurement lacks.
> `battery_mv` is kept alongside because it is a *measurement* rather than an
> inference, and an implausible value there identifies a wiring fault the band
> would disguise.

### `status_events` — append-only history

One row per **real change**, never per report. `(device_id, boot_id, seq)` is
unique — that constraint *is* the idempotency mechanism.

### `service_windows` — when devices are expected to be awake

Fleet defaults plus optional per-device overrides.

---

## 3. Write model

**Current state is UPDATED in place; history is appended only on change.**

Appending every report would be ~30 devices × 8640/day ≈ **259k rows/day**,
exhausting the free tier in about ten days. The heartbeat is **60 s** and only
proves liveness, since the firmware posts on any real change.

### One telemetry round per 5 s per device

`POST_MIN_INTERVAL_MS` puts a hard floor between rounds. A round is at most two
requests — the queued history batch, then the current-state PATCH.

This throttles the **wake-up, not the data**. Every change is still enqueued the
instant it is observed, with its own `age_ms`, so `recorded_at` keeps full
resolution; changes falling inside one window are simply **clubbed into the next
round**. The whole batch goes in one POST and the state row carries the latest
values by construction, so nothing is merged away or dropped.

> The floor exists because an unstable input turns into one HTTP request per
> transition. Measured: eight `ok`/`discontiguous` flips in 45 s from a
> misaligned sensor, and separately dozens of battery-band flips per minute from
> a cell resting on a threshold.
>
> An earlier version of this limiter **did not work**. It lived inside
> `requestImmediateUpsert()` and gated only the PATCH, leaving the event POST
> completely unthrottled — so a flapping input still produced a request per
> transition, which is the exact thing it was written to prevent. There is now
> one limiter, in `telemetry::loop()`, covering both paths.

The floor does not delay the boot report: the first round after power-up is
always immediate.

### No upserts anywhere — this is deliberate

The first design had the device POST with `Prefer: resolution=merge-duplicates`,
i.e. `INSERT ... ON CONFLICT`. Every such statement was rejected for `anon` with
`42501`, while a plain `INSERT` by the same role into the same table succeeded.

Testing established that **`ON CONFLICT` requires full-table `SELECT` plus an
RLS SELECT policy** — column-level SELECT is not enough, and `DO NOTHING`
additionally fails the RLS check. Making upserts work would therefore have let
every device read every installation's telemetry, which is precisely the
property the design exists to protect.

So the device no longer upserts:

| Path | Method | Idempotency |
| --- | --- | --- |
| current state | `PATCH /device_status?device_id=eq.X` | n/a — it is an update |
| history | `POST /status_events` | unique constraint → `23505` = already recorded |

Both need strictly fewer privileges than the upsert version.

> A `PATCH` matching **zero rows is a successful 204.** An unregistered device
> would have written nothing forever while looking healthy, so the firmware
> sends `Prefer: count=exact` and treats `Content-Range` ending `/0` as
> unprovisioned.

### Clock-free timestamps

The device has **no RTC and no guaranteed NTP**. Events buffered while offline
store `millis()` when queued; at send time the device reports `age_ms`, and a
`BEFORE INSERT` trigger sets `recorded_at = now() - age_ms`. The server clock is
authoritative; the device only ever says "this happened N ms ago".

Verified in the field: four events buffered for 5 min 15 s replayed with
`recorded_at` spread across the 2.96 s in which they actually occurred,
preserving ordering to **14 ms**.

`now()` is `transaction_timestamp`, so every row of one batch shares a reference
instant and relative ordering within the batch is exact — `clock_timestamp()`
would drift across rows.

`age_ms` is `bigint`, not `int`: the device sends `(uint32_t)(millis() - eventMs)`
which can exceed `INT32_MAX`, and PostgREST would reject that with a 400
*before* the clamping trigger could run — leaving the device retrying the same
batch forever.

### `seq` and `boot_id`

`seq` increments when an event is **enqueued**, not when sent, so a dropped
entry leaves a visible gap server-side rather than vanishing. With `boot_id` it
makes retries idempotent: a POST that succeeds but whose response is lost is a
no-op on retry, not a duplicated batch.

### Wire vocabulary

`BowlLogic::wireName()` produces the canonical lowercase tokens, pinned by CHECK
constraints. It is deliberately separate from the plotter display strings —
editing those for cosmetic reasons would otherwise reject every device's traffic
at once.

---

## 4. Service hours

Devices are powered **only during meal service** — breakfast 06:00–09:00, lunch
11:30–14:00, dinner 18:30–21:00 — and dark the other ~16 hours.

Liveness therefore cannot be "has it reported recently": that would raise a
false alarm on every healthy device for two thirds of the day and bury a
genuinely dead unit among 30 of them. The device has no clock, so the logic is
entirely server-side.

`in_service_window(at, tz, device_id, margin)` evaluates wall-clock windows in
each installation's timezone. Adding any `service_windows` row for a device
replaces the fleet defaults for that device entirely — list all three meals.

---

## 5. Security

The device holds only the **anon key**. It gets:

| Table | anon may |
| --- | --- |
| `devices` | nothing at all |
| `device_status` | `SELECT(device_id)`, `UPDATE(payload columns)` |
| `status_events` | `INSERT(payload columns)` |

**No read path to any telemetry column, ever.**

> **Grants and policies are independent gates and you need both.** Supabase
> bootstraps every new public table with `ALTER DEFAULT PRIVILEGES ... GRANT ALL
> ... TO anon`, so the schema **revokes first**. Policies alone are the easy way
> to believe you have RLS and not have it.

Two subtleties that cost real debugging time:

- **A SELECT policy is required even though the device never reads.** PostgREST
  issues `PATCH ...?device_id=eq.X`, and evaluating that `WHERE` is a read
  subject to RLS. Without the policy the WHERE matched nothing and the PATCH
  silently succeeded having changed nothing. The **column grant** is what keeps
  telemetry unreadable — the policy widens rows, the grant chooses columns.
- **`devices` is invisible to the device**, which works because
  referential-integrity checks bypass row security. That doubles as the
  provisioning gate: an unregistered `device_id` fails with `23503`.

`status_events.id` is `generated always as identity`, not `bigserial` — identity
skips the sequence ACL check, so `anon` needs no `GRANT USAGE ON SEQUENCE`, and
the id cannot be client-spoofed.

### Not yet done

The anon key sits in every flash image, so anyone holding it can write for any
`device_id`. **Per-device JWTs** would close that while keeping direct table
access: mint one HS256 token per unit carrying `device_id`, and make the
policies compare it against the row.

---

## 6. Volume and retention

`device_status` takes ~8600 updates/device/day against at most 32 live rows, so
it is configured with `fillfactor = 70` and aggressive autovacuum thresholds —
`device_id` is the only indexed column and never changes, making every update
HOT-eligible if the page has headroom.

`status_events` is bounded but not zero: ~30 devices × ~50 changes/day ≈ 550k
rows/year ≈ 170 MB against a 500 MB tier. Retention via `pg_cron` is documented
at the end of `schema.sql`.

`device_status` deliberately has **no index beyond its PK** — 32 rows always
seq-scan, and a second index would break HOT under that update rate.
