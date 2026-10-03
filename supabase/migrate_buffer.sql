-- =====================================================================
--  Migration -- the buffer platforms. BWL-xxx stop counting bowls and
--  start weighing them.
--
--  Run as owner in the Supabase SQL editor, AFTER apply_loadcell.sql (or
--  its parts up to and including migrate_burn_rate.sql). Idempotent and
--  ADDITIVE: it widens two constraints, adds three nullable columns to
--  device_status and to weight_samples, adds two guard triggers and rewrites
--  the stock views. No table is dropped and no row is rewritten.
--
--  NO BOWL FIGURE MOVES WHEN THIS COMMITS. Every BWL is still
--  kind = 'stack' afterwards, so the buffer term it adds is empty, and
--  apply_loadcell.sql's check #2 proves the bowl columns unchanged. The
--  change becomes visible at supabase/cutover_buffers.sql, which is a
--  separate file on purpose -- see ROLLOUT below. Three things DO change at
--  once, each a correction (measured on a seeded copy of the live shape):
--
--    * slot_overview.weight_g reads NULL, not 0, at a position where nothing
--      is known -- a menu kg-per-bowl and no trusted count, no ok scale.
--    * slot_overview.scale_issues no longer holds a NULL element for a scale
--      that never reported (section 9, note E).
--    * slot_stock_series stops carrying a scale's last ok weight forward once
--      its newest sample is not ok (section 12), so the run-out line can move
--      while a counter is settling or dead -- and says partial while it does.
--
--  Reversible: supabase/rollback_buffer.sql (after
--  rollback_cutover_buffers.sql, if the cut-over has run).
--
--  schema.sql carries the same end state for a from-scratch rebuild, and
--  weekly_menu_and_offline.sql carries copies of device_overview and
--  slot_overview that have been updated in step. All three must agree.
--
--  WHY
--  ---
--  The buffer area was measured by BWL-xxx ToF stacks reporting a bowl count
--  0-4, and its kilograms were only ever ESTIMATED: bowls x a kg-per-bowl
--  somebody typed on the Menu tab. Each stack is being replaced by a 200 kg
--  load cell under the buffer platform, which measures the food instead of
--  inferring it. The ids stay BWL-xxx; what they MEASURE changes, and
--  devices.kind is exactly the column that says what a device measures.
--
--  THE BUFFER CONTRACT, which the firmware, the simulator and the dashboard
--  were all written against:
--
--    weight_g         FOOD, in grams: gross - bowls x 2500. Always food,
--                     whatever the panel's display switch says. NULL unless
--                     weight_state = 'ok' (the existing honesty CHECK).
--    gross_g          everything on the platform. NULL unless ok.
--    bowls            0..8 (4 is the real maximum). NULL unless ok.
--    bowls_confirmed  false = remembered across a power cycle, or in doubt.
--                     NULL exactly when bowls is NULL.
--    cells_online     0 or 1 -- one cell per platform.
--
--  A buffer never sends stack_*, never inserts status_events. A scale (the
--  counter, LDC-xxx) never sends bowls/bowls_confirmed/gross_g and keeps its
--  100 kg sanity rail. A stack never sends any of the weight columns.
--
--  WHAT THIS ADDS
--  --------------
--    1. devices.kind                 -- 'buffer' joins 'stack' and 'scale'
--    2. weight_g range               -- 250 kg on both tables; the counter's
--                                       100 kg moves into the guard, per kind
--    3. bowls, bowls_confirmed,      -- on device_status AND weight_samples,
--       gross_g                         with named CHECKs
--    4. the anon grants              -- extended, never SELECT
--    5. tg_weight_samples_kind       -- accepts 'buffer'
--    6. device_status_kind trigger   -- which columns a kind may write
--    7. status_events_kind trigger   -- the guard that table never had
--    8. device_overview              -- the three new columns, appended
--    9. slot_overview                -- measured buffer kg; NULL, never 0
--   10. slot_quantity                -- the same, across halls
--   11. device_burn_rate             -- covers buffers
--   12. slot_stock_series,           -- the buffer series from weight_samples
--       slot_burn_rate
--
--  ROLLOUT -- four steps, in this order, and the order is the design:
--
--    1. this file            additive; every BWL still 'stack'
--    2. push the web         it must read BOTH shapes -- before and after 3
--    3. cutover_buffers.sql  re-kinds every BWL-% to 'buffer' and NULLs its
--                            stack_* in the same transaction
--    4. the kg fleet sim     only now does anything post buffer weights;
--                            before step 3 the guard below refuses them
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 0. Prerequisites.
--
-- Checked BEFORE anything is written, so an out-of-order run leaves the
-- database exactly as it found it and names the file to run. Section 12
-- drops and recreates the burn-rate views, whose bodies read
-- burn_rate_tuning() and weight_samples; without these checks the file
-- would get that far and abort on a symptom.
-- ---------------------------------------------------------------------
do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'devices'
                    and column_name = 'kind') then
    raise exception
      'Run supabase/apply_loadcell.sql FIRST -- devices.kind does not exist '
      'yet (migrate_loadcell.sql adds it). Nothing has been changed.';
  end if;
  if to_regclass('public.weight_samples') is null then
    raise exception
      'Run supabase/apply_loadcell.sql FIRST -- weight_samples does not exist '
      'yet (migrate_weight_samples.sql adds it). Nothing has been changed.';
  end if;
  if to_regprocedure('public.burn_rate_tuning()') is null then
    raise exception
      'Run supabase/apply_loadcell.sql FIRST -- burn_rate_tuning() does not '
      'exist yet (migrate_burn_rate.sql adds it), and this file rewrites the '
      'views that read it. Nothing has been changed.';
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. devices.kind -- 'buffer'.
--
-- A THIRD KIND, NOT A SECOND KIND OF SCALE. A buffer and the counter are the
-- same converter under different platforms, but they answer different
-- questions -- food held in reserve versus food on the serving line -- and
-- the stock views must keep the two pools apart to say "36 kg buffered,
-- nothing on the counter". A buffer also carries bowls and gross_g, which a
-- counter never does, and has a 250 kg range where the counter has 100.
--
-- The default stays 'stack'. Nothing registers a buffer from scratch --
-- cutover_buffers.sql re-kinds the existing BWL rows -- and every older
-- script that inserts a device without naming its kind (register_devices,
-- the smoke fixtures) keeps meaning exactly what it meant.
--
-- Same constraint NAME, so migrate_loadcell.sql's
-- `add constraint ... exception when duplicate_object` stays a no-op on a
-- re-run of apply_loadcell.sql rather than re-adding the narrow one.
-- ---------------------------------------------------------------------
alter table public.devices
  drop constraint if exists devices_kind_ck,
  add  constraint devices_kind_ck check (kind in ('stack','scale','buffer'));

comment on column public.devices.kind is
  'What this installation MEASURES. stack = four ToF sensors up a pipe, '
  'reporting a bowl count; scale = the serving-counter load cell (LDC), '
  'reporting grams; buffer = the 200 kg buffer platform (BWL), reporting '
  'grams of FOOD plus the bowls it stands in. Fixed at registration or by '
  'cutover_buffers.sql, never a setting. The views use it to decide which '
  'columns of device_status are a measurement and which are absent, and '
  'device_status_kind refuses a write to the wrong half.';

-- ---------------------------------------------------------------------
-- 2. weight_g -- 250 kg, on both tables.
--
-- A 200 kg cell under a full buffer reads 190 kg of food with headroom
-- above it; 100 kg -- three 20 kg counter cells' worth -- would refuse a
-- perfectly good reading. And a refusal here is not a refused WEIGHT, it is
-- a 400 that fails the whole PATCH or POST, so the station would go silent
-- including the state that explains why.
--
-- THE COUNTER KEEPS ITS 100 kg RAIL, but in the guard trigger (section 6),
-- per kind, rather than in this CHECK. A table-wide CHECK cannot see
-- devices.kind; a guard can, and 100 kg on a 60 kg-rated counter is still
-- what identifies a units mix-up or a wild calibration factor there.
--
-- The negative floor is unchanged. A buffer's food is gross - bowls x 2500,
-- so a remembered bowl count that is too high can push it below zero. That
-- is COUPLED TO FIRMWARE the way battery_mv's 6000 is: include/load_scale.h
-- rails the food figure at the same -5000..250000 (railMinG / railMaxG) and
-- reports over_range with no weight outside it, so nothing the firmware
-- sends as 'ok' can fail this CHECK. Change one, change the other.
-- ---------------------------------------------------------------------
alter table public.device_status
  drop constraint if exists device_status_weight_range_ck,
  add  constraint device_status_weight_range_ck
       check (weight_g is null or weight_g between -5000 and 250000);

-- weight_samples declared its range INLINE, so Postgres named it
-- (weight_samples_weight_g_check, on every database seen so far). Found by
-- what it constrains and what it says rather than by that name: a database
-- whose table was created by a different statement could have another one,
-- and guessing wrong would leave the 100 kg limit in place and make every
-- buffer history POST a 400 that the firmware retries forever.
do $$
declare r record;
begin
  for r in
    select c.conname
      from pg_constraint c
     where c.conrelid = 'public.weight_samples'::regclass
       and c.contype  = 'c'
       and c.conname <> 'weight_samples_weight_range_ck'
       and c.conkey   = array[(select a.attnum from pg_attribute a
                                where a.attrelid = 'public.weight_samples'::regclass
                                  and a.attname  = 'weight_g')]
       and pg_get_constraintdef(c.oid) like '%100000%'
  loop
    execute format('alter table public.weight_samples drop constraint %I',
                   r.conname);
  end loop;
end $$;

alter table public.weight_samples
  drop constraint if exists weight_samples_weight_range_ck,
  add  constraint weight_samples_weight_range_ck
       check (weight_g is null or weight_g between -5000 and 250000);

-- ---------------------------------------------------------------------
-- 3. bowls, bowls_confirmed, gross_g.
--
-- ON BOTH TABLES, because the burn rate is computed from history and a
-- history that cannot say how many bowls were on the platform cannot tell a
-- bowl being carried to the counter from food being eaten.
--
-- WEIGHT_G STAYS THE FOOD, AND IS ALWAYS THE FOOD. Every view sums weight_g
-- as food; if a buffer published gross there whenever its panel showed
-- gross, every slot would be overstated by 2.5 kg per bowl whenever somebody
-- flipped a display switch. gross_g is its own column so the meaning of
-- weight_g never depends on a setting.
--
-- NULL UNLESS ok, the same honesty rule weight_g already obeys. A bowl count
-- from a dead or unsettled cell is the firmware's memory, not a reading; a
-- count of 3 beside 'no_cells' would put bowls on the dashboard that nothing
-- measured. 'is not distinct from' because device_status.weight_state is
-- NULL before a device first reports, and a CHECK that evaluates to NULL
-- passes -- `weight_state = 'ok'` alone would wave bowls through on a row
-- that has never spoken.
--
-- 0..8 IS HEADROOM, NOT THE LIMIT. Four bowls fit a platform today and the
-- firmware clamps there; eight keeps a reconfigured maximum from turning
-- into a 400 and a silent station.
--
-- gross_g's ceiling is the food ceiling plus four bowls of dish mass
-- (250 kg + 4 x 2.5 kg), so any gross the firmware can pair with an
-- in-range food figure is accepted.
-- ---------------------------------------------------------------------
alter table public.device_status
  add column if not exists bowls           smallint,
  add column if not exists bowls_confirmed boolean,
  add column if not exists gross_g         integer;

alter table public.weight_samples
  add column if not exists bowls           smallint,
  add column if not exists bowls_confirmed boolean,
  add column if not exists gross_g         integer;

-- Dropped and re-added rather than guarded with IF NOT EXISTS, so a re-run
-- converges on THESE definitions even if an earlier draft of one was applied.
alter table public.device_status
  drop constraint if exists device_status_bowls_range_ck,
  drop constraint if exists device_status_bowls_confirmed_ck,
  drop constraint if exists device_status_gross_range_ck,
  drop constraint if exists device_status_bowls_ok_ck,
  add  constraint device_status_bowls_range_ck
       check (bowls is null or bowls between 0 and 8),
  add  constraint device_status_bowls_confirmed_ck
       check ((bowls is null) = (bowls_confirmed is null)),
  add  constraint device_status_gross_range_ck
       check (gross_g is null or gross_g between -5000 and 260000),
  add  constraint device_status_bowls_ok_ck
       check ((bowls is null and gross_g is null)
              or weight_state is not distinct from 'ok');

alter table public.weight_samples
  drop constraint if exists weight_samples_bowls_range_ck,
  drop constraint if exists weight_samples_bowls_confirmed_ck,
  drop constraint if exists weight_samples_gross_range_ck,
  drop constraint if exists weight_samples_bowls_ok_ck,
  add  constraint weight_samples_bowls_range_ck
       check (bowls is null or bowls between 0 and 8),
  add  constraint weight_samples_bowls_confirmed_ck
       check ((bowls is null) = (bowls_confirmed is null)),
  add  constraint weight_samples_gross_range_ck
       check (gross_g is null or gross_g between -5000 and 260000),
  add  constraint weight_samples_bowls_ok_ck
       check ((bowls is null and gross_g is null)
              or weight_state is not distinct from 'ok');

comment on column public.device_status.weight_g is
  'What is on the platform, in grams, net of the platform zero and the '
  'session tare. On a buffer it is FOOD: gross_g less 2500 g per bowl, '
  'whatever the panel displays. Present ONLY when weight_state = ''ok'' -- '
  'enforced by device_status_weight_agrees_ck. NULL means no trustworthy '
  'weight; 0 means a measured, empty platform. Those are different facts and '
  'must not be collapsed.';

comment on column public.device_status.bowls is
  'BUFFER ONLY. Bowls standing on the platform, 0-8 (4 fit today). NULL '
  'unless weight_state = ''ok'', and always NULL on a scale or a stack. '
  'weight_g = gross_g - bowls x 2500 g.';
comment on column public.device_status.bowls_confirmed is
  'BUFFER ONLY. false when the bowl count is remembered from before a power '
  'cycle or is otherwise in doubt; true once a load/unload event or an empty '
  'platform has confirmed it. NULL exactly when bowls is NULL.';
comment on column public.device_status.gross_g is
  'BUFFER ONLY. Everything on the platform, bowls included, in grams. NULL '
  'unless weight_state = ''ok''. weight_g, not this, is the food.';

comment on column public.weight_samples.bowls is
  'BUFFER ONLY. As device_status.bowls, at the moment of the sample.';
comment on column public.weight_samples.bowls_confirmed is
  'BUFFER ONLY. As device_status.bowls_confirmed, at the moment of the sample.';
comment on column public.weight_samples.gross_g is
  'BUFFER ONLY. As device_status.gross_g, at the moment of the sample.';

-- ---------------------------------------------------------------------
-- 4. The anon write path -- extended, not replaced.
--
-- Column grants ACCUMULATE, so these add to the existing lists and a re-run
-- is a no-op. `grant select (device_id)` on device_status deliberately
-- stays at one column, and weight_samples keeps NO select at all: a buffer
-- can no more read its own weight back than a counter can. apply_loadcell's
-- check #5 and smoke assertion 28 are what prove it stayed that way.
-- ---------------------------------------------------------------------
grant update (bowls, bowls_confirmed, gross_g)
  on public.device_status to anon;
grant insert (bowls, bowls_confirmed, gross_g)
  on public.weight_samples to anon;

-- ---------------------------------------------------------------------
-- 5. tg_weight_samples_kind -- accepts buffers.
--
-- Same function, same trigger, same SECURITY DEFINER reasoning as
-- migrate_weight_samples.sql section 3: it reads public.devices on behalf of
-- anon, which cannot. Two additions for a scale, because this is the only
-- place that can see the kind of the row being inserted:
--
--   * the 100 kg rail the table CHECK gave up in section 2, and
--   * no bowls / gross_g -- a counter has neither, and a counter row that
--     carried them would be a buffer row filed under the wrong kind.
--
-- An unregistered id still raises here, as it always has; this file does not
-- change what the counter firmware sees for that case.
-- ---------------------------------------------------------------------
create or replace function public.tg_weight_samples_kind()
returns trigger language plpgsql security definer set search_path = '' as $$
declare v_kind text;
begin
  select kind into v_kind from public.devices where device_id = new.device_id;
  if v_kind is distinct from 'scale' and v_kind is distinct from 'buffer' then
    raise exception
      'weight_samples accepts only devices.kind = ''scale'' or ''buffer''; % is %',
      new.device_id, coalesce(v_kind, 'not registered')
      using errcode = 'check_violation';
  end if;
  if v_kind = 'scale' then
    if new.bowls is not null or new.bowls_confirmed is not null
       or new.gross_g is not null then
      raise exception
        '% is a scale: it has no bowls and no gross_g -- those are buffer columns',
        new.device_id using errcode = 'check_violation';
    end if;
    if new.weight_g > 100000 then
      raise exception
        '% is a scale reporting % g; above 100 kg on the counter is a units '
        'mix-up or a wild calibration factor, not food', new.device_id, new.weight_g
        using errcode = 'check_violation';
    end if;
  end if;
  return new;
end $$;

revoke all on function public.tg_weight_samples_kind() from public, anon, authenticated;

comment on function public.tg_weight_samples_kind() is
  'Guard on weight_samples: only scales and buffers append here; a scale '
  'carries no bowls/gross_g and stays under 100 kg. SECURITY DEFINER because '
  'it reads devices, which anon cannot.';

-- ---------------------------------------------------------------------
-- 6. device_status_kind -- which half of the row a device may write.
--
-- One row shape serves three products, and until now nothing stopped a
-- device writing the wrong half: the anon grant is per COLUMN, the policy
-- per ROW, and neither knows what the device is. That mattered little while
-- the halves were disjoint by firmware. It matters now because the SAME ids
-- change product at the cut-over: a ToF board or a stack simulator still
-- pointed at BWL-007 after it became a buffer would write a bowl count into
-- a row the views read as kilograms, and nothing would look wrong.
--
-- ONLY COLUMNS THAT CHANGED TO A NON-NULL VALUE ARE TESTED. In a BEFORE
-- UPDATE trigger NEW carries every column the PATCH did not mention at its
-- OLD value -- so testing NEW alone would reject every buffer PATCH on a row
-- that still held its last stack_count. cutover_buffers.sql NULLs those
-- anyway; this is what keeps the guard from depending on that.
--
-- NAMED TO FIRE FIRST. Row triggers of one event fire in name order, and
-- 'device_status_kind' sorts before 'device_status_stamp', so a refused
-- write raises before the stamp could mark the row as reported.
--
-- SECURITY DEFINER for the reason tg_weight_samples_kind gives: it reads
-- devices on behalf of anon, which has no SELECT there -- and without it
-- this guard would reject EVERY device PATCH with 42501.
--
-- 23514 (check_violation), so a refusal reads like the CHECKs beside it: a
-- 400 that the firmware drops rather than retries.
-- ---------------------------------------------------------------------
create or replace function public.tg_device_status_kind()
returns trigger language plpgsql security definer set search_path = '' as $$
declare v_kind text;
begin
  select kind into v_kind from public.devices where device_id = new.device_id;

  if v_kind = 'stack' then
    if (new.weight_g        is not null and new.weight_g        is distinct from old.weight_g)
    or (new.weight_state    is not null and new.weight_state    is distinct from old.weight_state)
    or (new.cells_online    is not null and new.cells_online    is distinct from old.cells_online)
    or (new.counts_per_gram is not null and new.counts_per_gram is distinct from old.counts_per_gram)
    or (new.net_counts      is not null and new.net_counts      is distinct from old.net_counts)
    or (new.bowls           is not null and new.bowls           is distinct from old.bowls)
    or (new.bowls_confirmed is not null and new.bowls_confirmed is distinct from old.bowls_confirmed)
    or (new.gross_g         is not null and new.gross_g         is distinct from old.gross_g) then
      raise exception
        '% is a stack: it reports stack_* and never a weight, bowls or gross_g',
        new.device_id using errcode = 'check_violation';
    end if;

  elsif v_kind in ('scale','buffer') then
    if (new.stack_count    is not null and new.stack_count    is distinct from old.stack_count)
    or (new.stack_status   is not null and new.stack_status   is distinct from old.stack_status)
    or (new.levels         is not null and new.levels         is distinct from old.levels)
    or (new.sensors_ok     is not null and new.sensors_ok     is distinct from old.sensors_ok)
    or (new.sensors_online is not null and new.sensors_online is distinct from old.sensors_online) then
      raise exception
        '% is a %: it never reports stack_count, stack_status, levels, '
        'sensors_ok or sensors_online', new.device_id, v_kind
        using errcode = 'check_violation';
    end if;

    if v_kind = 'scale' then
      if (new.bowls           is not null and new.bowls           is distinct from old.bowls)
      or (new.bowls_confirmed is not null and new.bowls_confirmed is distinct from old.bowls_confirmed)
      or (new.gross_g         is not null and new.gross_g         is distinct from old.gross_g) then
        raise exception
          '% is a scale: it has no bowls and no gross_g -- those are buffer columns',
          new.device_id using errcode = 'check_violation';
      end if;
      -- The counter's 100 kg rail; the table CHECK is 250 kg for the buffers.
      if new.weight_g > 100000 and new.weight_g is distinct from old.weight_g then
        raise exception
          '% is a scale reporting % g; above 100 kg on the counter is a units '
          'mix-up or a wild calibration factor, not food', new.device_id, new.weight_g
          using errcode = 'check_violation';
      end if;
    end if;
  end if;

  return new;
end $$;

revoke all on function public.tg_device_status_kind() from public, anon, authenticated;

comment on function public.tg_device_status_kind() is
  'Guard on device_status: a stack writes only stack_*, a scale or buffer '
  'never writes stack_*, a scale never writes bowls/gross_g and stays under '
  '100 kg. Tests only columns that changed to a non-null value. SECURITY '
  'DEFINER because it reads devices, which anon cannot.';

drop trigger if exists device_status_kind on public.device_status;
create trigger device_status_kind
  before update on public.device_status
  for each row execute function public.tg_device_status_kind();

-- ---------------------------------------------------------------------
-- 7. status_events_kind -- the guard that table never had.
--
-- migrate_weight_samples.sql declined to add it: no scale could satisfy the
-- five bowl-shaped NOT NULLs, so nothing could reach it. A BWL that has
-- become a buffer CAN -- any stack firmware or simulator still pointed at
-- its id writes a perfectly well-formed bowl count -- and slot_stock_series
-- would then value that count at the menu kg-per-bowl beside the weights.
--
-- AN UNREGISTERED ID IS LET THROUGH, to the foreign key. That answers 23503,
-- which the firmware latches as "unprovisioned" and backs off from; raising
-- 23514 here instead would quietly turn the provisioning gate into an
-- ordinary rejection. Smoke assertion 8 holds that line.
-- ---------------------------------------------------------------------
create or replace function public.tg_status_events_kind()
returns trigger language plpgsql security definer set search_path = '' as $$
declare v_kind text;
begin
  select kind into v_kind from public.devices where device_id = new.device_id;
  if v_kind is not null and v_kind <> 'stack' then
    raise exception
      'status_events accepts only devices.kind = ''stack''; % is a %',
      new.device_id, v_kind
      using errcode = 'check_violation';
  end if;
  return new;
end $$;

revoke all on function public.tg_status_events_kind() from public, anon, authenticated;

comment on function public.tg_status_events_kind() is
  'Guard on status_events: only stacks append bowl history. An unregistered '
  'id passes through to the foreign key, which answers 23503.';

drop trigger if exists status_events_kind on public.status_events;
create trigger status_events_kind
  before insert on public.status_events
  for each row execute function public.tg_status_events_kind();

-- ---------------------------------------------------------------------
-- 8. device_overview -- the three new columns, APPENDED.
--
-- CREATE OR REPLACE VIEW may only add columns at the end, so they go after
-- net_counts, and schema.sql and weekly_menu_and_offline.sql say the same.
-- ---------------------------------------------------------------------
create or replace view public.device_overview
with (security_invoker = true) as
select d.device_id,
       d.location,
       d.food_slot,
       d.label,
       d.timezone,

       m.food_name                          as current_food,
       public.current_meal_type(d.timezone) as current_meal,

       s.reported,
       s.updated_at,
       now() - s.updated_at as stale_for,
       public.in_service_window(now(), d.timezone, d.device_id) as in_service,

       (coalesce(s.reported, false)
        and public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at < now() - public.offline_after()) as offline,

       (not coalesce(s.reported, false)) as awaiting_deployment,

       (not public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at is not null) as data_is_stale,

       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_level, s.charging,
       s.uptime_s, s.firmware, s.mac,

       coalesce(coalesce(s.reported, false)
                and d.location in ('D','M','T')
                and d.food_slot is not null
                and s.updated_at <
                    public.last_service_window_end(d.timezone, d.device_id)
                      - public.offline_after(),
                false) as missed_last_service,

       d.kind,
       s.weight_g,
       s.weight_state,
       s.cells_online,
       s.counts_per_gram,
       s.net_counts,

       -- The buffer half. NULL on every scale and stack, and on a buffer
       -- whenever weight_state is not 'ok' -- see section 3.
       s.bowls,
       s.bowls_confirmed,
       s.gross_g
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone);

-- ---------------------------------------------------------------------
-- 9. slot_overview -- the buffer measured, per hall per position.
--
-- A. EVERY STACK TERM IS NOW FILTERED ON kind = 'stack'. bowls_trusted,
--    bowls_reported, any_fault and any_degraded used to read every device
--    at the position, which was harmless while only stacks wrote stack_*.
--    A re-kinded BWL is the case that breaks it: if its last bowl count were
--    ever left in place, it would be counted as bowls AND weighed as food.
--
-- B. buffer_g IS NOW "WHAT IS IN THE BUFFER": the measured food on the
--    buffer platforms PLUS the old bowls x kg-per-bowl estimate from any
--    stack that is still a stack. The estimate goes dormant by itself once
--    every BWL is a buffer, and nothing has to be switched off.
--
-- C. weight_g IS NULL WHEN NOTHING IS KNOWN, NEVER 0. It used to be 0
--    whenever the dish had a per-bowl weight and no stack had a trusted
--    count -- "nothing measured" printed as "empty", which sends somebody to
--    refill a position nobody has heard from. Now it is NULL unless a
--    buffer or a scale here has an ok weight or a stack estimate exists. 0
--    is still a real weight: a tared, empty platform.
--
-- D. buffer_issues COUNTS ONLY BUFFERS THAT HAVE REPORTED A STATE, and that
--    state is not ok. A weighing platform always sends weight_state, so NULL
--    means it has never reported as one: registered and not yet fitted, or a
--    BWL re-kinded by the cut-over that has not weighed anything since. Both
--    are awaiting deployment, which device_overview already says. Counting
--    them as faults would mark a position partial FOREVER -- D/1 carries
--    BWL-002/003, which no panel feeds while only B1 is fitted, and every
--    position carries an LDC of which only LDC-001 exists.
--
-- E. scale_issues STOPS CARRYING A NULL for a scale that never reported, for
--    the same reason (slot_quantity always filtered it). So a hall's figure is
--    a lower bound exactly when it has one and buffer_issues > 0 or
--    scale_issues is non-empty -- and never because of `buffers_ok < buffers`
--    or `scales_ok < scales`, which count the unfitted platforms.
--
-- Rewritten as one aggregate CTE plus arithmetic over it, which CREATE OR
-- REPLACE allows -- only the output column list is fixed, and it is: the 22
-- columns it had, same names, same types, then six appended.
-- ---------------------------------------------------------------------
create or replace view public.slot_overview
with (security_invoker = true) as
with a as (
  select d.location,
         d.food_slot,
         max(m.food_name)                                        as current_food,
         public.current_meal_type(max(d.timezone))               as current_meal,
         count(*) filter (where d.kind = 'stack')                as devices,
         count(*) filter (where d.kind = 'stack'
                            and coalesce(s.reported, false))     as devices_reported,
         (count(*) filter (where d.kind = 'stack') * 4)::bigint  as bowls_capacity,
         sum(s.stack_count) filter (where d.kind = 'stack'
                                      and s.stack_status = 'ok') as bowls_trusted,
         sum(s.stack_count) filter (where d.kind = 'stack')      as bowls_reported,
         bool_or(s.stack_status = 'discontiguous')
           filter (where d.kind = 'stack')                       as any_fault,
         bool_or(s.stack_status = 'degraded')
           filter (where d.kind = 'stack')                       as any_degraded,
         -- Battery and liveness stay unfiltered: any device here that is flat
         -- or dark is a position that needs somebody, whatever it measures.
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
         count(*) filter (where d.kind = 'scale')                as scales,
         count(*) filter (where d.kind = 'scale'
                            and s.weight_state = 'ok')           as scales_ok,
         sum(s.weight_g) filter (where d.kind = 'scale'
                                   and s.weight_state = 'ok')    as measured_weight_g,
         -- `weight_state is not null` added, as slot_quantity has always had
         -- it: a scale that never reported put a NULL element here, so
         -- "scale_issues is non-empty" read as a fault at every position
         -- with an unfitted LDC -- which is all of them but D/1. Now an
         -- element is always a reported, non-ok state, and a hall is partial
         -- exactly when this or buffer_issues is non-empty.
         array_remove(array_agg(distinct s.weight_state)
                      filter (where d.kind = 'scale'
                                and s.weight_state is not null), 'ok')
                                                                 as scale_issues,
         max(m.bowl_weight_g)                                    as bowl_weight_g,
         -- The stack estimate, exactly as before apart from the kind filter:
         -- NULL with no trusted count or no per-bowl weight.
         (sum(s.stack_count) filter (where d.kind = 'stack'
                                       and s.stack_status = 'ok'))
           * max(m.bowl_weight_g)                                as stack_est_g,
         count(*) filter (where d.kind = 'buffer')               as buffers,
         count(*) filter (where d.kind = 'buffer'
                            and s.weight_state = 'ok')           as buffers_ok,
         -- NULL, never 0, when no buffer here has a usable weight: sum() over
         -- an empty filter is NULL, which is the answer wanted.
         sum(s.weight_g) filter (where d.kind = 'buffer'
                                   and s.weight_state = 'ok')    as buffer_measured_g,
         sum(s.bowls)    filter (where d.kind = 'buffer'
                                   and s.weight_state = 'ok')    as buffer_bowls,
         coalesce(bool_or(not s.bowls_confirmed)
                    filter (where d.kind = 'buffer'
                              and s.weight_state = 'ok'), false) as buffer_unconfirmed,
         count(*) filter (where d.kind = 'buffer'
                            and s.weight_state <> 'ok')          as buffer_issues
    from public.devices d
    left join public.device_status s using (device_id)
    left join public.meal_food_mapping m
           on m.location  = d.location
          and m.food_slot = d.food_slot
          and m.meal_date = public.current_meal_date(d.timezone)
          and m.meal_type = public.current_meal_type(d.timezone)
   where d.location is not null
     and d.food_slot is not null
   group by d.location, d.food_slot
),
-- coalesce per TERM, never on the sum: a position with an ok buffer and no
-- stack estimate has a real buffer figure, and so does the reverse.
b as (
  select a.*,
         case when a.buffer_measured_g is null and a.stack_est_g is null then null
              else coalesce(a.buffer_measured_g, 0) + coalesce(a.stack_est_g, 0)
         end                                                     as buffer_g
    from a
)
select b.location,
       b.food_slot,
       b.current_food,
       b.current_meal,
       b.devices,
       b.devices_reported,
       b.bowls_capacity,
       b.bowls_trusted,
       b.bowls_reported,
       b.any_fault,
       b.any_degraded,
       b.any_battery_warn,
       b.any_offline,
       b.oldest_update,
       b.any_missed_service,
       b.scales,
       b.scales_ok,
       b.measured_weight_g,
       b.scale_issues,
       b.bowl_weight_g,
       b.buffer_g,
       -- Buffer plus counter, NULL only when neither is known -- note C.
       case when b.buffer_g is null and b.measured_weight_g is null then null
            else coalesce(b.buffer_g, 0) + coalesce(b.measured_weight_g, 0)
       end                                                       as weight_g,

       -- Appended: what the buffer platforms at this position say.
       b.buffers,
       b.buffers_ok,
       b.buffer_measured_g,
       b.buffer_bowls,
       b.buffer_unconfirmed,
       b.buffer_issues
  from b;

-- ---------------------------------------------------------------------
-- 10. slot_quantity -- the same arithmetic, per slot across halls.
--
-- Note A of section 9 applies to bowls_trusted, any_fault and any_degraded
-- here too; they are filtered on kind = 'stack'.
--
-- THE BUFFER TERM PER HALL is measured buffer food plus the stack estimate,
-- and the estimate now exists only where a STACK does (devices > 0). Before,
-- a hall with a per-bowl weight and no stack at all got an estimate of 0 --
-- harmless beside a reporting stack, and exactly wrong once the stacks are
-- gone: it would make every hall "known, and empty".
--
-- THE SLOT IS PARTIAL (weight_is_partial) when it has a figure and some
-- buffer or scale there has reported a state that is not ok -- the figure
-- then omits food that is physically present, so it is a lower bound and the
-- dashboard prints ">=". A platform that has never reported a state does not
-- count, for the reason note D gives. est_is_partial (a hall with bowls and
-- no kg-per-bowl) is OR'd in.
--
-- DROPPED, not replaced, as migrate_loadcell.sql did: nothing depends on it,
-- and a drop lets the per-area CTEs change shape freely. The column list it
-- ends with is the old 28, same names and types, then six appended.
-- ---------------------------------------------------------------------
drop view if exists public.slot_quantity;

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

         count(*) filter (where d.kind = 'stack')                as devices,
         bool_or(coalesce(s.reported, false))                    as any_reported,
         (count(*) filter (where d.kind = 'stack') * 4)::bigint  as bowls_capacity,
         sum(s.stack_count) filter (where d.kind = 'stack'
                                      and s.stack_status = 'ok') as bowls_trusted,

         sum(s.weight_g) filter (where d.kind = 'scale'
                                   and s.weight_state = 'ok')    as measured_weight_g,
         count(*) filter (where d.kind = 'scale')                as scales,
         count(*) filter (where d.kind = 'scale'
                            and s.weight_state = 'ok')           as scales_ok,
         -- `weight_state is not null` stays in the FILTER: array_agg keeps a
         -- NULL and array_remove strips only 'ok'.
         array_remove(array_agg(distinct s.weight_state)
                      filter (where d.kind = 'scale'
                                and s.weight_state is not null), 'ok')
                                                                 as scale_issues,

         count(*) filter (where d.kind = 'buffer')               as buffers,
         count(*) filter (where d.kind = 'buffer'
                            and s.weight_state = 'ok')           as buffers_ok,
         sum(s.weight_g) filter (where d.kind = 'buffer'
                                   and s.weight_state = 'ok')    as buffer_measured_g,
         sum(s.bowls)    filter (where d.kind = 'buffer'
                                   and s.weight_state = 'ok')    as buffer_bowls,
         coalesce(bool_or(not s.bowls_confirmed)
                    filter (where d.kind = 'buffer'
                              and s.weight_state = 'ok'), false) as buffer_unconfirmed,
         -- Weighing platforms that have reported a state and it is not ok --
         -- what makes the slot figure a lower bound. Note D of section 9.
         count(*) filter (where d.kind in ('scale','buffer')
                            and s.weight_state <> 'ok')          as weighers_not_ok,

         bool_or(s.stack_status = 'discontiguous')
           filter (where d.kind = 'stack')                       as any_fault,
         bool_or(s.stack_status = 'degraded')
           filter (where d.kind = 'stack')                       as any_degraded,
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
-- The derivations, one level up because every input is an aggregate above.
--
-- TWO OF EACH, as before: the _total_ figures feed the SLOT sum and treat a
-- silent stack hall as contributing 0, while the _line_ figures are the
-- hall's own row and show NULL for "no reading" -- because a dash beside a
-- hall is not "an empty counter". A measured buffer is the same in both.
--
-- Each term is coalesced on its own and the sum is NULL only when every
-- term is unknown -- a hall with a counter and no buffer still has a total,
-- and so does the reverse.
per_area_w as (
  select p.*,
         e.est_total_g, e.est_line_g,
         b.buffer_total_g, b.buffer_line_g,
         case when b.buffer_total_g is null and p.measured_weight_g is null then null
              else coalesce(b.buffer_total_g, 0) + coalesce(p.measured_weight_g, 0)
         end                                                     as weight_total_g,
         case when b.buffer_line_g is null and p.measured_weight_g is null then null
              else coalesce(b.buffer_line_g, 0) + coalesce(p.measured_weight_g, 0)
         end                                                     as weight_line_g
    from per_area p
   cross join lateral (
     select case when p.devices > 0 and p.bowl_weight_g is not null
                 then coalesce(p.bowls_trusted, 0)::bigint * p.bowl_weight_g
            end                                                  as est_total_g,
            case when p.bowl_weight_g is not null
                  and p.bowls_trusted is not null
                 then p.bowls_trusted::bigint * p.bowl_weight_g
            end                                                  as est_line_g
   ) e
   cross join lateral (
     select case when p.buffer_measured_g is null and e.est_total_g is null then null
                 else coalesce(p.buffer_measured_g, 0) + coalesce(e.est_total_g, 0)
            end                                                  as buffer_total_g,
            case when p.buffer_measured_g is null and e.est_line_g is null then null
                 else coalesce(p.buffer_measured_g, 0) + coalesce(e.est_line_g, 0)
            end                                                  as buffer_line_g
   ) b
),
-- One row per slot, joined once, so it cannot multiply anything. See the
-- note in migrate_loadcell.sql section 7 for what a lateral unnest here did.
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

         -- A hall short a per-bowl weight: it holds stack bowls, cannot value
         -- them, and is serving a dish. Unchanged; see migrate_loadcell.sql.
         bool_or(p.weight_total_g is null
                 and coalesce(p.bowls_trusted, 0) > 0
                 and p.food_name is not null)                    as est_is_partial,
         array_remove(array_agg(
           case when p.weight_total_g is null
                 and coalesce(p.bowls_trusted, 0) > 0
                 and p.food_name is not null
                then p.location end), null)                      as areas_without_weight,

         jsonb_agg(jsonb_build_object(
           'location',           p.location,
           'food_name',          p.food_name,
           'bowl_weight_g',      p.bowl_weight_g,
           'bowls_trusted',      p.bowls_trusted,
           'bowls_capacity',     p.bowls_capacity,
           'devices',            p.devices,
           -- The hall's own mass: buffer plus counter, NULL -- never 0 --
           -- when the hall has nothing to report.
           'weight_g',           p.weight_line_g,
           'est_weight_g',       p.est_line_g,
           'measured_weight_g',  p.measured_weight_g,
           'scales',             p.scales,
           'capacity_weight_g',  case when p.bowl_weight_g is not null
                                      then p.bowls_capacity * p.bowl_weight_g end,
           'buffers',            p.buffers,
           'buffers_ok',         p.buffers_ok,
           'buffer_g',           p.buffer_line_g,
           'buffer_bowls',       p.buffer_bowls,
           'buffer_unconfirmed', p.buffer_unconfirmed
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

         -- The two pools, kept apart as well as summed: buffer_g is what is
         -- held in reserve (measured, plus any stack estimate), counter_g is
         -- what is on the serving line. weight_g is their sum.
         sum(p.buffer_total_g)                                   as buffer_g,
         sum(p.measured_weight_g)                                as counter_g,

         sum(p.buffers)                                          as buffers,
         sum(p.buffers_ok)                                       as buffers_ok,
         sum(p.buffer_measured_g)                                as buffer_measured_g,
         sum(p.buffer_bowls)                                     as buffer_bowls,
         bool_or(p.buffer_unconfirmed)                           as buffer_unconfirmed,
         (sum(p.weight_total_g) is not null
            and sum(p.weighers_not_ok) > 0)
           or bool_or(p.weight_total_g is null
                      and coalesce(p.bowls_trusted, 0) > 0
                      and p.food_name is not null)               as weight_is_partial
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
       q.scales,
       q.scales_ok,
       q.measured_weight_g,
       q.weight_g,
       q.buffer_g,
       q.counter_g,
       si.scale_issues,

       -- APPENDED below the 28 that existed, so a client written against the
       -- old shape reads exactly what it read before.
       q.buffers,
       q.buffers_ok,
       q.buffer_measured_g,
       q.buffer_bowls,
       q.buffer_unconfirmed,
       q.weight_is_partial
  from per_slot q
  left join slot_issues si on si.food_slot = q.food_slot
 order by q.food_slot;

revoke all on public.slot_quantity from anon, authenticated, public;
grant select on public.slot_quantity to authenticated;

comment on view public.slot_quantity is
  'Master Dashboard source: quantity per dish position across every serving '
  'area, in grams. weight_g is a SUM: buffer_g (food held in reserve -- '
  'measured on the buffer platforms, plus bowls x per-bowl weight for any '
  'remaining ToF stack) plus counter_g (food on the serving-counter scales). '
  'NULL, never 0, when nothing at the slot has a figure; weight_is_partial '
  'marks a figure that omits a reported instrument with no usable weight.';

-- ---------------------------------------------------------------------
-- 11. device_burn_rate -- buffers too.
--
-- Same body; only the kind filter widens. On a buffer the per-device rate
-- includes bowls carried to the counter -- that is food leaving THIS
-- platform -- which is why the slot-level rate below sums buffer and counter
-- first and this one is the per-station diagnostic.
-- ---------------------------------------------------------------------
create or replace view public.device_burn_rate
with (security_invoker = true) as
with t as (select * from public.burn_rate_tuning()),
pts as (
  select w.device_id, w.recorded_at, w.weight_g
    from public.weight_samples w
   cross join t
   where w.weight_state = 'ok'
     and w.weight_g is not null
     and w.recorded_at >= now() - t.window_span
),
lagged as (
  select p.device_id,
         p.recorded_at,
         p.weight_g,
         lag(p.weight_g)    over w                            as prev_g,
         lag(p.recorded_at) over w                            as prev_at
    from pts p
  window w as (partition by p.device_id order by p.recorded_at)
),
steps as (
  select l.device_id,
         l.recorded_at,
         l.weight_g,
         l.prev_g,
         l.prev_at,
         count(*) filter (where l.weight_g - l.prev_g >= (select refill_g from t))
           over (partition by l.device_id
                 order by l.recorded_at
                 rows between unbounded preceding and current row) as segment
    from lagged l
),
seg as (
  select s.device_id,
         s.segment,
         count(*)                                             as n,
         min(s.recorded_at)                                   as seg_first_at,
         max(s.recorded_at)                                   as seg_last_at,
         (array_agg(s.weight_g order by s.recorded_at))[1]
           - (array_agg(s.weight_g order by s.recorded_at desc))[1] as seg_drop_g,
         extract(epoch from max(s.recorded_at) - min(s.recorded_at)) as seg_s
    from steps s
   group by s.device_id, s.segment
  having count(*) >= 2
),
agg as (
  select g.device_id,
         sum(g.n)                                             as samples,
         min(g.seg_first_at)                                  as first_at,
         max(g.seg_last_at)                                   as last_at,
         sum(greatest(g.seg_drop_g, 0))                       as consumed_g,
         sum(g.seg_s) filter (where g.seg_drop_g > 0)         as consuming_s,
         max(g.seg_first_at) filter (where g.segment > 0)     as last_refill_at
    from seg g
   group by g.device_id
)
select d.device_id,
       d.location,
       d.food_slot,
       a.samples,
       a.first_at,
       a.last_at,
       (a.last_at - a.first_at)                              as covered,
       a.consumed_g,
       a.last_refill_at,
       case when a.last_at - a.first_at >= (select min_span from t)
             and coalesce(a.consuming_s, 0) > 0
            then round((a.consumed_g / (a.consuming_s / 3600.0))::numeric, 0)
       end                                                   as g_per_hour,
       s.weight_g                                            as current_g,
       case when a.last_at - a.first_at >= (select min_span from t)
             and coalesce(a.consuming_s, 0) > 0
             and a.consumed_g > 0
             and s.weight_g is not null
            then make_interval(secs =>
                   (s.weight_g / (a.consumed_g / a.consuming_s))::double precision)
       end                                                   as runs_out_in,
       case when a.last_at - a.first_at >= (select min_span from t)
             and coalesce(a.consuming_s, 0) > 0
             and a.consumed_g > 0
             and s.weight_g is not null
            then now() + make_interval(secs =>
                   (s.weight_g / (a.consumed_g / a.consuming_s))::double precision)
       end                                                   as runs_out_at
  from agg a
  join public.devices d       on d.device_id = a.device_id
  left join public.device_status s on s.device_id = a.device_id
 where d.kind in ('scale','buffer');

revoke all on public.device_burn_rate from anon, authenticated, public;
grant select on public.device_burn_rate to authenticated;

comment on view public.device_burn_rate is
  'Consumption rate per weighing platform (counter scale or buffer) over the '
  'last hour: endpoints within segments split at refills, ok samples only. '
  'g_per_hour and runs_out_at are NULL when the series is too short to '
  'support them. Per-station diagnostic; the slot figure is slot_burn_rate.';

-- ---------------------------------------------------------------------
-- 12. slot_stock_series + slot_burn_rate -- the buffer series, measured.
--
-- THE BUFFER HALF NOW COMES FROM weight_samples of kind = 'buffer', beside
-- the counter, and the old status_events x kg-per-bowl half stays only for
-- devices that are still stacks -- dormant once none are.
--
-- NEWEST SAMPLE IN ANY STATE, AND ITS WEIGHT ONLY IF IT IS ok. The counter
-- half used to take the newest OK sample, which carried a weight forward
-- forever: a scale that dropped to no_cells kept contributing the last
-- figure it had, so the series showed food that nothing was measuring. Now
-- a platform that stops being ok stops contributing at that grid point, and
-- buffer_is_partial says the total is missing something.
--
-- buffer_is_partial THEREFORE COVERS THE COUNTER TOO. The name predates
-- this; the column is kept, not renamed, because slot_burn_rate and the
-- dashboard read it, and what it means to both is unchanged -- "this total
-- is not the whole stock, treat a rate over it accordingly".
--
-- DROPPED, not replaced, in dependency order, as migrate_burn_rate.sql does:
-- the per-instrument rows are now one UNION, and sum() over a bigint would
-- have retyped the columns. Grants and comments are restated because a
-- dropped view takes its privileges with it. Columns, names and types are
-- exactly the old eight.
--
-- RE-RUNNING migrate_burn_rate.sql ON ITS OWN WOULD SILENTLY RESTORE THE OLD
-- BODIES. apply_loadcell.sql runs this file after it, which is why this file
-- is the last of its parts.
-- ---------------------------------------------------------------------
drop view if exists public.slot_burn_rate;
drop view if exists public.slot_stock_series;

create view public.slot_stock_series
with (security_invoker = true) as
with t as (select * from public.burn_rate_tuning()),
grid as (
  select g as at_ts
    from t, generate_series(now() - t.window_span, now(), interval '5 minutes') g
),
-- The per-bowl weight in force per hall and slot, for the dormant stack
-- half only, and whether that hall is serving the slot at all.
wt as (
  select d.location, d.food_slot, max(m.bowl_weight_g) as bowl_weight_g,
         bool_or(m.food_name is not null)      as is_serving
    from public.devices d
   cross join lateral public.last_served_meal(d.timezone) lm
    left join public.meal_food_mapping m
           on m.location = d.location and m.food_slot = d.food_slot
          and m.meal_date = lm.meal_date and m.meal_type = lm.meal_type
   where d.location in ('D','M','T') and d.food_slot is not null
   group by d.location, d.food_slot
),
-- ONE ROW PER INSTRUMENT PER GRID POINT: what it contributed there, which
-- pool it belongs to, and whether its contribution is incomplete. Grams are
-- integer here so the sums below are bigint, as they always were.
parts as (
  -- Stacks: the last TRUSTED bowl count at or before the point, valued at the
  -- hall's per-bowl weight. Unchanged from migrate_burn_rate.sql.
  select g.at_ts, d.location, d.food_slot,
         true                                                  as is_buffer,
         e.stack_count * w.bowl_weight_g                       as grams,
         (w.bowl_weight_g is null and e.stack_count > 0
          and w.is_serving)                                    as partial,
         false                                                 as scale_ok
    from grid g
    join public.devices d on d.kind = 'stack'
                         and d.location in ('D','M','T')
                         and d.food_slot is not null
    join wt w on w.location = d.location and w.food_slot = d.food_slot
   cross join lateral (
     select ev.stack_count
       from public.status_events ev
      where ev.device_id = d.device_id
        and ev.recorded_at <= g.at_ts
        and ev.stack_status = 'ok'
      order by ev.recorded_at desc
      limit 1
   ) e
  union all
  -- Buffers and counter scales: the NEWEST sample at or before the point,
  -- whatever its state; weighed only if that sample is ok.
  select g.at_ts, d.location, d.food_slot,
         d.kind = 'buffer'                                     as is_buffer,
         case when ws.weight_state = 'ok' then ws.weight_g end as grams,
         ws.weight_state <> 'ok'                               as partial,
         d.kind = 'scale' and ws.weight_state = 'ok'           as scale_ok
    from grid g
    join public.devices d on d.kind in ('scale','buffer')
                         and d.location in ('D','M','T')
                         and d.food_slot is not null
   cross join lateral (
     select s.weight_state, s.weight_g
       from public.weight_samples s
      where s.device_id = d.device_id
        and s.recorded_at <= g.at_ts
      order by s.recorded_at desc
      limit 1
   ) ws
)
select p.at_ts,
       p.location,
       p.food_slot,
       sum(p.grams) filter (where p.is_buffer)                 as buffer_g,
       sum(p.grams) filter (where not p.is_buffer)             as counter_g,
       -- NULL only when no instrument contributes anything -- sum() skips
       -- NULLs, so this is buffer + counter with each term coalesced alone.
       sum(p.grams)                                            as total_g,
       coalesce(bool_or(p.partial), false)                     as buffer_is_partial,
       count(*) filter (where p.scale_ok)                      as scales
  from parts p
 group by p.at_ts, p.location, p.food_slot;

revoke all on public.slot_stock_series from anon, authenticated, public;
grant select on public.slot_stock_series to authenticated;

comment on view public.slot_stock_series is
  'Total food per hall and dish position over the last hour, on a 5-minute '
  'grid: the buffer (weighed on the buffer platforms, plus bowls x per-bowl '
  'weight for any remaining ToF stack) PLUS the serving counter (weighed). '
  'Each platform contributes its newest sample at each point, and only if '
  'that sample is ok; buffer_is_partial marks a point where one is not. '
  'Summing both is what makes a buffer-to-counter transfer read as flat.';

-- Recreated verbatim from migrate_burn_rate.sql section 4 -- dropped above
-- only because it reads slot_stock_series.
create view public.slot_burn_rate
with (security_invoker = true) as
with t as (select * from public.burn_rate_tuning()),
pts as (
  select * from public.slot_stock_series where total_g is not null
),
lagged as (
  select p.*, lag(p.total_g) over w as prev_g
    from pts p
  window w as (partition by p.location, p.food_slot order by p.at_ts)
),
steps as (
  select l.*,
         count(*) filter (where l.total_g - l.prev_g >= (select refill_g from t))
           over (partition by l.location, l.food_slot order by l.at_ts
                 rows between unbounded preceding and current row) as segment
    from lagged l
),
seg as (
  select s.location, s.food_slot, s.segment,
         min(s.at_ts) as seg_first_at, max(s.at_ts) as seg_last_at,
         (array_agg(s.total_g order by s.at_ts))[1]
           - (array_agg(s.total_g order by s.at_ts desc))[1] as drop_g,
         extract(epoch from max(s.at_ts) - min(s.at_ts))     as seg_s,
         bool_or(s.buffer_is_partial)                        as partial
    from steps s group by s.location, s.food_slot, s.segment having count(*) >= 2
),
agg as (
  select g.location, g.food_slot,
         min(g.seg_first_at)                            as first_at,
         max(g.seg_last_at)                             as last_at,
         sum(greatest(g.drop_g, 0))                     as consumed_g,
         sum(g.seg_s) filter (where g.drop_g > 0)       as consuming_s,
         max(g.seg_first_at) filter (where g.segment > 0) as last_delivery_at,
         bool_or(g.partial)                             as partial
    from seg g group by g.location, g.food_slot
)
select a.location,
       a.food_slot,
       (select array_agg(distinct m.food_name)
          from public.devices d
         cross join lateral public.last_served_meal(d.timezone) lm
          join public.meal_food_mapping m
            on m.location = d.location and m.food_slot = d.food_slot
           and m.meal_date = lm.meal_date and m.meal_type = lm.meal_type
         where d.food_slot = a.food_slot
           and d.location = a.location
           and m.food_name is not null)                  as dishes,
       (a.last_at - a.first_at)                         as covered,
       a.consumed_g                                     as consumed_g_window,
       n.buffer_g                                       as buffer_g,
       n.counter_g                                      as counter_g,
       n.total_g                                        as total_g,
       a.last_delivery_at,
       a.partial                                        as is_partial,
       case when a.last_at - a.first_at >= (select min_span from t)
             and coalesce(a.consuming_s,0) > 0
            then round((a.consumed_g / (a.consuming_s/3600.0))::numeric, 0)
       end                                              as g_per_hour,
       case when a.last_at - a.first_at >= (select min_span from t)
             and coalesce(a.consuming_s,0) > 0 and a.consumed_g > 0
             and n.total_g is not null
            then now() + make_interval(secs =>
                   (n.total_g / (a.consumed_g / a.consuming_s))::double precision)
       end                                              as runs_out_at,
       case when a.last_at - a.first_at >= (select min_span from t)
             and coalesce(a.consuming_s,0) > 0 and a.consumed_g > 0
             and n.total_g is not null
            then now() + make_interval(secs =>
                   (n.total_g / (a.consumed_g / a.consuming_s))::double precision)
                 < (select ((now() at time zone 'Asia/Kolkata')::date + w.ends_at)
                             at time zone 'Asia/Kolkata'
                      from public.service_windows w
                     where w.device_id is null
                       and (now() at time zone 'Asia/Kolkata')::time
                             between w.starts_at and w.ends_at
                     order by w.starts_at limit 1)
       end                                              as short_before_close
  from agg a
  join lateral (
    select s.buffer_g, s.counter_g, s.total_g
      from public.slot_stock_series s
     where s.location = a.location and s.food_slot = a.food_slot
       and s.total_g is not null
     order by s.at_ts desc limit 1
  ) n on true
;

revoke all on public.slot_burn_rate from anon, authenticated, public;
grant select on public.slot_burn_rate to authenticated;

comment on view public.slot_burn_rate is
  'Consumption per dish position, over the buffer PLUS the active counter '
  'combined -- so moving bowls from a buffer platform onto the counter reads '
  'as flat rather than as a serving. g_per_hour and runs_out_at are NULL when '
  'the series is too short to support them; is_partial marks a total that '
  'was missing some platform''s weight (or some hall''s per-bowl weight), '
  'which makes the rate unreliable in either direction.';

commit;

-- ---------------------------------------------------------------------
--  Verify. One result set.
-- ---------------------------------------------------------------------
select 'devices_kind_ck accepts buffer' as what,
       (select count(*) from pg_constraint
         where conname = 'devices_kind_ck'
           and pg_get_constraintdef(oid) like '%buffer%')::text as detail
union all
select 'new columns (device_status + weight_samples)',
       (select count(*) from information_schema.columns
         where table_schema = 'public'
           and table_name in ('device_status','weight_samples')
           and column_name in ('bowls','bowls_confirmed','gross_g'))::text
union all
select 'guard triggers',
       (select count(*) from pg_trigger
         where not tgisinternal
           and tgname in ('device_status_kind','status_events_kind',
                          'weight_samples_kind'))::text
union all
-- The one thing section 2 finds by search rather than by name. Non-zero means
-- a 100 kg weight_g CHECK survived on a live database, and every full-buffer
-- POST or PATCH will answer 400.
select 'weight_g CHECKs still capped at 100 kg',
       (select count(*) from pg_constraint
         where conrelid in ('public.device_status'::regclass,
                            'public.weight_samples'::regclass)
           and contype = 'c'
           and pg_get_constraintdef(oid) like '%weight_g%100000%')::text
union all
select 'anon SELECT columns on device_status',
       (select count(*) from information_schema.column_privileges
         where table_schema = 'public' and table_name = 'device_status'
           and grantee = 'anon' and privilege_type = 'SELECT')::text
union all
select 'devices by kind',
       (select string_agg(kind || ' ' || n, ', ' order by kind)
          from (select kind, count(*) as n from public.devices group by kind) k);
-- Expect: 1, 6, 3, 0, 1, and every BWL still counted as 'stack' -- nothing is
-- a buffer until cutover_buffers.sql runs.
