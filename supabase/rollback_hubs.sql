-- =====================================================================
--  Undo migrate_hubs.sql.
--
--  Run as owner in the Supabase SQL editor. Returns the database to the
--  shape migrate_buffer.sql left it in: kind is stack, scale or buffer, no
--  hub devices, no supply_mv / crc_errors / responding / no_load_g, the
--  kind guard and device_overview back to migrate_buffer.sql's bodies.
--
--  ORDER. FIRST of the undos -- before rollback_cutover_buffers.sql and
--  rollback_buffer.sql, which refuses while this file's columns exist.
--  apply_loadcell.sql's header lists the whole sequence.
--
--  WHAT THIS CANNOT UNDO
--  ---------------------
--  The trim. migrate_hubs.sql section 5 NULLed battery_mv, battery_level,
--  charging and external_power on every scale and buffer row, and those
--  values are gone -- this file does not and cannot put them back. Nothing
--  is lost that a device does not resend: a platform running firmware that
--  still reports power repopulates them on its next PATCH, because the
--  guard that refused it is the first thing restored below.
--
--  WHAT THIS DESTROYS
--  ------------------
--  The three hubs -- their devices rows, their device_status rows (the
--  battery and mains each hub reported) and any per-hub service_windows
--  override, which cascades. A hub has no history to lose: status_events
--  and weight_samples both refuse a hub. And every platform's node health.
--
--  IT REFUSES, before changing anything, while any device other than
--  HUB-D, HUB-M and HUB-T is kind 'hub'. The restored kind CHECK would
--  reject that row, and deleting a device this file did not register is not
--  its decision to take: re-kind or delete it yourself, then run this again.
--
--  A hub firmware still PATCHing HUB-D/M/T then matches no row and latches
--  "unprovisioned"; a firmware sending node health gets 400 on every PATCH
--  and goes silent. Stop both first.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 0. Refusals. Checked BEFORE anything is written.
-- ---------------------------------------------------------------------
do $$
declare v_ids text;
begin
  if to_regprocedure('public.tg_device_status_kind()') is null
     or not exists (select 1 from information_schema.columns
                     where table_schema = 'public' and table_name = 'device_status'
                       and column_name = 'bowls') then
    raise exception
      'migrate_buffer.sql''s objects are gone, so the device_overview and the '
      'guard this file restores cannot be rebuilt. Run this BEFORE '
      'rollback_buffer.sql. Nothing has been changed.';
  end if;

  select string_agg(device_id, ', ' order by device_id) into v_ids
    from public.devices
   where kind = 'hub' and device_id not in ('HUB-D','HUB-M','HUB-T');
  if v_ids is not null then
    raise exception
      'kind = ''hub'' on device(s) this migration did not register: %. The '
      'restored kind CHECK would refuse them -- re-kind or delete them first. '
      'Nothing has been changed.', v_ids;
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. device_status_kind -- back to migrate_buffer.sql section 6.
--
-- Restored BEFORE the columns go: a plpgsql body is not dependency-
-- tracked, so the hub version left in place would fail at the next PATCH,
-- on a column that no longer exists.
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

-- ---------------------------------------------------------------------
-- 2. device_overview -- back to migrate_buffer.sql section 8.
--
-- DROPPED, not replaced: CREATE OR REPLACE VIEW cannot remove the appended
-- columns, and the columns cannot be dropped while the view reads them.
-- Grants restated because a dropped view takes them with it.
-- ---------------------------------------------------------------------
drop view if exists public.device_overview;

create view public.device_overview
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
       s.gross_g
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone);

revoke all on public.device_overview from anon, authenticated, public;
grant select on public.device_overview to authenticated;

-- ---------------------------------------------------------------------
-- 3. The hubs. Status rows first, explicitly, though the foreign key would
--    cascade them -- so the statement says what it removes.
-- ---------------------------------------------------------------------
delete from public.device_status
 where device_id in (select device_id from public.devices where kind = 'hub');
delete from public.devices where kind = 'hub';

-- ---------------------------------------------------------------------
-- 4. The node-health columns. Constraints first and by name, so a rollback
--    of a half-applied migration still converges. The anon grants and the
--    column comments die with their columns.
-- ---------------------------------------------------------------------
alter table public.device_status
  drop constraint if exists device_status_supply_mv_ck,
  drop constraint if exists device_status_crc_errors_ck,
  drop constraint if exists device_status_no_load_ck;

alter table public.device_status
  drop column if exists supply_mv,
  drop column if exists crc_errors,
  drop column if exists responding,
  drop column if exists no_load_g;

-- ---------------------------------------------------------------------
-- 5. devices.kind -- stack, scale or buffer, and migrate_buffer.sql's
--    comment.
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

commit;

-- ---------------------------------------------------------------------
--  Verify. One result set; every count must be 0.
-- ---------------------------------------------------------------------
select 'node-health columns remaining' as what,
       count(*) as remaining
  from information_schema.columns
 where table_schema = 'public'
   and table_name in ('device_status','device_overview')
   and column_name in ('supply_mv','crc_errors','responding','no_load_g')
union all
select 'hub devices remaining',
       count(*)
  from public.devices
 where kind = 'hub'
union all
select 'kind CHECK still accepting hub',
       count(*)
  from pg_constraint
 where conname = 'devices_kind_ck'
   and pg_get_constraintdef(oid) like '%''hub''%';
