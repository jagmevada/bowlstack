-- =====================================================================
--  Undo migrate_loadcell.sql.
--
--  Run as owner in the Supabase SQL editor. Returns the database to exactly
--  the shape it had before the load-cell work: no devices.kind, no weight
--  columns on device_status, no weight_mismatch_tolerance(), and all three
--  dashboard views back to their previous bodies.
--
--  WHAT THIS DESTROYS
--  ------------------
--  Every weight any load cell has reported, and the record of which
--  installations are scales. Nothing else -- the registry, the bowl counters,
--  their telemetry, the menu and the whole existing dashboard are untouched,
--  because the migration never modified any of them.
--
--  A load-cell station keeps running afterwards and keeps weighing; its
--  PATCH simply starts failing with a 400 (no such column), which its uplink
--  reports and backs off from. Turn the stations off, or re-run the migration,
--  rather than leaving them retrying into a schema that no longer has anywhere
--  to put the answer.
--
--  ORDER MATTERS, and differently from rollback_bowl_weight.sql. The three
--  views SELECT the new columns, so the columns cannot be dropped while the
--  views still reference them -- Postgres tracks the dependency and would
--  refuse, or with CASCADE would silently take the views with it.
--
--  And the views must be DROPPED, not replaced. CREATE OR REPLACE VIEW may
--  only ADD columns at the end; it cannot remove them. So each one is dropped
--  and recreated from its pre-migration body -- which also means re-granting,
--  since a dropped view takes its privileges with it.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 1. The three views, back to their pre-migration bodies.
--
-- Dropped together and recreated together. None of them reads another, so the
-- order within this block is presentational rather than a dependency -- but
-- all three must go before section 3 can touch a column.
-- ---------------------------------------------------------------------
drop view if exists public.slot_quantity;
drop view if exists public.slot_overview;
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
                false) as missed_last_service
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone);

create view public.slot_overview
with (security_invoker = true) as
select d.location,
       d.food_slot,
       max(m.food_name)                                        as current_food,
       public.current_meal_type(max(d.timezone))               as current_meal,
       count(*)                                                as devices,
       count(*) filter (where coalesce(s.reported, false))     as devices_reported,

       (count(*) * 4)::bigint                                  as bowls_capacity,

       sum(s.stack_count) filter (where s.stack_status = 'ok') as bowls_trusted,
       sum(s.stack_count)                                      as bowls_reported,

       bool_or(s.stack_status = 'discontiguous')               as any_fault,
       bool_or(s.stack_status = 'degraded')                    as any_degraded,
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
               false))                                         as any_missed_service
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
         count(*)                                                as devices,
         bool_or(coalesce(s.reported, false))                    as any_reported,
         (count(*) * 4)::bigint                                  as bowls_capacity,
         sum(s.stack_count) filter (where s.stack_status = 'ok') as bowls_trusted,
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
)
select p.food_slot,

       public.current_meal_type(max(p.timezone))                 as current_meal,

       max(p.menu_meal_date)                                     as menu_meal_date,
       max(p.menu_meal_type)                                     as menu_meal_type,
       coalesce(max(p.menu_meal_type)
                  = public.current_meal_type(max(p.timezone))
                and max(p.menu_meal_date)
                  = public.current_meal_date(max(p.timezone)), false)
                                                                 as menu_is_live,

       array_agg(distinct p.food_name)
         filter (where p.food_name is not null)                  as dishes,

       (case when count(distinct p.bowl_weight_g) = 1
             then max(p.bowl_weight_g) end)                      as bowl_weight_g,

       sum(p.devices)                                            as devices,
       sum(p.bowls_capacity)                                     as bowls_capacity,

       sum(p.bowls_trusted)                                      as bowls_trusted,

       sum(coalesce(p.bowls_trusted, 0)::bigint * p.bowl_weight_g)
         filter (where p.bowl_weight_g is not null)              as est_weight_g,

       sum(p.bowls_capacity * p.bowl_weight_g)
         filter (where p.bowl_weight_g is not null)              as capacity_weight_g,

       bool_or(p.bowl_weight_g is null
               and coalesce(p.bowls_trusted, 0) > 0)             as est_is_partial,

       array_remove(array_agg(
         case when p.bowl_weight_g is null
               and coalesce(p.bowls_trusted, 0) > 0
              then p.location end), null)                        as areas_without_weight,

       jsonb_agg(jsonb_build_object(
         'location',       p.location,
         'food_name',      p.food_name,
         'bowl_weight_g',  p.bowl_weight_g,
         'bowls_trusted',  p.bowls_trusted,
         'bowls_capacity', p.bowls_capacity,
         'devices',        p.devices,
         'weight_g',       case when p.bowl_weight_g is not null
                                 and p.bowls_trusted is not null
                                then p.bowls_trusted::bigint * p.bowl_weight_g end,
         'capacity_weight_g', case when p.bowl_weight_g is not null
                                   then p.bowls_capacity * p.bowl_weight_g end
       ) order by p.location)                                    as areas,

       bool_or(p.any_fault)                                      as any_fault,
       bool_or(p.any_degraded)                                   as any_degraded,
       bool_or(p.any_battery_warn)                               as any_battery_warn,
       bool_or(p.any_offline)                                    as any_offline,
       bool_or(p.any_missed_service)                             as any_missed_service,
       min(p.oldest_update)                                      as oldest_update
  from per_area p
 group by p.food_slot
having bool_or(p.any_reported)
 order by p.food_slot;

-- Grants go back too. A dropped view takes its privileges with it, so leaving
-- these out would return a database whose columns match and whose ACLs do not
-- -- and the dashboard would fail with "permission denied for view", which
-- reads as an RLS problem rather than as an incomplete rollback.
revoke all on public.device_overview from anon, authenticated, public;
revoke all on public.slot_overview   from anon, authenticated, public;
revoke all on public.slot_quantity   from anon, authenticated, public;
grant select on public.device_overview to authenticated;
grant select on public.slot_overview   to authenticated;
grant select on public.slot_quantity   to authenticated;

comment on view public.slot_quantity is
  'Master Dashboard source: quantity per dish position, summed across every '
  'serving area, in grams. Each area is weighed against ITS OWN dish before '
  'summing. NULL where no weight is configured -- never zero.';

-- ---------------------------------------------------------------------
-- 2. The tolerance helper. Nothing else reads it once the views are back.
-- ---------------------------------------------------------------------
drop function if exists public.weight_mismatch_tolerance();

-- ---------------------------------------------------------------------
-- 3. The columns, and their constraints.
--
-- Constraints first, explicitly, rather than relying on DROP COLUMN to take
-- them along. It does -- but naming them means a rollback run against a
-- half-applied migration (constraints added, columns not) still converges,
-- which is the property that makes this safe to re-run.
--
-- The GRANTs need no undoing: a column grant dies with its column.
-- ---------------------------------------------------------------------
alter table public.device_status
  drop constraint if exists device_status_weight_state_ck,
  drop constraint if exists device_status_weight_agrees_ck,
  drop constraint if exists device_status_weight_range_ck,
  drop constraint if exists device_status_cells_online_ck;

alter table public.device_status
  drop column if exists weight_g,
  drop column if exists weight_state,
  drop column if exists cells_online,
  drop column if exists counts_per_gram,
  drop column if exists net_counts;

alter table public.devices
  drop constraint if exists devices_kind_ck;

alter table public.devices
  drop column if exists kind;

commit;
