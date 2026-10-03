-- =====================================================================
--  Migration -- the area HUBS. Battery and mains belong to the ESP32 that
--  runs an area, not to the platforms it weighs; a platform reports the
--  health of its node instead.
--
--  Run as owner in the Supabase SQL editor, AFTER migrate_buffer.sql (and
--  so after everything apply_loadcell.sql fuses ahead of it). It is also
--  the LAST part of apply_loadcell.sql. Idempotent: a re-run registers
--  nothing twice, re-adds the same constraints and trims nothing, because
--  the guard it installs has kept every platform's power columns NULL.
--
--  Reversible: supabase/rollback_hubs.sql -- with one exception. Section 5
--  NULLs the battery and mains figures every scale and buffer row held, and
--  no rollback can put them back. They were ToF-era and simulator values, a
--  copy of a battery that is now reported where it lives; nothing reads
--  them as history, which is in weight_samples and is not touched.
--
--  schema.sql carries the same end state for a from-scratch rebuild (except
--  the three hub rows, which are data and are registered here), and
--  weekly_menu_and_offline.sql carries a copy of device_overview that has
--  been updated in step. All three must agree.
--
--  WHY
--  ---
--  One ESP32 runs each serving area -- D Darshanarthi, M Mahatma, T Tiffin
--  -- on mains with a small backup cell, and every platform in that area
--  hangs off it: the counter scale and the buffers today, RS485 ATtiny
--  nodes later. It has ONE battery. Reported on each platform's row, that
--  one cell read as four batteries, each of which could be "low" on its
--  own, and a platform cannot be charged or unplugged -- the hub can.
--
--  So the hub gets a row of its own, and a platform's row says what can go
--  wrong with a PLATFORM: no supply at the node, a noisy bus, a node that
--  has stopped answering, a zero that has drifted.
--
--  WHAT THIS CHANGES
--  -----------------
--    1. devices.kind               -- 'hub' joins stack, scale and buffer
--    2. HUB-D, HUB-M, HUB-T        -- registered, location D/M/T, no slot
--    3. supply_mv, checksum_errors,     -- platform node health on device_status,
--       responding, no_load_g         with named CHECKs; NULL = not measured
--    4. the anon grants            -- UPDATE on those four, still no SELECT
--    5. THE TRIM                   -- battery_mv, battery_level, charging and
--                                     external_power NULLed on every scale
--                                     and buffer row, WITHOUT stamping it
--    6. device_status_kind         -- a platform refuses power; a hub
--                                     refuses everything but power
--    7. device_overview            -- the four node-health columns, appended
--
--  A HUB'S ROW USES COLUMNS THAT ALREADY EXIST: boot_id, uptime_s, firmware,
--  mac, battery_mv, battery_level, charging (true = on its charger / mains
--  present, false = on its cell, NULL = unknown) and external_power. No new
--  hub column.
--
--  THE SLOT VIEWS ARE UNCHANGED, and need not change: a hub has no
--  food_slot, so no stock view sees it. Their any_battery_warn now sees no
--  platform battery at all -- after the cut-over it is false or NULL at
--  every position -- because battery is shown per hub, on device_overview
--  and device_power, rather than smeared across the dishes a hub serves.
--
--  ROLLOUT -- the order is the design, because section 6 makes a battery
--  written to a platform a 400 that fails the WHOLE PATCH:
--
--    1. stop every writer that puts battery_mv, battery_level, charging or
--       external_power on a platform id: the LDC/BWL firmware from before
--       the hub split (it sends battery_mv and external_power on EVERY
--       PATCH -- applied first, the counter and every buffer go SILENT,
--       weight included), and tools/fleet_sim.py, which posts battery for
--       the dummy buffers. Reflash, or stop them.
--    2. this file.
--    3. the hub firmware / simulator starts PATCHing HUB-D, HUB-M, HUB-T.
--       Before this file they do not exist, and a PATCH to them matches no
--       row -- which the firmware latches as "unprovisioned".
--
--  A BWL RE-KINDED LATER keeps whatever battery its stack row held: the
--  trim covers the scales and buffers that exist when it runs, and the
--  guard stops only NEW values. Run this file again after any later
--  cutover_buffers.sql -- the trim is what a re-run does.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 0. Prerequisites.
--
-- Checked BEFORE anything is written, so an out-of-order run leaves the
-- database exactly as it found it and names the file to run. Section 6
-- replaces migrate_buffer.sql's guard and reads external_power; section 7
-- appends to migrate_buffer.sql's device_overview. Without these checks the
-- file would get that far and abort on a symptom.
-- ---------------------------------------------------------------------
do $$
begin
  if to_regprocedure('public.tg_device_status_kind()') is null
     or not exists (select 1 from information_schema.columns
                     where table_schema = 'public' and table_name = 'device_status'
                       and column_name = 'bowls') then
    raise exception
      'Run supabase/migrate_buffer.sql FIRST (the last part of '
      'apply_loadcell.sql before this one) -- device_status_kind and '
      'device_status.bowls do not exist yet. Nothing has been changed.';
  end if;
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'device_status'
                    and column_name = 'external_power') then
    raise exception
      'Run supabase/migrate_vbus_sense.sql FIRST (part of apply_loadcell.sql) '
      '-- device_status.external_power does not exist yet, and a hub reports '
      'its mains there. Nothing has been changed.';
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. devices.kind -- 'hub'.
--
-- A FOURTH KIND, because a hub measures nothing on the line: no weight, no
-- bowls, no stack. What it has is the area's power, and kind is exactly the
-- column that says which half of device_status a device owns.
--
-- Same constraint NAME, so migrate_loadcell.sql's add-if-absent stays a
-- no-op and migrate_buffer.sql's widen-only re-add leaves this list alone
-- on a re-run of apply_loadcell.sql.
-- ---------------------------------------------------------------------
alter table public.devices
  drop constraint if exists devices_kind_ck,
  add  constraint devices_kind_ck
       check (kind in ('stack','scale','buffer','hub'));

comment on column public.devices.kind is
  'What this installation MEASURES. stack = four ToF sensors up a pipe, '
  'reporting a bowl count; scale = the serving-counter load cell (LDC), '
  'reporting grams; buffer = the 200 kg buffer platform (BWL), reporting '
  'grams of FOOD plus the bowls it stands in; hub = the ESP32 that runs a '
  'serving area (HUB-D/M/T), reporting only its own power and identity -- '
  'battery and mains belong to the hub, never to a platform. Fixed at '
  'registration or by cutover_buffers.sql, never a setting. The views use it '
  'to decide which columns of device_status are a measurement and which are '
  'absent, and device_status_kind refuses a write to the wrong half.';

-- ---------------------------------------------------------------------
-- 2. The three hubs.
--
-- LOCATION SET, food_slot NULL. The location is what keeps a hub out of
-- reset_spares.sql, which resets anything at 'R' or nowhere; the missing
-- slot is what keeps it out of every stock view, which group by position.
--
-- ON CONFLICT DO NOTHING, so a re-run never overwrites a label or a
-- location somebody has since edited on the dashboard. kind is the one
-- column forced, for the reason register_loadcells.sql gives: a row
-- inserted before the kind existed, or by hand without it, defaults to
-- 'stack', and a hub filed as a stack would be refused its own battery.
--
-- The devices_create_status trigger creates each hub's device_status row.
-- ---------------------------------------------------------------------
insert into public.devices (device_id, location, food_slot, label, timezone, kind)
values ('HUB-D', 'D', null, 'Darshanarthi hub', 'Asia/Kolkata', 'hub'),
       ('HUB-M', 'M', null, 'Mahatma hub',      'Asia/Kolkata', 'hub'),
       ('HUB-T', 'T', null, 'Tiffin hub',       'Asia/Kolkata', 'hub')
on conflict (device_id) do nothing;

update public.devices
   set kind = 'hub'
 where device_id in ('HUB-D','HUB-M','HUB-T')
   and kind is distinct from 'hub';

-- ---------------------------------------------------------------------
-- 3. Node health -- four columns a PLATFORM writes.
--
-- What can go wrong with a weighing platform that its weight_state does
-- not say: the node's supply sagging, a noisy bus corrupting frames, the
-- node no longer answering, and the empty-platform zero walking away. The
-- hub polls each node and PATCHes these onto that platform's own row.
--
-- ALL NULL = NOT MEASURED, NEVER 0, the rule weight_g keeps: today's
-- platforms are wired straight to the hub and measure none of these, and a
-- 0 checksum_errors or 0 g no_load beside them would claim a check nobody ran.
--
-- The ranges are wide on purpose -- a value the hub can send that a CHECK
-- refuses is a 400 that loses the weight in the same PATCH:
--
--   supply_mv   0..20000   a 12 V RS485 supply with surge headroom.
--                          smallint holds it.
--   checksum_errors  >= 0       a counter since the node powered up; it falls
--                          back to 0 when the node restarts, which is
--                          itself worth seeing beside uptime.
--   no_load_g   +/-50 kg   a zero that has drifted 50 kg is a broken cell,
--                          not drift; past that the number means nothing.
--
-- Constraints dropped and re-added rather than IF NOT EXISTS, so a re-run
-- converges on THESE definitions.
-- ---------------------------------------------------------------------
alter table public.device_status
  add column if not exists supply_mv  smallint,
  add column if not exists checksum_errors integer,
  add column if not exists responding boolean,
  add column if not exists no_load_g  integer;

alter table public.device_status
  drop constraint if exists device_status_supply_mv_ck,
  drop constraint if exists device_status_checksum_errors_ck,
  drop constraint if exists device_status_no_load_ck,
  add  constraint device_status_supply_mv_ck
       check (supply_mv is null or supply_mv between 0 and 20000),
  add  constraint device_status_checksum_errors_ck
       check (checksum_errors is null or checksum_errors >= 0),
  add  constraint device_status_no_load_ck
       check (no_load_g is null or no_load_g between -50000 and 50000);

comment on column public.device_status.supply_mv is
  'PLATFORM NODE HEALTH. Supply voltage at the platform''s node, measured by '
  'its own ADC, in mV (0..20000). Not a battery: a platform has none -- the '
  'area''s battery is on its hub''s row. NULL = not measured.';
comment on column public.device_status.checksum_errors is
  'PLATFORM NODE HEALTH. Frames from this node that failed their checksum '
  'since the node powered up (>= 0); back to 0 when the node restarts. '
  'NULL = not measured.';
comment on column public.device_status.responding is
  'PLATFORM NODE HEALTH. Whether the node answered the hub''s last health '
  'poll. false = asked and heard nothing; NULL = not polled.';
comment on column public.device_status.no_load_g is
  'PLATFORM NODE HEALTH. What the platform read the last time it was empty, '
  'in grams (-50000..50000). Should sit near 0; a slow walk away from 0 is '
  'zero drift, visible here before it corrupts weight_g. 0 is a real reading; '
  'NULL = not measured.';

-- ---------------------------------------------------------------------
-- 4. The anon write path -- extended, not replaced.
--
-- Column grants ACCUMULATE, so a re-run is a no-op, and SELECT stays at
-- device_id alone: the hub can no more read a platform's health back than
-- the platform can read its weight.
-- ---------------------------------------------------------------------
grant update (supply_mv, checksum_errors, responding, no_load_g)
  on public.device_status to anon;

-- ---------------------------------------------------------------------
-- 5. THE TRIM -- every scale and buffer stops carrying a battery.
--
-- The rows hold ToF-era figures (every BWL was a battery-powered stack
-- until the cut-over) and simulator values, and LDC-001's own reading. Left
-- in place they would sit under device_overview forever, beside a guard
-- that stops them ever changing -- a frozen "good" on a platform that has
-- no battery at all.
--
-- WITHOUT STAMPING THE ROWS, exactly as cutover_buffers.sql section 2 does
-- and for its reason: device_status_stamp sets updated_at := now() and
-- reported := true on every UPDATE, which would make every platform look as
-- if it had just reported -- a dead one live for forty seconds, its
-- missed_last_service cleared, a spare retired from "awaiting deployment".
-- None of that is a measurement. The stamp is off for this one statement,
-- inside this transaction, and back on before the commit; if anything
-- fails, the rollback restores it with everything else.
--
-- device_status_kind stays ON: it refuses only values changed to non-null,
-- and this sets NULL.
--
-- Only rows that HOLD something are touched, which is what makes a re-run
-- a no-op. Recorded first so the report can say how many.
-- ---------------------------------------------------------------------
drop table if exists _hubs_trim;
create temp table _hubs_trim as
  select s.device_id
    from public.device_status s
    join public.devices d using (device_id)
   where d.kind in ('scale','buffer')
     and (s.battery_mv is not null or s.battery_level is not null
          or s.charging is not null or s.external_power is not null);

alter table public.device_status disable trigger device_status_stamp;

update public.device_status s
   set battery_mv     = null,
       battery_level  = null,
       charging       = null,
       external_power = null
  from public.devices d
 where d.device_id = s.device_id
   and d.kind in ('scale','buffer')
   and (s.battery_mv is not null or s.battery_level is not null
        or s.charging is not null or s.external_power is not null);

alter table public.device_status enable trigger device_status_stamp;

-- ---------------------------------------------------------------------
-- 6. device_status_kind -- power belongs to the hub.
--
-- migrate_buffer.sql section 6's body with two additions; every existing
-- test is unchanged, and so is the changed-to-non-null rule, the
-- SECURITY DEFINER reasoning, the 23514 and the name that makes it fire
-- before device_status_stamp.
--
--   * A SCALE OR BUFFER refuses battery_mv, battery_level, charging and
--     external_power. A firmware from before the hub split sends them on
--     every PATCH, and without this the trim above would be undone within
--     twenty seconds by every platform still running it.
--
--   * A HUB refuses everything that is not power or identity: the weight
--     half, the buffer half, stack_* and the platform node health. A hub
--     writing a weight would put food on no dish -- it has no slot -- and
--     node health on the hub's row would be filed under the wrong device.
--     cells_online, counts_per_gram and net_counts are refused with the
--     rest of the weight half, as the stack branch already refuses them.
--
-- Explicit NULLs pass, which is what lets current firmware keep sending
-- `"battery_mv": null` to a platform and clear rather than fail.
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

    -- Power is the hub's (migrate_hubs.sql).
    if (new.battery_mv     is not null and new.battery_mv     is distinct from old.battery_mv)
    or (new.battery_level  is not null and new.battery_level  is distinct from old.battery_level)
    or (new.charging       is not null and new.charging       is distinct from old.charging)
    or (new.external_power is not null and new.external_power is distinct from old.external_power) then
      raise exception
        '% is a %: it has no battery -- battery_mv, battery_level, charging and '
        'external_power belong on its area''s hub (HUB-D/M/T)', new.device_id, v_kind
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

  elsif v_kind = 'hub' then
    if (new.weight_g        is not null and new.weight_g        is distinct from old.weight_g)
    or (new.weight_state    is not null and new.weight_state    is distinct from old.weight_state)
    or (new.cells_online    is not null and new.cells_online    is distinct from old.cells_online)
    or (new.counts_per_gram is not null and new.counts_per_gram is distinct from old.counts_per_gram)
    or (new.net_counts      is not null and new.net_counts      is distinct from old.net_counts)
    or (new.bowls           is not null and new.bowls           is distinct from old.bowls)
    or (new.bowls_confirmed is not null and new.bowls_confirmed is distinct from old.bowls_confirmed)
    or (new.gross_g         is not null and new.gross_g         is distinct from old.gross_g)
    or (new.stack_count     is not null and new.stack_count     is distinct from old.stack_count)
    or (new.stack_status    is not null and new.stack_status    is distinct from old.stack_status)
    or (new.levels          is not null and new.levels          is distinct from old.levels)
    or (new.sensors_ok      is not null and new.sensors_ok      is distinct from old.sensors_ok)
    or (new.sensors_online  is not null and new.sensors_online  is distinct from old.sensors_online)
    or (new.supply_mv       is not null and new.supply_mv       is distinct from old.supply_mv)
    or (new.checksum_errors      is not null and new.checksum_errors      is distinct from old.checksum_errors)
    or (new.responding      is not null and new.responding      is distinct from old.responding)
    or (new.no_load_g       is not null and new.no_load_g       is distinct from old.no_load_g) then
      raise exception
        '% is a hub: it reports its own power and identity only -- never a '
        'weight, bowls, stack_* or a platform''s node health', new.device_id
        using errcode = 'check_violation';
    end if;
  end if;

  return new;
end $$;

revoke all on function public.tg_device_status_kind() from public, anon, authenticated;

comment on function public.tg_device_status_kind() is
  'Guard on device_status: a stack writes only stack_*, a scale or buffer '
  'never writes stack_* or power (battery/charging/external_power -- that is '
  'the hub''s), a scale never writes bowls/gross_g and stays under 100 kg, a '
  'hub writes only power and identity. Tests only columns that changed to a '
  'non-null value. SECURITY DEFINER because it reads devices, which anon '
  'cannot.';

-- ---------------------------------------------------------------------
-- 7. device_overview -- the four node-health columns, APPENDED.
--
-- CREATE OR REPLACE VIEW may only add columns at the end, so they go after
-- gross_g; every existing column keeps its name, place and type, and
-- schema.sql and weekly_menu_and_offline.sql say the same. A hub's battery
-- needs nothing new here -- it is in battery_mv / battery_level / charging,
-- where it always was, on the hub's own row.
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
       -- whenever weight_state is not 'ok' -- see migrate_buffer.sql
       -- section 3.
       s.bowls,
       s.bowls_confirmed,
       s.gross_g,

       -- Platform node health. NULL -- never 0 -- on a hub, a stack, and a
       -- platform whose node has not measured it.
       s.supply_mv,
       s.checksum_errors,
       s.responding,
       s.no_load_g
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone);

commit;

-- ---------------------------------------------------------------------
--  Verify. One result set.
-- ---------------------------------------------------------------------
select 'devices_kind_ck accepts hub' as what,
       (select count(*) from pg_constraint
         where conname = 'devices_kind_ck'
           and pg_get_constraintdef(oid) like '%''hub''%')::text as detail
union all
select 'hubs registered, each with a status row',
       (select count(*) from public.devices d
          join public.device_status s using (device_id)
         where d.kind = 'hub')::text
union all
select 'node-health columns (device_status / device_overview)',
       (select count(*) from information_schema.columns
         where table_schema = 'public' and table_name = 'device_status'
           and column_name in ('supply_mv','checksum_errors','responding','no_load_g'))
       || ' / ' ||
       (select count(*) from information_schema.columns
         where table_schema = 'public' and table_name = 'device_overview'
           and column_name in ('supply_mv','checksum_errors','responding','no_load_g'))
union all
select 'platform rows trimmed by this run',
       (select count(*) from _hubs_trim)::text
union all
select 'platform rows still holding power',
       (select count(*) from public.device_status s
          join public.devices d using (device_id)
         where d.kind in ('scale','buffer')
           and (s.battery_mv is not null or s.battery_level is not null
                or s.charging is not null or s.external_power is not null))::text
union all
select 'anon SELECT columns on device_status',
       (select count(*) from information_schema.column_privileges
         where table_schema = 'public' and table_name = 'device_status'
           and grantee = 'anon' and privilege_type = 'SELECT')::text
union all
select 'devices by kind',
       (select string_agg(kind || ' ' || n, ', ' order by kind)
          from (select kind, count(*) as n from public.devices group by kind) k);
-- Expect: 1, 3, 4 / 4, however many platforms held a battery (0 on a
-- re-run), 0, 1, and hub 3 beside the kinds that were there.
