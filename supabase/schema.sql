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
--      6. smoke_test.sql        25 assertions; expect ALL PASS
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
--    9 views               device_overview, slot_overview, slot_quantity
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
drop view  if exists public.slot_quantity;
drop view  if exists public.slot_overview;
drop view  if exists public.device_overview;
drop table if exists public.status_events      cascade;
drop table if exists public.device_status      cascade;
drop table if exists public.meal_food_mapping  cascade;
drop table if exists public.meal_menu_template cascade;
drop table if exists public.service_windows    cascade;
drop table if exists public.devices            cascade;
drop function if exists public.tg_device_status_stamp()   cascade;
drop function if exists public.tg_status_events_stamp()   cascade;
drop function if exists public.tg_devices_create_status() cascade;
drop function if exists public.tg_meal_food_mapping_touch() cascade;
drop function if exists public.in_service_window(timestamptz, text, text, interval) cascade;
drop function if exists public.last_service_window_start(text, text, timestamptz) cascade;
drop function if exists public.last_service_window_end(text, text, timestamptz) cascade;
drop function if exists public.offline_after() cascade;
drop function if exists public.meal_template_apply(text, date, date, boolean) cascade;
drop function if exists public.current_meal_type(text, timestamptz) cascade;
drop function if exists public.current_meal_date(text, timestamptz) cascade;
drop function if exists public.meal_mapping_preload(text, text, date) cascade;
drop function if exists public.last_served_meal(text, timestamptz) cascade;
-- Without this entry a second run of schema.sql leaves the previous definition
-- in place, which is the one way this file stops being idempotent.
drop function if exists public.weight_mismatch_tolerance() cascade;

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
  created_at  timestamptz not null default now(),

  -- WHAT THIS INSTALLATION MEASURES, and therefore which half of
  -- device_status is a measurement and which half is simply absent.
  --
  --   stack  four VL53L0X up a pipe, reporting a bowl COUNT. Leaves every
  --          weight_* column NULL.
  --   scale  three NAU7802 under a platform, reporting GRAMS. Leaves every
  --          stack_* column NULL.
  --
  -- A hardware fact fixed at registration, never a setting. No device writes
  -- it -- `devices` has no anon policy at all.
  --
  -- DEFAULT 'stack' so the 24 bowl counters already registered are correct
  -- the moment the column appears, which is what makes migrate_loadcell.sql
  -- safe to run mid-trial.
  --
  -- LAST, matching where ALTER TABLE ADD COLUMN puts it, so a rebuilt database
  -- and a migrated one have the same column order and `select *` cannot tell
  -- them apart.
  kind        text not null default 'stack'
                constraint devices_kind_ck check (kind in ('stack','scale'))
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
  mac            text,

  -- --- the load-cell half -------------------------------------------------
  -- Nullable, like everything above, and for a sharper reason: a bowl counter
  -- never writes any of these and a load cell never writes the stack_* columns
  -- above. One row shape, two products, and `kind` in `devices` says which
  -- half to read.
  --
  -- THE STATE IS ALWAYS KNOWN; THE NUMBER IS NOT. A scale that has never seen
  -- a known mass has counts and no grams -- deliberately, because inventing a
  -- factor produces a confident wrong weight. So weight_state is what the
  -- device always sends and weight_g is present only when the state is 'ok'.
  --
  -- ZERO IS A REAL WEIGHT. An empty, tared, calibrated platform reads 0 g and
  -- that is a measurement: the counter is empty and needs refilling. NULL
  -- means nobody knows. Same distinction bowls_trusted already draws, and the
  -- CHECK below is what stops the two collapsing.
  weight_g       integer,
  weight_state   text,
  cells_online   smallint,
  counts_per_gram numeric(9,3),
  net_counts     bigint,

  -- Reported in priority order, the fault NEAREST THE HARDWARE first, because
  -- that is the one somebody can act on:
  --   no_cells       no converter is producing conversions at all
  --   cells_partial  fewer than three are. Cells under one platform SUM, so a
  --                  missing cell makes the total silently LOW, not noisy
  --   over_range     a converter hit the end of its 24-bit range, so the
  --                  total has stopped rising
  --   settling       the automatic power-up tare has not concluded
  --   uncalibrated   no counts-to-gram factor on this unit; no grams exist
  --   untared        calibrated, so right about CHANGE and wrong about the
  --                  absolute -- it still includes the platform
  --   ok             a weight
  constraint device_status_weight_state_ck
    check (weight_state is null or weight_state in
           ('ok','uncalibrated','untared','settling',
            'cells_partial','no_cells','over_range')),

  -- THE HONESTY RULE, MADE STRUCTURAL. A gram figure exists exactly when the
  -- state says it does. Without this a firmware bug could publish weight_g
  -- beside 'uncalibrated' and put an invented number on the dashboard with
  -- nothing objecting; and 'ok' with a NULL weight would be a state claiming a
  -- measurement it did not carry. An equality of two booleans rather than two
  -- CHECKs, so both directions fail at the edge as a 400.
  constraint device_status_weight_agrees_ck
    check (weight_state is null
           or (weight_state = 'ok') = (weight_g is not null)),

  -- Sanity rails, the role bowl_weight_g's 100..50000 plays. Three 20 kg cells
  -- is 60 kg of rating, so 100 kg is unreachable and identifies a units mix-up
  -- or a wild calibration factor. The negative floor exists because a tared
  -- platform legitimately drifts a few grams below zero, and because removing
  -- something present at tare time is a real thing to do.
  --
  -- COUPLED TO FIRMWARE exactly as battery_mv's 0..6000 bound is: a value the
  -- device can produce but this rejects is not a rejected weight, it is a 400
  -- that fails the whole PATCH -- so the station stops reporting anything at
  -- all, including the state that would have explained why.
  constraint device_status_weight_range_ck
    check (weight_g is null or weight_g between -5000 and 100000),

  constraint device_status_cells_online_ck
    check (cells_online is null or cells_online between 0 and 8)
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

-- The edges are ASYMMETRIC, deliberately. An earlier version widened the
-- window by 10 minutes at both ends, which alarmed at both edges of every
-- meal: at 05:50 the window was "open" but devices had not booted
-- (updated_at = last night -> offline true until the first report), and at
-- 21:00-21:10 devices were legitimately powered down but still "expected".
-- Three windows a day, two edges each -- the fleet cried wolf six times
-- daily, which trains people to ignore the one alarm that matters.
--
-- `margin` is the boot grace: how long after opening before absence is
-- judged. 90 s covers power-on + WiFi join + first PATCH. The close is
-- sharp: a device that stops at ends_at is asleep, not missing.
create or replace function public.in_service_window(
  at_ts  timestamptz,
  tz     text,
  dev_id text     default null,
  margin interval default interval '90 seconds'
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
           (select t from local_now) >= (a.starts_at + margin)
       and (select t from local_now) <= a.ends_at), false)
    from applicable a;
$$;

-- When did the most recently COMPLETED service window start, in absolute
-- time? The anchor for missed_last_service: a device whose updated_at
-- precedes it slept through a whole window it should have attended --
-- which, unlike plain staleness, is a fault the dark hours cannot excuse.
--
-- Anchored on the window START, not its end, so staff powering a station
-- down early never flags it: reporting at any point during the window
-- clears the device. Evaluated in the device's own timezone; (date + time)
-- AT TIME ZONE tz makes a 21:00 IST dinner that is already tomorrow in UTC
-- resolve correctly. Today's and yesterday's instances suffice: the
-- service_window_order CHECK forces every window to close the same local
-- day it opens, so yesterday always contributes a completed candidate and
-- the result is never NULL for a device with any applicable window.
create or replace function public.last_service_window_start(
  tz     text,
  dev_id text        default null,
  at_ts  timestamptz default now()
) returns timestamptz language sql stable set search_path = '' as $$
  with applicable as (
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id = dev_id
    union all
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id is null
        and not exists (select 1 from public.service_windows x
                         where x.device_id = dev_id)
  ),
  local_day as (
    select (at_ts at time zone coalesce(tz, 'UTC'))::date as d
  ),
  candidates as (
    select ((ld.d - n) + a.starts_at) at time zone coalesce(tz, 'UTC') as w_start,
           ((ld.d - n) + a.ends_at)   at time zone coalesce(tz, 'UTC') as w_end
      from applicable a
     cross join local_day ld
     cross join generate_series(0, 1) n
  )
  select max(w_start) from candidates where w_end <= at_ts;
$$;

-- ...and when did it END? This is the anchor missed_last_service uses:
-- "alive at the CLOSE of the last completed window". A healthy 20 s-heartbeat
-- device is at most ~40 s stale when a window shuts, so subtracting
-- offline_after() cannot flag it, and anything silent longer was already red
-- in-window via `offline`. (An earlier cut anchored on the window START to
-- tolerate early power-downs; field use showed a unit switched off mid-lunch
-- then read healthy all afternoon -- the very false negative this flag exists
-- to kill. A station legitimately shut early therefore flags until it next
-- reports; if early shutdown becomes routine practice, soften this rule.)
create or replace function public.last_service_window_end(
  tz     text,
  dev_id text        default null,
  at_ts  timestamptz default now()
) returns timestamptz language sql stable set search_path = '' as $$
  with applicable as (
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id = dev_id
    union all
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id is null
        and not exists (select 1 from public.service_windows x
                         where x.device_id = dev_id)
  ),
  local_day as (
    select (at_ts at time zone coalesce(tz, 'UTC'))::date as d
  ),
  candidates as (
    select ((ld.d - n) + a.ends_at) at time zone coalesce(tz, 'UTC') as w_end
      from applicable a
     cross join local_day ld
     cross join generate_series(0, 1) n
  )
  select max(w_end) from candidates where w_end <= at_ts;
$$;

-- How long a device may be silent, while it is SUPPOSED to be reporting, before
-- it counts as offline.
--
-- A function rather than a literal for two reasons. It was written out twice --
-- device_overview.offline and slot_overview.any_offline -- so the two could
-- drift and disagree about whether the same device was down. And retuning it is
-- now one CREATE OR REPLACE, with no need to re-run schema.sql, which drops
-- every table including the telemetry history.
--
-- SIZING. It must comfortably exceed the firmware heartbeat, or a healthy device
-- alarms between its own posts:
--
--     detection (worst case) = this threshold + the front-end poll interval
--     this threshold         > STATUS_PERIOD_MS + one RETRY_PERIOD_MS
--
-- At 20 s heartbeat and 15 s retry backoff, a healthy unit that loses one post
-- is 35 s stale, so 40 s leaves real slack while giving ~60 s detection against
-- the dashboard's 20 s poll. Going below ~40 s starts flagging ordinary WiFi
-- reconnections as outages.
create or replace function public.offline_after()
returns interval language sql immutable parallel safe as $$
  select interval '40 seconds'
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

  -- Mass of ONE full bowl of this dish, in grams. NULLABLE and deliberately
  -- NOT defaulted to zero: everywhere else here NULL means "no reading" and 0
  -- means "measured, and empty" (see slot_overview.bowls_trusted). Defaulting
  -- to 0 would render a full counter as "0.0 kg remaining" the moment someone
  -- typed a dish and forgot the weight, sending staff to refill something that
  -- is full. NULL renders as "weight not set" and prompts instead.
  --
  -- GRAMS as integer, not kilograms as numeric. slot_quantity sums products
  -- across up to five devices and three areas per slot; binary floating point
  -- accumulates visible drift on a screen showing one decimal. The UI divides
  -- by 1000 exactly once, at render.
  --
  -- The bounds are sanity rails: 100 g is below any real serving bowl, 50 kg
  -- above any bowl a person lifts onto a counter. They turn a units mix-up
  -- (kg typed into a grams field) into a 400 at the edge rather than a
  -- dashboard reading 14 000 kg.
  bowl_weight_g integer check (bowl_weight_g is null
                               or bowl_weight_g between 100 and 50000),

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

-- The fixed weekly menu, per weekday. Configuration that PRODUCES
-- meal_food_mapping rows; never itself the menu -- the dashboard views keep
-- reading the dated table and nothing else. A weekday-keyed template has no
-- date, so if the views resolved dishes from it directly, a whole service
-- could pass with a dish name on screen and no dated row behind it, after
-- which the historical join returns NULL for that day forever and next
-- year's template edit would silently rewrite what "was served" last
-- Tuesday. The template reaches the dashboard only by being MATERIALISED:
-- the editor's Save, or meal_template_apply() (section 6b).
--
-- weekday is 0-6 with 0 = Sunday -- Postgres extract(dow) AND JavaScript
-- Date.getDay(), so neither side converts. isodow (1=Mon..7=Sun) was
-- rejected: that off-by-one serves Monday's menu on Sunday and survives
-- testing until a week boundary.
create table public.meal_menu_template (
  id         bigint generated always as identity,

  -- 'R' excluded: reserved units occupy no serving position, so a template
  -- row there could only be a mistake -- and one that would break
  -- smoke_test's carry-forward fixture, which deliberately uses 'R' as a
  -- location no real menu touches.
  location   text     not null check (location in ('D','M','T')),
  weekday    smallint not null check (weekday between 0 and 6),
  meal_type  text     not null check (meal_type in ('Breakfast','Lunch','Dinner')),
  food_slot  smallint not null check (food_slot between 1 and 8),

  -- Same CHECK as the dated table, for the same reason: a blank name is
  -- not a mapping, and clearing a slot is a DELETE.
  food_name  text     not null check (length(btrim(food_name)) > 0),

  -- Mass of ONE full bowl of this dish, in grams. NULLABLE and deliberately
  -- NOT defaulted to zero: everywhere else here NULL means "no reading" and 0
  -- means "measured, and empty" (see slot_overview.bowls_trusted). Defaulting
  -- to 0 would render a full counter as "0.0 kg remaining" the moment someone
  -- typed a dish and forgot the weight, sending staff to refill something that
  -- is full. NULL renders as "weight not set" and prompts instead.
  --
  -- GRAMS as integer, not kilograms as numeric. slot_quantity sums products
  -- across up to five devices and three areas per slot; binary floating point
  -- accumulates visible drift on a screen showing one decimal. The UI divides
  -- by 1000 exactly once, at render.
  --
  -- The bounds are sanity rails: 100 g is below any real serving bowl, 50 kg
  -- above any bowl a person lifts onto a counter. They turn a units mix-up
  -- (kg typed into a grams field) into a 400 at the edge rather than a
  -- dashboard reading 14 000 kg.
  bowl_weight_g integer check (bowl_weight_g is null
                               or bowl_weight_g between 100 and 50000),

  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),

  constraint meal_menu_template_pk
    primary key (location, weekday, meal_type, food_slot),
  constraint meal_menu_template_id_key unique (id)
);

create trigger meal_menu_template_touch
  before update on public.meal_menu_template
  for each row execute function public.tg_meal_food_mapping_touch();

-- Preload a mapping: saved rows for the exact date first; else, for
-- today-or-future dates, the weekly template for that weekday; else the
-- most recent previous meal of the same type and location (carry-forward).
-- Nothing at all if there is no history -- the UI starts empty.
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
  food_slot     smallint,
  food_name     text,
  -- Weight rides with the dish through all three branches below, so an
  -- inherited or templated menu arrives weighed as well as named. Carrying
  -- the name forward but not the weight would mean re-typing a figure that
  -- does not change from one day to the next.
  bowl_weight_g integer,
  source_date   date,
  is_saved      boolean
) language sql stable set search_path = '' as $$
  with exact as (
    select m.food_slot, m.food_name, m.bowl_weight_g,
           m.meal_date as source_date, true as is_saved
      from public.meal_food_mapping m
     where m.location  = p_location
       and m.meal_type = p_meal_type
       and m.meal_date = p_meal_date
  ),
  -- The weekly template, for TODAY-OR-FUTURE dates only. source_date is the
  -- target date itself -- the marker the UI reads as "from the template"
  -- (carry-forward always carries an earlier date). Past gaps never see the
  -- template: what was probably served then is what was served around it,
  -- not what this week's configuration says.
  templ as (
    select t.food_slot, t.food_name, t.bowl_weight_g,
           p_meal_date as source_date, false as is_saved
      from public.meal_menu_template t
     where t.location  = p_location
       and t.meal_type = p_meal_type
       and t.weekday   = extract(dow from p_meal_date)::smallint
       and p_meal_date >= public.current_meal_date('Asia/Kolkata')
  ),
  -- Carry-forward means "probably the same as the LAST service" -- so it
  -- only reaches back two days. Unbounded, it resurrected week-old menus as
  -- drafts that could never be refused: blanking a draft deletes nothing
  -- (there are no rows for the date), so refresh brought the zombie back.
  -- Field report: slots blanked and saved kept "restoring from 26 Jul".
  previous as (
    select m.food_slot, m.food_name, m.bowl_weight_g,
           m.meal_date as source_date, false as is_saved
      from public.meal_food_mapping m
     where m.location  = p_location
       and m.meal_type = p_meal_type
       and m.meal_date = (
         select max(m2.meal_date)
           from public.meal_food_mapping m2
          where m2.location  = p_location
            and m2.meal_type = p_meal_type
            and m2.meal_date < p_meal_date
            and m2.meal_date >= p_meal_date - 2
       )
  )
  select * from exact
  union all
  select * from templ    where not exists (select 1 from exact)
  union all
  select * from previous where not exists (select 1 from exact)
                           and not exists (select 1 from templ)
  order by food_slot
$$;

-- ---------------------------------------------------------------------
-- 6b. meal_menu_template -- the fixed weekly menu, per weekday.
--
-- Configuration that PRODUCES meal_food_mapping rows; never itself the
-- menu. The dashboard views keep reading the dated table and nothing
-- else: a weekday-keyed template has no date, so if the views resolved
-- dishes from it directly, a whole service could pass with a dish name on
-- screen and no dated row behind it -- after which the historical join in
-- docs/meal_mapping.md returns NULL for that day forever, and next year's
-- template edit would silently rewrite what "was served" last Tuesday.
-- The template reaches the dashboard only by being MATERIALISED: the
-- editor's Save, or meal_template_apply() over a date range.
--
-- weekday is 0-6 with 0 = Sunday -- Postgres extract(dow) AND JavaScript
-- Date.getDay(), so neither side converts. isodow (1=Mon..7=Sun) was
-- rejected: that off-by-one serves Monday's menu on Sunday and survives
-- testing until a week boundary.
--
-- Location 'R' is excluded: reserved units occupy no serving position, so
-- a template row there could only be a mistake -- and one that would break
-- smoke_test's carry-forward fixture, which deliberately uses 'R' as a
-- location no real menu touches.
--
-- (The table itself is created in section 6, before meal_mapping_preload:
-- SQL-language function bodies are validated at CREATE time, so the
-- template must exist before the preload that reads it.)
-- ---------------------------------------------------------------------

-- Freeze the template into dated rows -- "apply this week" is this, called
-- with today .. today+6.
--
--   - Past dates are refused outright: writing today's template into last
--     month would fabricate history as ordinary, unmarked rows -- the
--     exact corruption the date key exists to prevent.
--   - Skips at MEAL granularity: if ANY row exists for (location, date,
--     meal), that whole meal is left alone. Row-level filling looked
--     friendlier but silently resurrects a slot someone deliberately
--     DELETEd ("no dal today"), and does it as a saved row nobody typed.
--   - p_overwrite := true resets each meal in the range to the template
--     exactly (delete then insert); the UI gates it behind a confirm.
--   - Range capped at 31 days: a fat-fingered year would write ~5k rows.
--
-- Runs as the INVOKER: authenticated's own grants on meal_food_mapping
-- authorise the writes, so this adds no privilege anywhere.
create or replace function public.meal_template_apply(
  p_location  text,
  p_from      date,
  p_to        date,
  p_overwrite boolean default false
) returns table (
  meal_date   date,
  meal_type   text,
  written     integer,
  skipped     boolean
) language plpgsql set search_path = '' as $$
declare
  v_today date := public.current_meal_date('Asia/Kolkata');
  v_date  date;
  v_meal  text;
  v_n     integer;
begin
  if p_from is null or p_to is null or p_location is null then
    raise exception 'meal_template_apply: location and both dates are required';
  end if;
  if p_from < v_today then
    raise exception 'meal_template_apply: % is in the past -- the template must never rewrite history', p_from;
  end if;
  if p_to < p_from then
    raise exception 'meal_template_apply: range is backwards (% .. %)', p_from, p_to;
  end if;
  if p_to - p_from > 31 then
    raise exception 'meal_template_apply: range is % days; the cap is 31', p_to - p_from;
  end if;

  for v_date in select d::date from generate_series(p_from, p_to, interval '1 day') d loop
    foreach v_meal in array array['Breakfast','Lunch','Dinner'] loop
      if not exists (select 1 from public.meal_menu_template t
                      where t.location = p_location
                        and t.weekday  = extract(dow from v_date)::smallint
                        and t.meal_type = v_meal) then
        continue;
      end if;

      -- Today's ALREADY-COMPLETED meals are history too, just very recent
      -- history: writing the template into this morning's Breakfast at 3pm
      -- fabricates a served meal nobody recorded. Skip them -- reported as
      -- skipped so the summary does not silently under-deliver.
      if v_date = v_today
         and exists (select 1 from public.service_windows w
                      where w.device_id is null
                        and initcap(w.label) = v_meal
                        and (now() at time zone 'Asia/Kolkata')::time > w.ends_at) then
        meal_date := v_date; meal_type := v_meal; written := 0; skipped := true;
        return next;
        continue;
      end if;

      if not p_overwrite
         and exists (select 1 from public.meal_food_mapping m
                      where m.location = p_location
                        and m.meal_date = v_date
                        and m.meal_type = v_meal) then
        meal_date := v_date; meal_type := v_meal; written := 0; skipped := true;
        return next;
        continue;
      end if;

      if p_overwrite then
        delete from public.meal_food_mapping m
         where m.location = p_location
           and m.meal_date = v_date
           and m.meal_type = v_meal;
      end if;

      insert into public.meal_food_mapping
             (location, meal_type, meal_date, food_slot, food_name, bowl_weight_g)
      select t.location, t.meal_type, v_date, t.food_slot, t.food_name,
             t.bowl_weight_g
        from public.meal_menu_template t
       where t.location = p_location
         and t.weekday  = extract(dow from v_date)::smallint
         and t.meal_type = v_meal;
      get diagnostics v_n = row_count;

      meal_date := v_date; meal_type := v_meal; written := v_n; skipped := false;
      return next;
    end loop;
  end loop;
end $$;

-- ---------------------------------------------------------------------
-- 7. Row-level security
-- ---------------------------------------------------------------------
alter table public.devices            enable row level security;
alter table public.device_status      enable row level security;
alter table public.status_events      enable row level security;
alter table public.service_windows    enable row level security;
alter table public.meal_food_mapping  enable row level security;
alter table public.meal_menu_template enable row level security;

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

-- meal_menu_template: same posture, same argument -- a device stores a slot
-- number and never learns the menu, let alone the whole week's.
create policy meal_menu_template_rw_staff on public.meal_menu_template
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
revoke all on public.devices            from anon, authenticated, public;
revoke all on public.device_status      from anon, authenticated, public;
revoke all on public.status_events      from anon, authenticated, public;
revoke all on public.service_windows    from anon, authenticated, public;
revoke all on public.meal_food_mapping  from anon, authenticated, public;
revoke all on public.meal_menu_template from anon, authenticated, public;

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

-- The load-cell half, as a SECOND grant on the same table. Column grants
-- ACCUMULATE, so this adds to the list above rather than replacing it -- which
-- is also what makes migrate_loadcell.sql idempotent, since re-granting is a
-- no-op.
--
-- No policy change accompanies it: device_status_update_device is row-scoped
-- `using (true) with check (true)`, and policies scope ROWS while grants scope
-- COLUMNS, so columns added later are already covered.
--
-- `grant select (device_id)` above deliberately stays at ONE column. A scale
-- cannot read its own weight back any more than a stack can read its own bowl
-- count, and smoke assertion 27 is what proves that stayed true.
grant update (weight_g, weight_state, cells_online, counts_per_gram, net_counts)
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

-- The weekly template is staff configuration like the menu itself.
grant select, insert, update, delete on public.meal_menu_template to authenticated;

revoke all on function public.tg_device_status_stamp()   from public, anon, authenticated;
revoke all on function public.tg_status_events_stamp()   from public, anon, authenticated;
revoke all on function public.tg_devices_create_status() from public, anon, authenticated;
revoke all on function public.tg_meal_food_mapping_touch() from public, anon, authenticated;
revoke all on function public.in_service_window(timestamptz, text, text, interval)
  from public, anon;
revoke all on function public.offline_after() from public, anon;
revoke all on function public.current_meal_type(text, timestamptz) from public, anon;
revoke all on function public.current_meal_date(text, timestamptz) from public, anon;
revoke all on function public.meal_mapping_preload(text, text, date) from public, anon;
revoke all on function public.last_service_window_start(text, text, timestamptz)
  from public, anon;
revoke all on function public.last_service_window_end(text, text, timestamptz)
  from public, anon;
revoke all on function public.meal_template_apply(text, date, date, boolean)
  from public, anon;
grant execute on function public.in_service_window(timestamptz, text, text, interval)
  to authenticated;
grant execute on function public.last_service_window_start(text, text, timestamptz)
  to authenticated;
grant execute on function public.last_service_window_end(text, text, timestamptz)
  to authenticated;
grant execute on function public.offline_after() to authenticated;
grant execute on function public.current_meal_type(text, timestamptz) to authenticated;
grant execute on function public.current_meal_date(text, timestamptz) to authenticated;
grant execute on function public.meal_mapping_preload(text, text, date) to authenticated;
grant execute on function public.meal_template_apply(text, date, date, boolean)
  to authenticated;

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
        and s.updated_at < now() - public.offline_after()) as offline,

       -- Registered but never heard from -- i.e. awaiting installation.
       (not coalesce(s.reported, false)) as awaiting_deployment,

       -- Outside service hours these numbers are the last known state from the
       -- previous service, not live data. Say so rather than letting a
       -- coordinator read a stale count as current.
       (not public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at is not null) as data_is_stale,

       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_level, s.charging,
       s.uptime_s, s.firmware, s.mac,

       -- Slept through the most recently completed service window. Unlike
       -- `offline` this survives the dark hours: a device dead since Tuesday
       -- stays flagged on Friday morning, instead of becoming
       -- indistinguishable from a healthy unit between meals. Appended LAST
       -- so the live database's CREATE OR REPLACE VIEW (which cannot reorder
       -- columns) and this rebuild agree exactly. Coalesced because a NULL
       -- anchor (no applicable window at all) must read "not flagged".
       --
       -- GATED ON DEPLOYMENT: a unit parked at 'R' or stripped of its slot
       -- (the swap-a-failed-board workflow) keeps reported = true with a
       -- frozen updated_at forever, and without the gate it would carry a
       -- permanent fleet-wide alarm only reset_spares.sql could clear. A
       -- unit that serves no position has no service to miss.
       --
       -- KNOWN LIMITS, both deliberate: a site holiday flags every deployed
       -- device until the next served meal; and a device that died MIDWAY
       -- through a window is unflagged from that window's close to the next
       -- window's open -- anchoring on the window END instead would
       -- false-alarm every time staff power a station down early. It was
       -- red while its window ran (`offline`) and goes red again the moment
       -- the next one opens.
       coalesce(coalesce(s.reported, false)
                and d.location in ('D','M','T')
                and d.food_slot is not null
                and s.updated_at <
                    public.last_service_window_end(d.timezone, d.device_id)
                      - public.offline_after(),
                false) as missed_last_service,

       -- The load-cell half, appended in one block below everything above --
       -- so the diff against the pre-load-cell body is purely additive, a
       -- reader can see at a glance which half describes which product, and
       -- the live database's CREATE OR REPLACE VIEW (which may only ADD
       -- columns at the end) and this rebuild agree exactly.
       d.kind,
       s.weight_g,
       s.weight_state,
       s.cells_online,
       s.counts_per_gram,
       s.net_counts
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
       count(*) filter (where d.kind = 'stack')                as devices,
       count(*) filter (where d.kind = 'stack'
                          and coalesce(s.reported, false))     as devices_reported,

       (count(*) filter (where d.kind = 'stack') * 4)::bigint  as bowls_capacity,

       sum(s.stack_count) filter (where s.stack_status = 'ok') as bowls_trusted,
       sum(s.stack_count)                                      as bowls_reported,

       bool_or(s.stack_status = 'discontiguous')               as any_fault,
       bool_or(s.stack_status = 'degraded')                    as any_degraded,
       -- Battery and liveness are NOT filtered by kind. A load cell has a
       -- battery and can go dark exactly like a stack, and a slot whose scale
       -- is flat is a slot that needs somebody -- so these keep meaning "any
       -- device here", which is what they already said.
       bool_or(s.battery_level in ('low','critical'))          as any_battery_warn,
       bool_or(coalesce(s.reported, false)
               and public.in_service_window(now(), d.timezone, d.device_id)
               and s.updated_at < now() - public.offline_after()) as any_offline,
       min(s.updated_at)                                       as oldest_update,

       bool_or(coalesce(coalesce(s.reported, false)
               and d.location in ('D','M','T')
               and d.food_slot is not null
               and s.updated_at <
                   public.last_service_window_end(d.timezone, d.device_id)
                     - public.offline_after(),
               false))                                         as any_missed_service,

       -- Appended: what the scales at this position say. NULL rather than 0
       -- when none of them has a usable weight -- sum() over an empty filter
       -- is NULL, which is the answer we want.
       count(*) filter (where d.kind = 'scale')                as scales,
       count(*) filter (where d.kind = 'scale'
                          and s.weight_state = 'ok')           as scales_ok,
       sum(s.weight_g) filter (where d.kind = 'scale'
                                 and s.weight_state = 'ok')    as measured_weight_g,
       -- Why any scale here is NOT contributing a weight, so the screen can
       -- name the fault instead of showing a silently smaller total. 'ok' is
       -- removed because it is not an issue.
       array_remove(array_agg(distinct s.weight_state)
                    filter (where d.kind = 'scale'), 'ok')     as scale_issues,

       -- THE SAME ARITHMETIC slot_quantity DOES, one grouping down. Stock is
       -- per HALL per position where Master is per position across halls, and
       -- both screens must answer "how much food is here" with the same
       -- number -- so the sum is computed once per row here rather than in the
       -- client, which is the rule slot_overview already follows for bowls.
       max(m.bowl_weight_g)                                    as bowl_weight_g,
       (sum(s.stack_count) filter (where s.stack_status = 'ok'))
         * max(m.bowl_weight_g)                                as buffer_g,
       -- coalesce per TERM, never on the sum: a position with a counter and no
       -- valued buffer has a real total, and so does the reverse. NULL only
       -- when neither term exists -- which for this view means the dish has no
       -- per-bowl weight AND no scale is reporting, and the card falls back to
       -- showing bowls.
       case when max(m.bowl_weight_g) is null
                 and sum(s.weight_g) filter (where d.kind = 'scale'
                                               and s.weight_state = 'ok') is null
            then null
            else coalesce((sum(s.stack_count) filter (where s.stack_status = 'ok'))
                            * max(m.bowl_weight_g), 0)
               + coalesce(sum(s.weight_g) filter (where d.kind = 'scale'
                                                    and s.weight_state = 'ok'), 0)
       end                                                     as weight_g
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

-- ---------------------------------------------------------------------
--  last_served_meal -- which meal's menu describes what is on the stations
--  RIGHT NOW, including the ~16 h a day when nothing is being served.
--
--  current_meal_type() answers NULL outside every window, which is correct
--  for "is a meal running" and wrong for "what is that food". The bowls are
--  still physically there between meals -- Stock keeps showing their count --
--  but with a NULL meal the menu join finds nothing, so the Master Dashboard
--  lost every dish name and every weight the moment dinner ended, and read
--  "12 bowls, no weight set" for two thirds of the day.
--
--  So this returns the most recently STARTED window instead of the currently
--  open one. During service that IS the current meal, so nothing changes;
--  outside service it is the meal that just finished, which is exactly the
--  food still sitting on the counters.
--
--  It deliberately looks BACKWARD only. Tomorrow's menu is usually entered a
--  day ahead, and picking it up would print tomorrow's dish over tonight's
--  leftovers -- a confident, wrong answer rather than a missing one.
--
--  Fleet-default windows only (device_id is null), matching
--  current_meal_type(). A per-device window override says when one station is
--  awake; it must not change which meal's MENU the site is reading, because
--  meal_food_mapping is keyed by location and has no idea devices exist.
-- ---------------------------------------------------------------------
create or replace function public.last_served_meal(
  tz    text        default 'Asia/Kolkata',
  at_ts timestamptz default now()
) returns table (meal_date date, meal_type text)
language sql stable set search_path = '' as $$
  with local_day as (
    select (at_ts at time zone coalesce(tz, 'UTC'))::date as d
  ),
  -- Today and yesterday. Yesterday is what carries the small hours: at 05:00
  -- the most recent meal is last night's dinner, and looking only at today
  -- would answer nothing at all.
  candidates as (
    select (ld.d - n)                as m_date,
           initcap(w.label)          as m_type,
           ((ld.d - n) + w.starts_at) at time zone coalesce(tz, 'UTC') as started_at
      from public.service_windows w
     cross join local_day ld
     cross join generate_series(0, 1) n
     where w.device_id is null
  )
  select c.m_date, c.m_type
    from candidates c
   where c.started_at <= at_ts
   order by c.started_at desc
   limit 1
$$;

revoke all on function public.last_served_meal(text, timestamptz) from public, anon;
grant execute on function public.last_served_meal(text, timestamptz) to authenticated;

-- ---------------------------------------------------------------------
--  weight_mismatch_tolerance -- one threshold, one place.
--
--  Modelled on offline_after(), for the same reason: a number that decides
--  whether the dashboard raises a flag belongs in exactly one object where it
--  can be found and changed, not inline in a view and again in the client.
--
--  WHAT A MISMATCH MEANS. Where a dish position has BOTH a load cell and bowl
--  counters with a per-bowl weight typed in, there are two independent answers
--  to one question. Agreement is worth nothing; DISAGREEMENT is the signal, and
--  it has exactly two causes worth chasing -- a miscalibrated cell, or a wrong
--  per-bowl weight in the menu. Neither is visible from either number alone,
--  and neither shows up as a fault anywhere else in this schema.
--
--  A PAIR, because a single test misfires at one end or the other: at 2 kg a
--  25% band is 500 g and any half-full bowl trips it, while at 80 kg a flat
--  2 kg band trips on rounding. The rule is "outside BOTH".
--
--  2000 g is roughly the lightest thing anybody stacks in these bowls, so a
--  difference under one bowl's worth cannot distinguish a calibration error
--  from an integer bowl count. 0.25 is one bowl in four, about the resolution a
--  bowl count has when the menu weight is typed by hand.
-- ---------------------------------------------------------------------
create or replace function public.weight_mismatch_tolerance(
  out abs_g integer, out frac numeric)
returns record
language sql immutable
set search_path = ''
as $$ select 2000, 0.25::numeric $$;

comment on function public.weight_mismatch_tolerance() is
  'How far a measured weight and a bowls-times-weight estimate may differ '
  'before the dashboard calls it a disagreement. A difference must exceed '
  'BOTH the absolute band and the fractional one -- a single percentage '
  'misfires on light slots and a single gram figure misfires on heavy ones.';

-- REVOKED FROM PUBLIC FIRST, like every other function in this schema. A fresh
-- CREATE FUNCTION carries Postgres's default of EXECUTE for PUBLIC, so a bare
-- GRANT to authenticated ADDS a privilege without removing that one -- and this
-- was, briefly, the only function here anon could call.
--
-- It returns two constants and leaks nothing, which is exactly why it is worth
-- fixing rather than excusing: the value of a uniform posture is that the one
-- exception does not have to be reasoned about, and "harmless" is the argument
-- that erodes it.
revoke all on function public.weight_mismatch_tolerance() from public, anon;
grant execute on function public.weight_mismatch_tolerance() to authenticated;

-- ---------------------------------------------------------------------
-- 9b. public.slot_quantity -- the Master Dashboard's source.
--
-- WHY THIS IS GROUPED BY food_slot ALONE
-- --------------------------------------
-- slot_overview answers "how much rice is left at Darshanarthi position 1".
-- This answers a different question -- "how much dal is left in the building"
-- -- because a slot NUMBER means the same dish across all three areas: slot 1
-- is dal at Darshanarthi, at Mahatma and at Tiffin alike. The kitchen orders,
-- cooks and refills against that site-wide figure, not against one hall's
-- share of it.
--
-- A NEW VIEW rather than a widened slot_overview. The Stock screen is the one
-- people watch through a service; changing the view underneath it to serve a
-- second screen's grouping would put both at risk for no gain, and PostgREST
-- has no trouble with two views over the same tables.
--
-- SUM-THEN-MULTIPLY, NOT MULTIPLY-THEN-SUM
-- ----------------------------------------
-- The obvious formula is (total bowls across the slot) x (one weight). It is
-- wrong whenever the three areas do NOT serve the same dish at that position
-- -- which happens: slot 3 is Curry at Darshanarthi, Sabzi at Mahatma, Bhaji
-- at Tiffin. Those bowls do not weigh the same, so there is no single weight
-- to multiply by.
--
-- So each area is weighed against ITS OWN dish first, and the products are
-- summed:
--
--     slot_total = SUM over areas ( bowls(area, slot) x weight(area, slot) )
--
-- When all three areas agree -- the normal case -- this reduces exactly to
-- total bowls x one weight. When they diverge it stays correct instead of
-- silently multiplying Tiffin's bowls by Darshanarthi's dish. It also means
-- the view never has to choose which area's weight "wins", a decision that
-- would have been arbitrary and invisible.
--
-- WHICH SLOTS APPEAR
-- ------------------
-- A slot appears once at least one of its devices has EVER reported. That
-- hides the backup units parked at slot 5, which are registered and assigned
-- but have never been powered on -- rendering them as a permanent "no data"
-- row would train people to ignore a row that means something real when a
-- deployed slot goes quiet.
--
-- It deliberately gates on `reported` (has ever spoken) and NOT on current
-- liveness. A slot whose devices all died mid-service keeps its row, carrying
-- its offline flags and its last known figure -- vanishing at exactly the
-- moment something breaks is the one behaviour a stock screen must not have.
-- The gate is the same one device_overview.offline uses, for the same reason.
--
-- security_invoker is mandatory: without it the view runs as its owner and
-- silently bypasses every policy in section 7.
-- ---------------------------------------------------------------------
create view public.slot_quantity
with (security_invoker = true) as
with per_area as (
  select d.location,
         d.food_slot,
         max(m.food_name)                                        as food_name,
         max(m.bowl_weight_g)                                    as bowl_weight_g,
         max(d.timezone)                                         as timezone,
         max(lm.meal_date)                                       as menu_meal_date,
         max(lm.meal_type)                                       as menu_meal_type,

         -- STACKS ONLY -- note A. A load cell bolted at this position is not a
         -- fourth bowl counter, and four more bowls of capacity is exactly
         -- what counting it would claim.
         count(*) filter (where d.kind = 'stack')                as devices,
         bool_or(coalesce(s.reported, false))                    as any_reported,
         (count(*) filter (where d.kind = 'stack') * 4)::bigint  as bowls_capacity,

         -- Trusted only: a degraded stack's count is a lower bound and a
         -- discontiguous one is not a count at all. Folding either into a
         -- WEIGHT would dress an unreliable number up as kilograms, which
         -- reads far more precise than it is.
         sum(s.stack_count) filter (where s.stack_status = 'ok') as bowls_trusted,

         -- What the load cells here actually weighed. NULL, never 0, when none
         -- has a usable figure -- sum() over an empty filter is NULL, which is
         -- the answer we want. weight_state = 'ok' is the whole gate: an
         -- uncalibrated or partly-dead scale reports its state and no grams,
         -- and must never contribute a silently low total.
         sum(s.weight_g) filter (where d.kind = 'scale'
                                   and s.weight_state = 'ok')    as measured_weight_g,
         count(*) filter (where d.kind = 'scale')                as scales,
         count(*) filter (where d.kind = 'scale'
                            and s.weight_state = 'ok')           as scales_ok,
         -- `weight_state is not null` is part of the FILTER, not an
         -- afterthought: array_agg(distinct ...) keeps a NULL, and
         -- array_remove strips only 'ok' -- so a scale that has never reported
         -- would put a NULL element in this array. That is not merely untidy;
         -- see the note on slot_issues below for what an extra element used to
         -- cost.
         array_remove(array_agg(distinct s.weight_state)
                      filter (where d.kind = 'scale'
                                and s.weight_state is not null), 'ok')
                                                                 as scale_issues,

         bool_or(s.stack_status = 'discontiguous')               as any_fault,
         bool_or(s.stack_status = 'degraded')                    as any_degraded,
         bool_or(s.battery_level in ('low','critical'))          as any_battery_warn,
         bool_or(coalesce(s.reported, false)
             and public.in_service_window(now(), d.timezone, d.device_id)
             and s.updated_at < now() - public.offline_after())  as any_offline,
         bool_or(coalesce(coalesce(s.reported, false)
                 and d.location in ('D','M','T')
                 and d.food_slot is not null
                 and s.updated_at <
                     public.last_service_window_end(d.timezone, d.device_id)
                       - public.offline_after(),
                 false))                                         as any_missed_service,
         min(s.updated_at)                                       as oldest_update
    from public.devices d
    left join public.device_status s using (device_id)
    left join lateral public.last_served_meal(d.timezone) lm on true
    left join public.meal_food_mapping m
           on m.location  = d.location
          and m.food_slot = d.food_slot
          and m.meal_date = lm.meal_date
          and m.meal_type = lm.meal_type
   where d.location is not null
     and d.food_slot is not null
     and d.location in ('D','M','T')
   group by d.location, d.food_slot
),
-- The derivations, one level up, because every input to them is an aggregate
-- of the CTE above and a SELECT cannot reference its own output aliases.
--
-- TWO ESTIMATES, NOT ONE, and the difference is not redundancy. The SLOT total
-- treats a silent hall as contributing nothing (coalesce to 0) while the
-- hall's own LINE shows a dash, because "no reading" is not "an empty
-- counter". slot_quantity already drew that distinction before load cells
-- existed, and the precedence rule has to preserve both halves of it.
per_area_w as (
  select p.*,
         case when p.bowl_weight_g is not null
              then coalesce(p.bowls_trusted, 0)::bigint * p.bowl_weight_g
         end                                                     as est_total_g,
         case when p.bowl_weight_g is not null
               and p.bowls_trusted is not null
              then p.bowls_trusted::bigint * p.bowl_weight_g
         end                                                     as est_line_g,
         -- ADDED, NOT CHOSEN, and this replaced a precedence rule that was
         -- simply wrong about the hardware.
         --
         -- The two terms are DIFFERENT FOOD IN DIFFERENT PLACES: the stack
         -- counts bowls held in reserve, the platform weighs what is on the
         -- serving counter. Neither substitutes for the other, so picking one
         -- discards the other.
         --
         -- Measured on the live database the evening this was found: D/1 held
         -- two buffered bowls at 18 kg each and an empty counter, and Master
         -- reported 18.0 kg for the slot instead of 54.0 kg -- because a scale
         -- was present and reading zero, so "measured beats estimated" threw
         -- away 36 kg of real food. A kitchen reading that number orders more
         -- rice it already has.
         --
         -- NULL only when NEITHER term exists. coalesce per TERM rather than
         -- on the sum, so a hall with a counter and no valued buffer still has
         -- a total, and so does the reverse.
         case when p.measured_weight_g is null and p.bowl_weight_g is null
              then null
              else coalesce(p.measured_weight_g, 0)
                 + coalesce(case when p.bowl_weight_g is not null
                                 then coalesce(p.bowls_trusted, 0)::bigint * p.bowl_weight_g
                            end, 0)
         end                                                     as weight_total_g,
         case when p.measured_weight_g is null
               and (p.bowl_weight_g is null or p.bowls_trusted is null)
              then null
              else coalesce(p.measured_weight_g, 0)
                 + coalesce(case when p.bowl_weight_g is not null
                                  and p.bowls_trusted is not null
                                 then p.bowls_trusted::bigint * p.bowl_weight_g
                            end, 0)
         end                                                     as weight_line_g
    from per_area p
),
-- THE MISMATCH CHECK IS GONE, and its removal is the point worth recording.
--
-- It compared each hall's measured counter weight against its bowls-times-
-- weight estimate and flagged a disagreement as a miscalibrated cell or a
-- wrong menu figure. That reasoning assumed the two describe the SAME food.
-- They do not: the stack counts bowls held in reserve and the platform weighs
-- what is on the serving counter, so they are different food in different
-- places and a difference between them carries no information at all.
--
-- What it actually produced, live: D/1 held two buffered bowls at 18 kg and an
-- empty counter, and the dashboard announced "Darshanarthi's scale disagrees
-- by 36.0 kg" about two instruments that were both perfectly correct.
--
-- A genuine cross-check between the two is possible and is a different
-- calculation entirely -- it would compare bowls REMOVED from the buffer
-- against grams ADDED to the counter over the same interval, which does
-- describe one movement of one lot of food. That needs the derivative of both
-- series, not their levels, and it is not attempted here.
-- THE DISTINCT REASONS, AGGREGATED ONCE PER SLOT AND JOINED ONCE.
--
-- This used to be `left join lateral unnest(p.scale_issues)` inside per_slot,
-- which is a row multiplier: a hall with two distinct non-ok states produced
-- TWO copies of that hall's row, and every sum in the group counted it twice.
-- Measured on a three-hall slot with one uncalibrated and one silent scale at
-- Darshanarthi: devices 6 -> 10, bowls_capacity 24 -> 40, bowls_trusted 4 -> 8.
--
-- The bowl count doubling is the part that matters. It is the number the
-- kitchen reads, it stayed internally consistent, and nothing about it looked
-- wrong -- the slot simply appeared to be holding twice the food it had,
-- whenever two of its scales were unhappy in two different ways.
slot_issues as (
  select p.food_slot,
         array_agg(distinct i order by i) as scale_issues
    from per_area_w p
   cross join lateral unnest(p.scale_issues) as i
   group by p.food_slot
),
per_slot as (
  select p.food_slot,

         public.current_meal_type(max(p.timezone))               as current_meal,
         max(p.menu_meal_date)                                   as menu_meal_date,
         max(p.menu_meal_type)                                   as menu_meal_type,
         coalesce(max(p.menu_meal_type)
                    = public.current_meal_type(max(p.timezone))
                  and max(p.menu_meal_date)
                    = public.current_meal_date(max(p.timezone)), false)
                                                                 as menu_is_live,

         array_agg(distinct p.food_name)
           filter (where p.food_name is not null)                as dishes,

         (case when count(distinct p.bowl_weight_g) = 1
               then max(p.bowl_weight_g) end)                    as bowl_weight_g,

         sum(p.devices)                                          as devices,
         sum(p.bowls_capacity)                                   as bowls_capacity,
         sum(p.bowls_trusted)                                    as bowls_trusted,

         sum(p.est_total_g)                                      as est_weight_g,
         sum(p.bowls_capacity * p.bowl_weight_g)
           filter (where p.bowl_weight_g is not null)            as capacity_weight_g,

         -- A LOWER BOUND -- but only where a weight is actually OWED.
         --
         -- THREE CONDITIONS, and the third was missing. A hall is short a
         -- weight when it holds bowls, cannot weigh them, AND IS SERVING A
         -- DISH. Without that last clause a hall that is simply not serving
         -- this meal got counted as unweighed: breakfast runs at Darshanarthi
         -- only, so Mahatma held a bowl with no dish against it, and the slot
         -- reported ">=36.4 kg" with a "Set weight" link pointing at a hall
         -- where there is nothing to set a weight FOR.
         --
         -- The bound was wrong as well as the prompt. ">=" claims there is
         -- more food than shown; a hall with no dish is not holding
         -- unmeasured food, so the figure was exact and said it was not.
         --
         -- food_name IS the test for "is this hall serving": it comes from
         -- meal_food_mapping through last_served_meal(), which is the same
         -- resolution the Menu tab writes and the dish names above are read
         -- from. A hall with a dish and no kg/bowl still prompts, which is the
         -- case the prompt exists for.
         bool_or(p.weight_total_g is null
                 and coalesce(p.bowls_trusted, 0) > 0
                 and p.food_name is not null)                    as est_is_partial,
         array_remove(array_agg(
           case when p.weight_total_g is null
                 and coalesce(p.bowls_trusted, 0) > 0
                 and p.food_name is not null
                then p.location end), null)                      as areas_without_weight,

         jsonb_agg(jsonb_build_object(
           'location',       p.location,
           'food_name',      p.food_name,
           'bowl_weight_g',  p.bowl_weight_g,
           'bowls_trusted',  p.bowls_trusted,
           'bowls_capacity', p.bowls_capacity,
           'devices',        p.devices,
           -- THE HALL'S OWN MASS, and it is now the precedence answer rather
           -- than the estimate it used to be. Same key, better number: a hall
           -- with a load cell shows what was weighed there. Still NULL when the
           -- hall has neither a measurement nor a reading to multiply -- never
           -- 0, which beside a bowl count of "--" is a contradiction that sends
           -- somebody to refill a station nobody has heard from.
           'weight_g',          p.weight_line_g,
           'est_weight_g',      p.est_line_g,
           'measured_weight_g', p.measured_weight_g,
           'scales',            p.scales,
           'capacity_weight_g', case when p.bowl_weight_g is not null
                                     then p.bowls_capacity * p.bowl_weight_g end
         ) order by p.location)                                  as areas,

         bool_or(p.any_fault)                                    as any_fault,
         bool_or(p.any_degraded)                                 as any_degraded,
         bool_or(p.any_battery_warn)                             as any_battery_warn,
         bool_or(p.any_offline)                                  as any_offline,
         bool_or(p.any_missed_service)                           as any_missed_service,
         min(p.oldest_update)                                    as oldest_update,

         sum(p.scales)                                           as scales,
         sum(p.scales_ok)                                        as scales_ok,
         sum(p.measured_weight_g)                                as measured_weight_g,
         sum(p.weight_total_g)                                   as weight_g,


         -- The two pools, kept separate as well as summed, so a screen can
         -- say "36 kg buffered, nothing on the counter" rather than only the
         -- total. weight_g above is their sum.
         sum(p.est_total_g)                                      as buffer_g,
         sum(p.measured_weight_g)                                as counter_g
    from per_area_w p
   group by p.food_slot
  having bool_or(p.any_reported)
)
select q.food_slot,
       q.current_meal,
       q.menu_meal_date,
       q.menu_meal_type,
       q.menu_is_live,
       q.dishes,
       q.bowl_weight_g,
       q.devices,
       q.bowls_capacity,
       q.bowls_trusted,
       q.est_weight_g,
       q.capacity_weight_g,
       q.est_is_partial,
       q.areas_without_weight,
       q.areas,
       q.any_fault,
       q.any_degraded,
       q.any_battery_warn,
       q.any_offline,
       q.any_missed_service,
       q.oldest_update,

       -- APPENDED, so CREATE OR REPLACE VIEW is legal against a live database:
       -- it may add columns at the end and may not reorder or retype the ones
       -- above.
       q.scales,
       q.scales_ok,
       q.measured_weight_g,
       q.weight_g,
       q.buffer_g,
       q.counter_g,
       -- LEFT joined, one row per slot, so it cannot multiply anything. NULL
       -- when every scale here is healthy, which is the common case.
       si.scale_issues
  from per_slot q
  left join slot_issues si on si.food_slot = q.food_slot
 order by q.food_slot;


comment on view public.slot_quantity is
  'Master Dashboard source: quantity per dish position across every serving '
  'area, in grams. weight_g is the authoritative figure and is a SUM: '
  'buffer_g (bowls waiting, counted x per-bowl weight) plus counter_g (food '
  'on the scales), because those are different food in different places. It '
  'was once measured-beats-estimated per area, which discarded a hall''s '
  'whole buffer whenever a scale there happened to read empty. est_weight_g '
  'and measured_weight_g are the two inputs, kept so a screen can say which '
  'part it is showing.';

revoke all on public.device_overview from anon, authenticated, public;
revoke all on public.slot_overview   from anon, authenticated, public;
revoke all on public.slot_quantity   from anon, authenticated, public;
grant select on public.device_overview to authenticated;
grant select on public.slot_overview   to authenticated;
grant select on public.slot_quantity   to authenticated;

commit;

-- ---------------------------------------------------------------------
-- 10. Next steps. Run these as separate files, in this order:
--       register_devices.sql    BWL-001 .. BWL-032
--       assign_devices.sql      the permanent location/food_slot assignment
--       seed_meal_mapping.sql   sample menus, for the front-end test bed
--       reset_spares.sql        restores awaiting_deployment for the reserved 8
--       smoke_test.sql          32 assertions; expect ALL PASS
--
--     Then, for the load cells at the serving counter:
--       apply_loadcell.sql      all four load-cell migrations in ONE
--                               self-verifying transaction -- it snapshots
--                               every device, status row and slot_overview
--                               figure first and RAISES if one moved
--     or the four separately: migrate_loadcell.sql, register_loadcells.sql,
--     assign_loadcells.sql, migrate_weight_samples.sql, migrate_burn_rate.sql
--
--     whats_installed.sql says which of those a database already has.
-- ---------------------------------------------------------------------

-- ---------------------------------------------------------------------
-- 11. Retention (optional; enable pg_cron under Database > Extensions).
--     ~30 devices x ~50 changes/day is roughly 550k rows/year, ~170 MB.
-- ---------------------------------------------------------------------
-- select cron.schedule('bowlstack-retention', '17 3 * * *',
--   $$delete from public.status_events
--      where recorded_at < now() - interval '180 days'$$);
