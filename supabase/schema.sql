-- =====================================================================
--  Bowlstack -- complete schema
--
--  Run as owner in the Supabase SQL editor. DROPS EVERYTHING first, then
--  rebuilds, so it is idempotent and destructive in equal measure.
--
--  APPLY ORDER
--  -----------
--      1. schema.sql            this file
--      2. register_devices.sql  BWL-001 .. BWL-032
--      3. assign_devices.sql    permanent location/food_slot assignment
--      4. seed_meal_mapping.sql sample menus, for the front-end test bed
--      5. reset_spares.sql      only after a stray write; see that file
--      6. smoke_test.sql        17 assertions; expect ALL PASS
--
--  diagnose.sql is read-only and can be run at any time.
--
--  CONTENTS
--    1 devices             installation registry + permanent assignment
--    2 device_status       current state, one row per device
--    3 status_events       append-only history
--    4 triggers            timestamps, the reported flag, clock-free ages
--    5 service windows     when devices are expected to be awake
--    6 meal_food_mapping   what each slot serves, per meal per day
--    7 row-level security
--    8 grants
--    9 views               device_overview, slot_overview
--
--  FOUR IDEAS THAT SHAPE EVERYTHING BELOW
--  --------------------------------------
--  1. State is UPDATED in place; history is APPENDED only on real change.
--     One row per device never grows. Appending every report instead would be
--     ~30 devices x 8640/day = 259k rows/day and would exhaust the free tier in
--     about ten days.
--
--  2. There are NO UPSERTS, and the design does not need any.
--     `INSERT ... ON CONFLICT` is rejected for the anon role with 42501, while a
--     plain INSERT by the same role into the same table succeeds. Testing
--     established that ON CONFLICT wants full-table SELECT plus an RLS SELECT
--     policy -- which would let every device read every installation's
--     telemetry, precisely the property this schema exists to protect. So:
--       device_status  the row is created when the device is REGISTERED
--                      (trigger, section 2), leaving the device a plain UPDATE.
--       status_events  plain INSERT. Idempotency comes from a unique constraint:
--                      a retried batch raises 23505, which the firmware reads as
--                      "already recorded".
--     This also needs strictly fewer privileges than upserting would.
--
--  3. (location, food_slot) is NOT unique.
--     Darshanarthi runs three counters per dish position, so three stacks share
--     one slot and remaining stock for a dish is the SUM across them.
--     public.slot_overview computes it; reading a single device and calling it
--     "Rice remaining" under-reports 3x on the busiest positions in the hall.
--
--  4. Devices never store food names.
--     A device stores a slot NUMBER. What that slot serves changes three times a
--     day and lives in meal_food_mapping, keyed by DATE so a past bowl count
--     stays attributable to the dish that was actually there.
--
--  Front-end contract: docs/meal_mapping.md and docs/FRONTEND_HANDOFF.md.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 0. Clean slate.
-- ---------------------------------------------------------------------
drop view  if exists public.slot_overview;
drop view  if exists public.device_overview;
drop table if exists public.status_events      cascade;
drop table if exists public.device_status      cascade;
drop table if exists public.meal_food_mapping  cascade;
drop table if exists public.service_windows    cascade;
drop table if exists public.devices            cascade;
drop function if exists public.tg_device_status_stamp()   cascade;
drop function if exists public.tg_status_events_stamp()   cascade;
drop function if exists public.tg_devices_create_status() cascade;
drop function if exists public.tg_meal_food_mapping_touch() cascade;
drop function if exists public.in_service_window(timestamptz, text, text, interval) cascade;
drop function if exists public.current_meal_type(text, timestamptz) cascade;
drop function if exists public.current_meal_date(text, timestamptz) cascade;
drop function if exists public.meal_mapping_preload(text, text, date) cascade;

-- ---------------------------------------------------------------------
-- 1. devices -- human-managed registry. No device ever writes here.
-- ---------------------------------------------------------------------
create table public.devices (
  device_id   text primary key
                check (device_id ~ '^[A-Za-z0-9_-]{3,32}$'),

  -- PERMANENT assignment. A unit is installed in one area at one dish position,
  -- and neither changes for the life of the installation except on failure or
  -- reassignment. Both stay NULL until deployed; assign_devices.sql sets them.
  --
  --   location   D = Darshanarthi, M = Mahatma, T = Tiffin, R = reserved/future
  --   food_slot  1-8, the dish position on the station
  --
  -- NOTE: there is deliberately NO unique index on (location, food_slot).
  -- Darshanarthi has THREE counters serving each dish position, so three stacks
  -- share a slot. That inverts the primary dashboard number: remaining stock for
  -- a dish is the SUM of stack_count across the devices sharing the slot, not any
  -- one device's count. Reading a single device and calling it "Rice remaining"
  -- under-reports 3x on exactly the busiest positions in the hall. See
  -- public.slot_overview, which computes that sum in one place.
  location    text     check (location in ('D','M','T','R')),
  food_slot   smallint check (food_slot between 1 and 8),

  -- Free text, set from the front-end. Not the identity -- device_id is. Names
  -- the PHYSICAL position, never the dish: what sits in slot 3 changes with the
  -- meal, so a label saying "Rice" would be wrong by lunchtime and would compete
  -- with meal_food_mapping as a source of truth.
  label       text,

  timezone    text not null default 'Asia/Kolkata',
  last_mac    text,
  created_at  timestamptz not null default now()
);

comment on column public.devices.location is
  'Serving area: D Darshanarthi, M Mahatma, T Tiffin, R reserved/future. '
  'NULL until deployed.';

comment on column public.devices.food_slot is
  'Dish position on the station, 1-8. NOT unique; several stacks may serve one '
  'slot, so remaining stock for a dish is the SUM over the devices sharing it. '
  'What FOOD occupies a slot changes per meal and lives in meal_food_mapping; '
  'the slot number is the fixed physical position, not the dish.';

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

  -- Raw cell millivolts: a measurement, kept for diagnosis. An implausible
  -- value here identifies a divider or wiring fault that a band alone would
  -- disguise as a merely flat battery.
  --
  -- COUPLED TO FIRMWARE: config::BATTERY_PUBLISH_MAX_MV clamps to this 6000
  -- ceiling before sending. Do not narrow this bound without changing that
  -- constant. A value the firmware can produce but this CHECK rejects is not a
  -- rejected battery reading -- PostgREST answers 400, the whole PATCH fails,
  -- and the device stops reporting its BOWL COUNT until the wiring is fixed.
  -- A floating ADC pin measured 6365 mV on this hardware, so the case is real.
  battery_mv     integer     check (battery_mv between 0 and 6000),

  -- A BAND, not a percentage. A resting-voltage SoC estimate moves several
  -- points with load, temperature, cell age and per-unit ADC calibration, so a
  -- number would invite the UI to render precision the measurement lacks. NULL
  -- means no cell detected -- never a fabricated 'critical', which would be
  -- indistinguishable from a genuinely flat battery.
  battery_level  text        check (battery_level in ('good','medium','low','critical')),
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
  battery_level  text        check (battery_level in ('good','medium','low','critical')),

  -- Sensed on GPIO27 from the charger's 5 V rail. Nullable rather than NOT
  -- NULL so a firmware that cannot measure it reports nothing instead of
  -- inventing `false`, which would be indistinguishable from "genuinely not
  -- charging".
  charging       boolean,
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

-- Which meal is it now? Derived from service_windows -- the same rows that drive
-- `offline` -- so there is one definition of when lunch is. Those labels are
-- lowercase ('lunch') and meal_type is capitalised ('Lunch'), so initcap()
-- bridges them. Reads only the fleet-wide rows: "which meal is it" is a property
-- of the site, not of one unit.
--
-- Returns NULL outside every window, which is the correct answer. There is no
-- current meal at 3pm, and the UI must show last-known rather than invent one.
create or replace function public.current_meal_type(
  tz    text        default 'Asia/Kolkata',
  at_ts timestamptz default now()
) returns text language sql stable set search_path = '' as $$
  select initcap(w.label)
    from public.service_windows w
   where w.device_id is null
     and (at_ts at time zone coalesce(tz, 'UTC'))::time
           between w.starts_at and w.ends_at
   order by w.starts_at
   limit 1
$$;

-- The service date in LOCAL terms. Not now()::date -- that is the server's date,
-- and a 21:00 dinner in Asia/Kolkata is already the next UTC day, so the evening
-- meal would be filed against tomorrow.
create or replace function public.current_meal_date(
  tz    text        default 'Asia/Kolkata',
  at_ts timestamptz default now()
) returns date language sql stable set search_path = '' as $$
  select (at_ts at time zone coalesce(tz, 'UTC'))::date
$$;

-- ---------------------------------------------------------------------
-- 6. meal_food_mapping -- what each slot serves, per meal per day.
--
-- Two things change at completely different rates, so they are stored apart:
-- where a device IS (rare, on hardware moves) lives on devices; what its slot
-- SERVES changes three times a day and lives here. DEVICES NEVER STORE FOOD
-- NAMES -- a device stores a slot number, and the dashboard resolves
-- device -> location + food_slot -> this table -> food_name.
--
-- Keyed by meal_date rather than holding only the current menu, so history stays
-- attributable: a past bowl count can be joined to the dish that was actually in
-- that slot at the time, answering "how much dal did we get through last
-- Tuesday". Storing only the present would make every historical count
-- unattributable the moment the menu rotated, and that is NOT recoverable after
-- the fact -- which is why the date is in the key rather than bolted on later.
-- ---------------------------------------------------------------------
create table public.meal_food_mapping (
  -- Surrogate key alongside the natural one. Redundant, deliberately: PostgREST
  -- and most client tooling want a single-column handle for an update or delete,
  -- and rebuilding it from four columns at every call site is worse.
  id         bigint generated always as identity,

  location   text     not null check (location in ('D','M','T','R')),
  meal_type  text     not null check (meal_type in ('Breakfast','Lunch','Dinner')),
  meal_date  date     not null,
  food_slot  smallint not null check (food_slot between 1 and 8),

  -- A blank name is not a mapping. Without this, an admin page that submits empty
  -- inputs for untouched slots fills the table with rows that render as an
  -- unnamed dish rather than as no dish at all. Clearing a slot is a DELETE.
  food_name  text     not null check (length(btrim(food_name)) > 0),

  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),

  constraint meal_food_mapping_pk
    primary key (location, meal_date, meal_type, food_slot),
  constraint meal_food_mapping_id_key unique (id)
);

-- Answers both "what is in this slot right now" and "what was in it then" -- the
-- lookup every dashboard render makes.
create index meal_food_mapping_lookup_idx
  on public.meal_food_mapping (location, meal_date desc, meal_type);

create or replace function public.tg_meal_food_mapping_touch()
returns trigger language plpgsql set search_path = '' as $$
begin
  new.updated_at := now();
  -- created_at is immutable: an UPDATE must not rewrite when the row first
  -- appeared, or the column has no audit value.
  new.created_at := old.created_at;
  return new;
end $$;

create trigger meal_food_mapping_touch
  before update on public.meal_food_mapping
  for each row execute function public.tg_meal_food_mapping_touch();

-- Preload a mapping from the most recent previous meal of the same type and
-- location, so an admin edits differences instead of retyping five dishes.
-- Today's Lunch inherits yesterday's Lunch; tomorrow's Breakfast inherits
-- today's. Nothing at all if there is no history -- the UI starts empty.
--
-- `is_saved` is why this is a function and not a plain query, and the UI MUST act
-- on it. A preloaded form is pixel-identical to a saved one, so without the flag
-- an admin who opens tomorrow's Lunch, agrees with every inherited dish and
-- navigates away would reasonably believe the menu was recorded -- and no row
-- would exist. `source_date` carries the "carried over from 24 Jul" line.
create or replace function public.meal_mapping_preload(
  p_location  text,
  p_meal_type text,
  p_meal_date date
) returns table (
  food_slot   smallint,
  food_name   text,
  source_date date,
  is_saved    boolean
) language sql stable set search_path = '' as $$
  with exact as (
    select m.food_slot, m.food_name, m.meal_date as source_date, true as is_saved
      from public.meal_food_mapping m
     where m.location  = p_location
       and m.meal_type = p_meal_type
       and m.meal_date = p_meal_date
  ),
  previous as (
    select m.food_slot, m.food_name, m.meal_date as source_date, false as is_saved
      from public.meal_food_mapping m
     where m.location  = p_location
       and m.meal_type = p_meal_type
       and m.meal_date = (
         select max(m2.meal_date)
           from public.meal_food_mapping m2
          where m2.location  = p_location
            and m2.meal_type = p_meal_type
            and m2.meal_date < p_meal_date
       )
  )
  select * from exact
  union all
  select * from previous where not exists (select 1 from exact)
  order by food_slot
$$;

-- ---------------------------------------------------------------------
-- 7. Row-level security
-- ---------------------------------------------------------------------
alter table public.devices           enable row level security;
alter table public.device_status     enable row level security;
alter table public.status_events     enable row level security;
alter table public.service_windows   enable row level security;
alter table public.meal_food_mapping enable row level security;

-- devices: no anon policy at all, so anon is denied everything. The foreign
-- keys from the other tables still work -- referential integrity checks
-- deliberately bypass row security and run as the table owner.
create policy devices_select_staff on public.devices
  for select to authenticated using (true);

-- The front-end configuration page assigns location, food_slot and label.
-- Devices never touch this table -- anon has no policy here at all.
create policy devices_update_staff on public.devices
  for update to authenticated using (true) with check (true);

-- meal_food_mapping: staff-only, full CRUD. Devices get NOTHING here, and not by
-- omission -- a device stores a slot number and never learns or needs the menu,
-- so granting anon access would hand every field unit the whole site's
-- configuration for no benefit.
create policy meal_food_mapping_rw_staff on public.meal_food_mapping
  for all to authenticated using (true) with check (true);

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
-- 8. GRANTs
--    Policies alone are NOT sufficient, and Supabase starts every new public
--    table with ALL granted to anon via ALTER DEFAULT PRIVILEGES. Revoke
--    first; missing this is the easiest way to believe you have RLS and not.
-- ---------------------------------------------------------------------
revoke all on public.devices           from anon, authenticated, public;
revoke all on public.device_status     from anon, authenticated, public;
revoke all on public.status_events     from anon, authenticated, public;
revoke all on public.service_windows   from anon, authenticated, public;
revoke all on public.meal_food_mapping from anon, authenticated, public;

-- Device write path.
--
-- UPDATE on the payload columns only, plus SELECT on device_id alone: PostgREST
-- issues PATCH ...?device_id=eq.X, and evaluating that WHERE clause is a read
-- of device_id. Granting only that column keeps every telemetry column
-- unreadable, and device_id is a value the device already supplies in each
-- request, so it reveals nothing new.
grant select (device_id) on public.device_status to anon;
grant update (boot_id, uptime_s, stack_count, stack_status, levels, sensors_ok,
              sensors_online, battery_mv, battery_level, charging, firmware, mac)
  on public.device_status to anon;

-- Plain INSERT; no SELECT of any kind. A duplicate raises 23505, which is the
-- idempotency mechanism.
grant insert (device_id, boot_id, seq, age_ms, reason, stack_count,
              stack_status, levels, sensors_ok, sensors_online,
              battery_level, charging, firmware)
  on public.status_events to anon;

-- Human read path.
grant select on public.devices, public.device_status, public.status_events,
                public.service_windows
  to authenticated;

-- Front-end configuration page. device_id is excluded on purpose: it is the
-- installation's identity and the key every history row hangs off, so it must
-- not be editable from a UI.
grant update (location, food_slot, label, timezone)
  on public.devices to authenticated;

-- Admin page for the menu. Full CRUD, because clearing a slot is a DELETE rather
-- than an empty name -- "no dish here" and "a dish with no name" are different,
-- and only one of them should render.
--
-- The identity sequence needs no grant: `generated always as identity` skips the
-- sequence ACL check, and the id cannot be supplied by a client.
grant select, insert, update, delete on public.meal_food_mapping to authenticated;

revoke all on function public.tg_device_status_stamp()   from public, anon, authenticated;
revoke all on function public.tg_status_events_stamp()   from public, anon, authenticated;
revoke all on function public.tg_devices_create_status() from public, anon, authenticated;
revoke all on function public.tg_meal_food_mapping_touch() from public, anon, authenticated;
revoke all on function public.in_service_window(timestamptz, text, text, interval)
  from public, anon;
revoke all on function public.current_meal_type(text, timestamptz) from public, anon;
revoke all on function public.current_meal_date(text, timestamptz) from public, anon;
revoke all on function public.meal_mapping_preload(text, text, date) from public, anon;
grant execute on function public.in_service_window(timestamptz, text, text, interval)
  to authenticated;
grant execute on function public.current_meal_type(text, timestamptz) to authenticated;
grant execute on function public.current_meal_date(text, timestamptz) to authenticated;
grant execute on function public.meal_mapping_preload(text, text, date) to authenticated;

-- ---------------------------------------------------------------------
-- 9. Dashboard views.
--    security_invoker is mandatory on both: without it a view runs as its owner
--    and silently bypasses every policy above.
-- ---------------------------------------------------------------------
create view public.device_overview
with (security_invoker = true) as
select d.device_id,
       d.location,
       d.food_slot,
       d.label,
       d.timezone,

       -- What this device's slot is serving RIGHT NOW. NULL outside service
       -- hours, or when nobody has entered a mapping -- both mean "no current
       -- dish", which is not the same as a dish with no name.
       m.food_name                          as current_food,
       public.current_meal_type(d.timezone) as current_meal,

       s.reported,
       s.updated_at,
       now() - s.updated_at as stale_for,
       public.in_service_window(now(), d.timezone, d.device_id) as in_service,

       -- Alarm only when the device SHOULD be reporting and is not. Devices are
       -- dark ~16h/day by design, so plain staleness is not a fault.
       --
       -- s.reported gates this so a device that has NEVER reported does not
       -- alarm: that is a registered-but-not-yet-deployed unit, not a failure.
       -- Without it, pre-registering the fleet would light up every unbuilt
       -- station as offline and bury the ones that genuinely went down.
       (coalesce(s.reported, false)
        and public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at < now() - interval '5 minutes') as offline,

       -- Registered but never heard from -- i.e. awaiting installation.
       (not coalesce(s.reported, false)) as awaiting_deployment,

       -- Outside service hours these numbers are the last known state from the
       -- previous service, not live data. Say so rather than letting a
       -- coordinator read a stale count as current.
       (not public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at is not null) as data_is_stale,

       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_level, s.charging,
       s.uptime_s, s.firmware, s.mac
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone);

-- Per-DISH stock -- the number the kitchen in-charge actually wants, and the
-- primary screen's source.
--
-- This view exists because (location, food_slot) is not unique: Darshanarthi runs
-- THREE counters per dish position, so remaining rice is the sum of three stacks.
-- Aggregating in the client would put the same arithmetic AND the same trust rules
-- into every screen that shows stock, and they would diverge.
--
-- Quantity is kept separate from trust deliberately. bowls_trusted counts only
-- devices reporting 'ok'; a degraded device's count is a lower bound and a
-- discontiguous one is not a count at all, so folding them into the total would
-- silently overstate confidence. The flags say what is wrong; the number says
-- what can be relied on.
create view public.slot_overview
with (security_invoker = true) as
select d.location,
       d.food_slot,
       max(m.food_name)                                        as current_food,
       public.current_meal_type(max(d.timezone))               as current_meal,
       count(*)                                                as devices,
       count(*) filter (where coalesce(s.reported, false))     as devices_reported,

       -- Ceiling for this slot, so a progress bar need not hardcode one. If a
       -- fourth stack joins Darshanarthi slot 1 the achievable total rises, and a
       -- UI with "max 12" baked in would misreport the busiest position in the
       -- hall.
       (count(*) * 4)::bigint                                  as bowls_capacity,

       -- The trustworthy total. NULL, not 0, when no device reported: no data is
       -- not an empty counter, and the two send staff to do opposite things.
       sum(s.stack_count) filter (where s.stack_status = 'ok') as bowls_trusted,
       sum(s.stack_count)                                      as bowls_reported,

       bool_or(s.stack_status = 'discontiguous')               as any_fault,
       bool_or(s.stack_status = 'degraded')                    as any_degraded,
       bool_or(s.battery_level in ('low','critical'))          as any_battery_warn,
       bool_or(coalesce(s.reported, false)
               and public.in_service_window(now(), d.timezone, d.device_id)
               and s.updated_at < now() - interval '5 minutes') as any_offline,
       min(s.updated_at)                                       as oldest_update
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone)
 where d.location is not null
   and d.food_slot is not null
 group by d.location, d.food_slot;

revoke all on public.device_overview from anon, authenticated, public;
revoke all on public.slot_overview   from anon, authenticated, public;
grant select on public.device_overview to authenticated;
grant select on public.slot_overview   to authenticated;

commit;

-- ---------------------------------------------------------------------
-- 10. Next steps. Run these as separate files, in this order:
--       register_devices.sql    BWL-001 .. BWL-032
--       assign_devices.sql      the permanent location/food_slot assignment
--       seed_meal_mapping.sql   sample menus, for the front-end test bed
--       reset_spares.sql        restores awaiting_deployment for the reserved 8
--       smoke_test.sql          17 assertions; expect ALL PASS
-- ---------------------------------------------------------------------

-- ---------------------------------------------------------------------
-- 11. Retention (optional; enable pg_cron under Database > Extensions).
--     ~30 devices x ~50 changes/day is roughly 550k rows/year, ~170 MB.
-- ---------------------------------------------------------------------
-- select cron.schedule('bowlstack-retention', '17 3 * * *',
--   $$delete from public.status_events
--      where recorded_at < now() - interval '180 days'$$);
