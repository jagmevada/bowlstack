-- =====================================================================
--  Bowlstack telemetry schema  (v2 -- full rebuild, drops everything first)
--
--  Run as owner in the Supabase SQL editor. Idempotent: safe to re-run.
--
--  WRITE MODEL
--  -----------
--  Current state is UPDATED in place (one row per device, no growth); history
--  is APPENDED only when something actually changed. Appending every report
--  would be ~30 devices x 8640/day = 259k rows/day, exhausting the free tier
--  in about ten days. Devices are also powered only during meal service, so
--  the real volume is roughly a third of that again.
--
--  NO UPSERTS ANYWHERE -- this is deliberate
--  -----------------------------------------
--  v1 had the device POST with `Prefer: resolution=merge-duplicates`, i.e.
--  INSERT ... ON CONFLICT. Every such statement was rejected for the anon role
--  with 42501 while a plain INSERT by the same role into the same table
--  succeeded, and neither column-level nor full-table SELECT grants changed
--  it. Since the heartbeat and the event batch both used ON CONFLICT, no
--  device traffic would have reached the server at all.
--
--  Rather than keep guessing at which privilege ON CONFLICT wants, the design
--  no longer needs it:
--
--    device_status -- the row is created automatically when a device is
--                     registered (trigger below), so the device only ever
--                     issues a plain UPDATE (PostgREST PATCH).
--    status_events -- plain INSERT. Idempotency comes from the unique
--                     constraint: a retried batch raises 23505, which the
--                     firmware treats as "already recorded".
--
--  This also needs strictly fewer privileges than the upsert version.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 0. Clean slate.
-- ---------------------------------------------------------------------
drop view  if exists public.device_overview;
drop table if exists public.status_events  cascade;
drop table if exists public.device_status  cascade;
drop table if exists public.service_windows cascade;
drop table if exists public.devices        cascade;
drop function if exists public.tg_device_status_stamp()   cascade;
drop function if exists public.tg_status_events_stamp()   cascade;
drop function if exists public.tg_devices_create_status() cascade;
drop function if exists public.in_service_window(timestamptz, text, text, interval) cascade;

-- ---------------------------------------------------------------------
-- 1. devices -- human-managed registry. No device ever writes here.
-- ---------------------------------------------------------------------
create table public.devices (
  device_id   text primary key
                check (device_id ~ '^[A-Za-z0-9_-]{3,32}$'),
  label       text,
  location    text,
  timezone    text not null default 'Asia/Kolkata',
  last_mac    text,
  created_at  timestamptz not null default now()
);

comment on table public.devices is
  'Installation registry, created by humans only. Registering a device here '
  'also creates its device_status row (trigger below), which is what lets the '
  'firmware use a plain UPDATE instead of an upsert. A device_id absent here '
  'therefore has nowhere to write: leave the firmware default "BWL-000" '
  '(include/version.h) permanently unregistered so a unit flashed without '
  '-DBOWLSTACK_DEVICE_ID fails loudly instead of corrupting a real slot.';

-- ---------------------------------------------------------------------
-- 2. device_status -- exactly one row per device, UPDATED in place.
-- ---------------------------------------------------------------------
create table public.device_status (
  device_id      text primary key
                   references public.devices(device_id) on delete cascade,
  updated_at     timestamptz,          -- null until the device first reports
  reported       boolean not null default false,

  boot_id        bigint,
  uptime_s       integer     check (uptime_s >= 0),

  stack_count    smallint    check (stack_count >= 0),
  stack_status   text        check (stack_status in ('ok','discontiguous','degraded')),

  -- Fixed-cardinality vectors, one entry per level, bottom-up (f1..f4).
  -- Arrays rather than jsonb: the shape is a compile-time constant and the
  -- vocabulary is a closed enum, so both are checkable here -- a firmware typo
  -- becomes a 400 at the edge instead of a silent data-quality problem.
  levels         text[]      check (levels <@ array['absent','present','unknown']::text[]),
  sensors_ok     boolean[],
  sensors_online smallint    check (sensors_online >= 0),

  battery_mv     integer     check (battery_mv between 0 and 6000),
  -- NULL, never -1: the firmware reports -1 for "no cell detected", and
  -- serialising that as a number would poison any average over this column.
  battery_pct    smallint    check (battery_pct between 0 and 100),
  charging       boolean,

  firmware       text,
  mac            text
);

-- Columns are nullable because the row exists before the device has ever
-- spoken. `reported` distinguishes "never heard from" from "reported zero
-- bowls", which a NULL stack_count alone would not.

-- ~8600 updates per device per day against at most 30 live rows. device_id is
-- the only indexed column and never changes, so every update is HOT-eligible;
-- fillfactor supplies the page headroom that makes HOT happen, and the
-- absolute thresholds stop 30 rows sitting under the default 20% scale factor
-- while the table bloats.
alter table public.device_status set (
  fillfactor                      = 70,
  autovacuum_vacuum_scale_factor  = 0.0,
  autovacuum_vacuum_threshold     = 200,
  autovacuum_analyze_scale_factor = 0.0,
  autovacuum_analyze_threshold    = 2000
);

-- Registering a device provisions its status row. This is what removes the
-- need for an upsert on the hot path.
create or replace function public.tg_devices_create_status()
returns trigger language plpgsql security definer set search_path = '' as $$
begin
  insert into public.device_status (device_id) values (new.device_id)
  on conflict (device_id) do nothing;   -- owner context, not the device's
  return new;
end $$;

create trigger devices_create_status
  after insert on public.devices
  for each row execute function public.tg_devices_create_status();

-- ---------------------------------------------------------------------
-- 3. status_events -- append-only history.
-- ---------------------------------------------------------------------
create table public.status_events (
  -- identity, not bigserial: serial compiles to nextval(), whose ACL is
  -- checked against the invoker, so anon would need GRANT USAGE ON SEQUENCE.
  -- Identity is evaluated internally with that check skipped, and the id
  -- cannot be spoofed by the client either.
  id             bigint generated always as identity primary key,

  device_id      text        not null
                   references public.devices(device_id) on delete restrict,

  -- Idempotency without ON CONFLICT: a retried batch violates this constraint
  -- and raises 23505, which the firmware treats as "already recorded".
  boot_id        bigint      not null,
  seq            bigint      not null check (seq >= 0),

  -- bigint, not int: the device sends (uint32_t)(millis() - eventMs), which
  -- can exceed INT32_MAX. PostgREST would reject that with a 400 BEFORE the
  -- trigger could clamp it, and the device would retry the batch forever.
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
  sensors_online smallint    not null check (sensors_online >= 0),
  battery_pct    smallint    check (battery_pct between 0 and 100),
  charging       boolean     not null,
  firmware       text        not null,

  constraint status_events_once unique (device_id, boot_id, seq)
);

create index status_events_device_time_idx
  on public.status_events (device_id, recorded_at desc);
create index status_events_recorded_at_idx
  on public.status_events (recorded_at);

-- ---------------------------------------------------------------------
-- 4. Triggers
-- ---------------------------------------------------------------------

-- updated_at, and the flag marking a device as having spoken at least once.
-- A column DEFAULT would not help here: the device issues UPDATEs, and
-- defaults only apply on INSERT.
create or replace function public.tg_device_status_stamp()
returns trigger language plpgsql set search_path = '' as $$
begin
  -- Stale-write guard: a retried packet arriving after a newer one must not
  -- roll the row backwards. Returning NULL skips the update; PostgREST still
  -- answers 204. Drop this block for plain last-write-wins.
  if tg_op = 'UPDATE'
     and old.reported
     and new.boot_id is not distinct from old.boot_id
     and new.uptime_s < old.uptime_s then
    return null;
  end if;

  -- Unconditional: anon holds UPDATE on this column, so a client could
  -- otherwise pin updated_at and hide the fact it went offline.
  new.updated_at := now();
  new.reported   := true;
  return new;
end $$;

create trigger device_status_stamp
  before update on public.device_status
  for each row execute function public.tg_device_status_stamp();

-- Clock-free timestamps. The device has no RTC and no guaranteed NTP: it
-- reports how long ago each buffered event happened and the server supplies
-- the absolute reference. This is what lets offline events replay with correct
-- times.
create or replace function public.tg_status_events_stamp()
returns trigger language plpgsql set search_path = '' as $$
declare v_age bigint := greatest(coalesce(new.age_ms, 0), 0);
begin
  -- Clamp rather than reject: a rejected batch is retried forever by a device
  -- that cannot fix its own arithmetic. (millis() wraps at 49.7 days, so a
  -- week is already generous.)
  v_age := least(v_age, 604800000);

  -- now() is transaction_timestamp, so every row of one batched POST shares a
  -- reference instant and relative ordering within the batch is exact.
  -- clock_timestamp() would drift across rows.
  new.age_ms      := v_age;
  new.recorded_at := now() - (v_age * interval '1 millisecond');
  new.received_at := now();
  return new;
end $$;

create trigger status_events_stamp
  before insert on public.status_events
  for each row execute function public.tg_status_events_stamp();

-- ---------------------------------------------------------------------
-- 5. Service windows.
--    Devices run only during meal service and are dark ~16h/day. Without
--    this, a liveness check would raise a false alarm on every healthy device
--    for two thirds of the day and bury a genuinely dead one among 30 of them.
--    The device has no RTC, so this logic must live server-side.
-- ---------------------------------------------------------------------
create table public.service_windows (
  id         bigint generated always as identity primary key,
  device_id  text references public.devices(device_id) on delete cascade,
  label      text not null,
  starts_at  time not null,
  ends_at    time not null,
  constraint service_window_order check (ends_at > starts_at)
);

create index service_windows_device_idx on public.service_windows (device_id);

-- Fleet defaults. A row naming a device overrides these for that device.
insert into public.service_windows (device_id, label, starts_at, ends_at)
values (null, 'breakfast', time '06:00', time '09:00'),
       (null, 'lunch',     time '11:30', time '14:00'),
       (null, 'dinner',    time '18:30', time '21:00');

create or replace function public.in_service_window(
  at_ts  timestamptz,
  tz     text,
  dev_id text     default null,
  margin interval default interval '10 minutes'
) returns boolean language sql stable set search_path = '' as $$
  with local_now as (
    select (at_ts at time zone coalesce(tz, 'UTC'))::time as t
  ),
  applicable as (
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id = dev_id
    union all
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id is null
        and not exists (select 1 from public.service_windows x
                         where x.device_id = dev_id)
  )
  select coalesce(bool_or(
           (select t from local_now) >= (a.starts_at - margin)
       and (select t from local_now) <= (a.ends_at   + margin)), false)
    from applicable a;
$$;

-- ---------------------------------------------------------------------
-- 6. Row-level security
-- ---------------------------------------------------------------------
alter table public.devices         enable row level security;
alter table public.device_status   enable row level security;
alter table public.status_events   enable row level security;
alter table public.service_windows enable row level security;

-- devices: no anon policy at all, so anon is denied everything. The foreign
-- keys from the other tables still work -- referential integrity checks
-- deliberately bypass row security and run as the table owner.
create policy devices_select_staff on public.devices
  for select to authenticated using (true);

-- device_status: the device UPDATEs only -- there is no INSERT policy because
-- it never inserts, the row having been created when the device was registered.
create policy device_status_update_device on public.device_status
  for update to anon using (true) with check (true);

-- A SELECT policy is REQUIRED even though the device never reads anything.
-- PostgREST issues PATCH ...?device_id=eq.X, i.e. UPDATE ... WHERE device_id =
-- 'X', and evaluating that WHERE is a read subject to RLS. Without this policy
-- the WHERE matches no rows and the PATCH silently succeeds having changed
-- nothing -- a zero-row UPDATE is not an error, so the device would look
-- healthy while writing nothing forever.
--
-- This does NOT make telemetry readable: the column-level GRANT below is what
-- limits readability, and it covers device_id alone. A policy widens which
-- ROWS are visible; the grant decides which COLUMNS. Smoke test assertion 3
-- proves stack_count stays denied.
create policy device_status_select_device on public.device_status
  for select to anon using (true);

create policy device_status_select_staff on public.device_status
  for select to authenticated using (true);

create policy status_events_insert_device on public.status_events
  for insert to anon with check (true);
create policy status_events_select_staff on public.status_events
  for select to authenticated using (true);

create policy service_windows_select_staff on public.service_windows
  for select to authenticated using (true);

-- ---------------------------------------------------------------------
-- 7. GRANTs
--    Policies alone are NOT sufficient, and Supabase starts every new public
--    table with ALL granted to anon via ALTER DEFAULT PRIVILEGES. Revoke
--    first; missing this is the easiest way to believe you have RLS and not.
-- ---------------------------------------------------------------------
revoke all on public.devices         from anon, authenticated, public;
revoke all on public.device_status   from anon, authenticated, public;
revoke all on public.status_events   from anon, authenticated, public;
revoke all on public.service_windows from anon, authenticated, public;

-- Device write path.
--
-- UPDATE on the payload columns only, plus SELECT on device_id alone: PostgREST
-- issues PATCH ...?device_id=eq.X, and evaluating that WHERE clause is a read
-- of device_id. Granting only that column keeps every telemetry column
-- unreadable, and device_id is a value the device already supplies in each
-- request, so it reveals nothing new.
grant select (device_id) on public.device_status to anon;
grant update (boot_id, uptime_s, stack_count, stack_status, levels, sensors_ok,
              sensors_online, battery_mv, battery_pct, charging, firmware, mac)
  on public.device_status to anon;

-- Plain INSERT; no SELECT of any kind. A duplicate raises 23505, which is the
-- idempotency mechanism.
grant insert (device_id, boot_id, seq, age_ms, reason, stack_count,
              stack_status, levels, sensors_ok, sensors_online,
              battery_pct, charging, firmware)
  on public.status_events to anon;

-- Human read path.
grant select on public.devices, public.device_status, public.status_events,
                public.service_windows
  to authenticated;

revoke all on function public.tg_device_status_stamp()   from public, anon, authenticated;
revoke all on function public.tg_status_events_stamp()   from public, anon, authenticated;
revoke all on function public.tg_devices_create_status() from public, anon, authenticated;
revoke all on function public.in_service_window(timestamptz, text, text, interval)
  from public, anon;
grant execute on function public.in_service_window(timestamptz, text, text, interval)
  to authenticated;

-- ---------------------------------------------------------------------
-- 8. Dashboard view.
--    security_invoker is mandatory: without it the view runs as its owner and
--    silently bypasses every policy above.
-- ---------------------------------------------------------------------
create view public.device_overview
with (security_invoker = true) as
select d.device_id,
       d.label,
       d.location,
       d.timezone,
       s.reported,
       s.updated_at,
       now() - s.updated_at as stale_for,
       public.in_service_window(now(), d.timezone, d.device_id) as in_service,

       -- Alarm only when the device SHOULD be reporting and is not. Devices are
       -- dark ~16h/day by design, so plain staleness is not a fault.
       (public.in_service_window(now(), d.timezone, d.device_id)
        and (s.updated_at is null
             or s.updated_at < now() - interval '5 minutes')) as offline,

       -- Outside service hours these numbers are the last known state from the
       -- previous service, not live data. Say so rather than letting a
       -- coordinator read a stale count as current.
       (not public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at is not null) as data_is_stale,

       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_pct, s.charging,
       s.uptime_s, s.firmware, s.mac
from public.devices d
left join public.device_status s using (device_id);

revoke all on public.device_overview from anon, authenticated, public;
grant select on public.device_overview to authenticated;

commit;

-- ---------------------------------------------------------------------
-- 9. Register your devices. The status row is created automatically.
-- ---------------------------------------------------------------------
-- insert into public.devices (device_id, label, location)
-- values ('BWL-001', 'bench rig', 'workshop');

-- ---------------------------------------------------------------------
-- 10. Retention (optional; enable pg_cron under Database > Extensions).
--     ~30 devices x ~50 changes/day is roughly 550k rows/year, ~170 MB.
-- ---------------------------------------------------------------------
-- select cron.schedule('bowlstack-retention', '17 3 * * *',
--   $$delete from public.status_events
--      where recorded_at < now() - interval '180 days'$$);
