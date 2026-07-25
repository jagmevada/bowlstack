-- =====================================================================
--  Bowlstack telemetry schema
--
--  Run as the owner (postgres) in the Supabase SQL editor.
--
--  Write model: current state is UPSERTED (one row per device, no growth);
--  history is APPENDED ONLY ON CHANGE. Appending every report would be
--  30 devices x 8640 reports/day = ~259k rows/day, which exhausts the 500 MB
--  free tier in roughly ten days.
--
--  The device holds only the anon key and reaches these tables directly --
--  there is no RPC. Every privilege below is therefore the real security
--  boundary, not a formality.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 1. devices -- human-managed registry. No device ever writes here.
-- ---------------------------------------------------------------------
create table public.devices (
  device_id   text primary key
                check (device_id ~ '^[A-Za-z0-9_-]{3,32}$'),
  label       text,
  location    text,
  last_mac    text,
  created_at  timestamptz not null default now()
);

comment on table public.devices is
  'Installation registry, created by humans only. A device_id absent here makes '
  'device writes fail with SQLSTATE 23503, which is the intended provisioning '
  'gate: leave the firmware default "BWL-000" (include/version.h) permanently '
  'unregistered so a unit flashed without -DBOWLSTACK_DEVICE_ID fails loudly '
  'instead of writing into a real installation''s row.';

-- ---------------------------------------------------------------------
-- 2. device_status -- exactly one row per device, upserted
-- ---------------------------------------------------------------------
create table public.device_status (
  device_id      text primary key
                   references public.devices(device_id) on delete cascade,
  updated_at     timestamptz not null default now(),  -- trigger keeps current

  boot_id        bigint      not null,                -- esp_random() at boot
  uptime_s       integer     not null check (uptime_s >= 0),

  stack_count    smallint    not null check (stack_count >= 0),
  stack_status   text        not null
                   check (stack_status in ('ok','discontiguous','degraded')),

  -- Fixed-cardinality vectors, one entry per level, bottom-up (f1..f4, the
  -- config::SENSORS order). Arrays rather than jsonb: the shape is a
  -- compile-time constant and the vocabulary is a closed enum, so both are
  -- checkable here -- a firmware typo becomes a 400 at the edge instead of a
  -- silent data-quality problem found months later.
  levels         text[]      not null
                   check (levels <@ array['absent','present','unknown']::text[]),
  sensors_ok     boolean[]   not null
                   check (cardinality(sensors_ok) = cardinality(levels)),
  sensors_online smallint    not null
                   check (sensors_online between 0 and cardinality(levels)),

  battery_mv     integer     not null check (battery_mv between 0 and 6000),
  -- NULL, never -1. The firmware reports -1 for "no cell detected"
  -- (src/device_status.cpp); serialising that as a number would poison every
  -- average taken over this column.
  battery_pct    smallint             check (battery_pct between 0 and 100),
  charging       boolean     not null,

  firmware       text        not null,
  mac            text        not null
);

-- ~8600 updates per device per day against at most 30 live rows. device_id is
-- the only indexed column and never changes, so every update is HOT-eligible;
-- fillfactor supplies the page headroom that makes HOT actually happen, and
-- the absolute thresholds stop 30 rows sitting forever under the default 20%
-- scale factor while the table and its index bloat.
alter table public.device_status set (
  fillfactor                      = 70,
  autovacuum_vacuum_scale_factor  = 0.0,
  autovacuum_vacuum_threshold     = 200,
  autovacuum_analyze_scale_factor = 0.0,
  autovacuum_analyze_threshold    = 2000
);

-- ---------------------------------------------------------------------
-- 3. status_events -- append-only history
-- ---------------------------------------------------------------------
create table public.status_events (
  -- identity, not bigserial: serial compiles to nextval(), whose ACL is
  -- checked against the invoker, so anon would need GRANT USAGE ON SEQUENCE.
  -- An identity column is evaluated internally with the permission check
  -- skipped, and the id cannot be spoofed by the client either.
  id             bigint generated always as identity primary key,

  device_id      text        not null
                   references public.devices(device_id) on delete restrict,

  -- Idempotency. A POST that succeeds server-side but whose response is lost
  -- (routine on flaky WiFi) is retried; without this the whole buffered batch
  -- would duplicate.
  boot_id        bigint      not null,
  seq            bigint      not null check (seq >= 0),

  -- bigint, not int: the device sends (uint32_t)(millis() - eventMs), which
  -- can legitimately exceed INT32_MAX. PostgREST would reject that with a 400
  -- BEFORE the trigger could clamp it, and the device would retry the same
  -- batch forever.
  age_ms         bigint      not null check (age_ms between 0 and 604800000),

  recorded_at    timestamptz not null,  -- now() - age_ms, set by trigger
  received_at    timestamptz not null,  -- raw arrival time, set by trigger

  reason         text        not null
                   check (reason in ('boot','change','periodic')),

  stack_count    smallint    not null check (stack_count >= 0),
  stack_status   text        not null
                   check (stack_status in ('ok','discontiguous','degraded')),
  levels         text[]      not null
                   check (levels <@ array['absent','present','unknown']::text[]),
  sensors_ok     boolean[]   not null
                   check (cardinality(sensors_ok) = cardinality(levels)),
  sensors_online smallint    not null
                   check (sensors_online between 0 and cardinality(levels)),
  battery_pct    smallint             check (battery_pct between 0 and 100),
  charging       boolean     not null,
  firmware       text        not null,

  constraint status_events_once unique (device_id, boot_id, seq)
);

-- ---------------------------------------------------------------------
-- 4. Triggers
-- ---------------------------------------------------------------------

-- 4a. updated_at.
-- A column DEFAULT fires only on INSERT. The UPDATE half of
-- ON CONFLICT DO UPDATE touches only the columns named in the payload, so
-- without this trigger updated_at would freeze at the value from the device's
-- very first contact -- and updated_at is precisely the column used to decide
-- whether a device has gone offline.
create or replace function public.tg_device_status_stamp()
returns trigger
language plpgsql
set search_path = ''
as $$
begin
  -- Stale-write guard: a retried packet arriving after a newer one must not
  -- roll the row backwards. Returning NULL skips the update; PostgREST still
  -- answers 201. Delete this block if you prefer plain last-write-wins.
  if tg_op = 'UPDATE'
     and new.boot_id is not distinct from old.boot_id
     and new.uptime_s < old.uptime_s then
    return null;
  end if;

  -- Unconditional, not "if null": anon holds UPDATE on this column, so a
  -- client could otherwise pin updated_at and hide the fact it went offline.
  new.updated_at := now();
  return new;
end;
$$;

create trigger device_status_stamp
  before insert or update on public.device_status
  for each row execute function public.tg_device_status_stamp();

-- 4b. Clock-free timestamps.
-- The device has no RTC and no guaranteed NTP. It reports how long ago each
-- buffered event happened; the server supplies the absolute reference. This is
-- what lets offline events be replayed with correct times.
create or replace function public.tg_status_events_stamp()
returns trigger
language plpgsql
set search_path = ''
as $$
declare
  v_age bigint := greatest(coalesce(new.age_ms, 0), 0);
begin
  -- Clamp rather than reject. A rejected batch is retried forever by a device
  -- that cannot fix its own arithmetic; a clamped one is merely imprecise.
  -- (millis() itself wraps at 49.7 days, so a week is already generous.)
  v_age := least(v_age, 604800000);

  -- now() is transaction_timestamp, so every row of one batched POST is offset
  -- from a single reference instant and relative ordering within the batch is
  -- preserved exactly. clock_timestamp() would drift across rows.
  new.age_ms      := v_age;
  new.recorded_at := now() - (v_age * interval '1 millisecond');
  new.received_at := now();
  return new;
end;
$$;

create trigger status_events_stamp
  before insert on public.status_events
  for each row execute function public.tg_status_events_stamp();

-- ---------------------------------------------------------------------
-- 5. Indexes
--    device_status deliberately gets NONE beyond its primary key: it holds at
--    most 30 rows, which always seq-scan, and a second index would break HOT
--    under 8600 updates/device/day.
-- ---------------------------------------------------------------------
create index status_events_device_time_idx
  on public.status_events (device_id, recorded_at desc);  -- history for a device

create index status_events_recorded_at_idx
  on public.status_events (recorded_at);                  -- retention purges

-- ---------------------------------------------------------------------
-- 6. Row-level security
-- ---------------------------------------------------------------------
alter table public.devices       enable row level security;
alter table public.device_status enable row level security;
alter table public.status_events enable row level security;

-- devices: no anon policy at all, so anon is denied everything.
-- The foreign keys from the other two tables still work. Referential integrity
-- checks deliberately bypass row security, and the RI trigger runs as the
-- owner of devices with RLS not forced -- so anon needs neither a SELECT grant
-- nor a SELECT policy here. (PostgreSQL docs, "Row Security Policies".)
create policy devices_select_staff on public.devices
  for select to authenticated using (true);

-- device_status: an upsert needs BOTH an INSERT and an UPDATE policy.
-- USING must be (true): under ON CONFLICT DO UPDATE, a conflicting row that
-- fails USING raises an error rather than being silently skipped.
create policy device_status_insert_device on public.device_status
  for insert to anon with check (true);

create policy device_status_update_device on public.device_status
  for update to anon using (true) with check (true);

create policy device_status_select_staff on public.device_status
  for select to authenticated using (true);

-- status_events: insert only. Safe without a SELECT policy because the device
-- always sends Prefer: return=minimal, so nothing is RETURNed.
create policy status_events_insert_device on public.status_events
  for insert to anon with check (true);

create policy status_events_select_staff on public.status_events
  for select to authenticated using (true);

-- No DELETE or UPDATE policy for anon anywhere: RLS default-denies.

-- ---------------------------------------------------------------------
-- 7. GRANTs
--    Policies alone are NOT sufficient, and on Supabase the starting point is
--    wide open: every new table in public inherits
--      alter default privileges in schema public
--        grant all on tables to postgres, anon, authenticated, service_role;
--    so all three tables begin fully readable AND writable by anon. Revoke
--    first, then grant the minimum. Missing this step is the single easiest
--    way to think you have RLS and not have it.
-- ---------------------------------------------------------------------
revoke all on public.devices       from anon, authenticated, public;
revoke all on public.device_status from anon, authenticated, public;
revoke all on public.status_events from anon, authenticated, public;

-- Device write path.
grant insert, update on public.device_status to anon;

-- Column-level grant: anon cannot even name id / recorded_at / received_at, so
-- a forged timestamp is refused at the privilege layer before the trigger's
-- unconditional overwrite is needed. If this proves awkward during bring-up,
-- fall back to `grant insert on public.status_events to anon;` and rely on the
-- trigger alone.
grant insert (device_id, boot_id, seq, age_ms, reason,
              stack_count, stack_status, levels, sensors_ok, sensors_online,
              battery_pct, charging, firmware)
  on public.status_events to anon;

-- Human read path.
grant select on public.devices, public.device_status, public.status_events
  to authenticated;

-- No sequence grant needed: identity columns bypass the sequence ACL check.
-- With bigserial this would have been required:
--   grant usage on sequence public.status_events_id_seq to anon;

revoke all on function public.tg_device_status_stamp() from public, anon, authenticated;
revoke all on function public.tg_status_events_stamp() from public, anon, authenticated;

-- ---------------------------------------------------------------------
-- 8. Dashboard view.
--    security_invoker is mandatory: without it the view executes as its owner
--    and silently bypasses every policy above.
-- ---------------------------------------------------------------------
create view public.device_overview
with (security_invoker = true) as
select d.device_id,
       d.label,
       d.location,
       s.updated_at,
       now() - s.updated_at                              as stale_for,
       (s.updated_at is null
        or s.updated_at < now() - interval '3 minutes')  as offline,
       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_pct, s.charging,
       s.firmware, s.mac
from public.devices d
left join public.device_status s using (device_id);

revoke all on public.device_overview from anon, authenticated, public;
grant select on public.device_overview to authenticated;

commit;

-- ---------------------------------------------------------------------
-- 9. Retention (optional, run separately).
--    status_events is bounded but not zero: ~30 devices x ~50 changes/day is
--    roughly 550k rows/year, ~170 MB against a 500 MB free tier.
--    Enable pg_cron under Database > Extensions first.
-- ---------------------------------------------------------------------
-- select cron.schedule('bowlstack-retention', '17 3 * * *',
--   $$delete from public.status_events
--      where recorded_at < now() - interval '180 days'$$);
