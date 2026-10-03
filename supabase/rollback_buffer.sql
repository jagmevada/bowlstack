-- =====================================================================
--  Undo migrate_buffer.sql.
--
--  Run as owner in the Supabase SQL editor. Returns the database to the
--  shape apply_loadcell.sql left it in before the buffer work: kind is
--  stack or scale, weight_g is capped at 100 kg by CHECK on both tables, no
--  bowls / bowls_confirmed / gross_g, no kind guards on device_status or
--  status_events, and every view back to its previous body, grants and
--  comment.
--
--  ORDER. After rollback_cutover_buffers.sql if the cut-over ever ran, and
--  BEFORE rollback_vbus_sense / manual_fill / burn_rate / weight_samples /
--  loadcell -- it recreates the burn-rate views, which read objects those
--  drop. apply_loadcell.sql's header lists the whole sequence.
--
--  WHAT THIS DESTROYS
--  ------------------
--  The bowl counts and gross weights every buffer reported, current and
--  historical. The FOOD weights stay: they are weight_g, which predates this
--  migration.
--
--  IT REFUSES, before changing anything, while:
--
--    * any device is still kind = 'buffer' -- the restored kind CHECK would
--      reject that row, and a buffer with its views gone is a device whose
--      weight the dashboard silently stops reading. Run
--      rollback_cutover_buffers.sql first.
--    * any weight_g above 100 kg is stored, current or historical. The
--      restored CHECKs would fail on it -- and the alternative, quietly
--      deleting food history to make a rollback fit, is not this file's
--      decision to take. Look at the rows, then delete or keep them.
--
--  A firmware still sending bowls / gross_g gets 400 on every PATCH after
--  this, and goes silent. Turn the buffer uplink off first.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 0. Refusals. Checked BEFORE anything is written.
-- ---------------------------------------------------------------------
do $$
declare v_n bigint;
begin
  if to_regprocedure('public.burn_rate_tuning()') is null
     or to_regclass('public.weight_samples') is null then
    raise exception
      'Run this BEFORE rollback_burn_rate.sql and rollback_weight_samples.sql '
      '-- it recreates views that read burn_rate_tuning() and weight_samples, '
      'and one of them is already gone. Nothing has been changed.';
  end if;

  select count(*) into v_n from public.devices where kind = 'buffer';
  if v_n > 0 then
    raise exception
      '% device(s) are still kind = ''buffer''. Run '
      'supabase/rollback_cutover_buffers.sql first. Nothing has been changed.',
      v_n;
  end if;

  select count(*) into v_n from public.device_status where weight_g > 100000;
  if v_n > 0 then
    raise exception
      '% device_status row(s) hold a weight above 100 kg, which the restored '
      'CHECK would refuse. Nothing has been changed.', v_n;
  end if;

  select count(*) into v_n from public.weight_samples where weight_g > 100000;
  if v_n > 0 then
    raise exception
      '% weight_samples row(s) are above 100 kg -- buffer history. Decide '
      'about them (select * from weight_samples where weight_g > 100000) and '
      'run this again. Nothing has been changed.', v_n;
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. The burn-rate views, back to migrate_burn_rate.sql's bodies.
--    Dependency order: slot_burn_rate reads slot_stock_series.
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
buffer AS (
  select g.at_ts, d.location, d.food_slot,
         sum(e.stack_count * w.bowl_weight_g)                  as buffer_g,
         bool_or(w.bowl_weight_g is null and e.stack_count > 0
                 and w.is_serving)                             as partial
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
   group by g.at_ts, d.location, d.food_slot
),
counter as (
  select g.at_ts, d.location, d.food_slot,
         sum(s.weight_g)  as counter_g,
         count(*)         as scales
    from grid g
    join public.devices d on d.kind = 'scale'
                         and d.location in ('D','M','T')
                         and d.food_slot is not null
   cross join lateral (
     select ws.weight_g
       from public.weight_samples ws
      where ws.device_id = d.device_id
        and ws.recorded_at <= g.at_ts
        and ws.weight_state = 'ok'
        and ws.weight_g is not null
      order by ws.recorded_at desc
      limit 1
   ) s
   group by g.at_ts, d.location, d.food_slot
)
select coalesce(b.at_ts, c.at_ts)                as at_ts,
       coalesce(b.location, c.location)          as location,
       coalesce(b.food_slot, c.food_slot)        as food_slot,
       b.buffer_g,
       c.counter_g,
       case when b.buffer_g is null and c.counter_g is null then null
            else coalesce(b.buffer_g, 0) + coalesce(c.counter_g, 0) end as total_g,
       coalesce(b.partial, false)                as buffer_is_partial,
       coalesce(c.scales, 0)                     as scales
  from buffer b
  full join counter c on c.at_ts = b.at_ts
                    and c.location = b.location
                    and c.food_slot = b.food_slot;

revoke all on public.slot_stock_series from anon, authenticated, public;
grant select on public.slot_stock_series to authenticated;

comment on view public.slot_stock_series is
  'Total food per dish position over the last hour, on a 5-minute grid: '
  'buffered bowls (BWL, valued at the menu per-bowl weight) PLUS the active '
  'counter (LDC, weighed). Summing both is what makes a buffer-to-counter '
  'refill read as flat rather than as a phantom serving.';

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
  'Consumption per dish position, over buffered stock PLUS the active counter '
  'combined -- so moving bowls from the buffer onto the counter reads as flat '
  'rather than as a serving. g_per_hour and runs_out_at are NULL when the '
  'series is too short to support them; is_partial marks a total that could '
  'not value every hall''s buffer, which makes the rate a lower bound.';

-- ---------------------------------------------------------------------
-- 2. device_burn_rate -- scales only again. Same columns, so REPLACE.
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
 where d.kind = 'scale';

comment on view public.device_burn_rate is
  'Consumption rate per load-cell station over the last hour. Sums only '
  'FALLING intervals, so a refill contributes nothing rather than a negative; '
  'ignores falls below burn_rate_tuning().min_drop_g, which is the same '
  'threshold the firmware uses to decide something happened. g_per_hour and '
  'runs_out_at are NULL when the series is too short to support them.';

-- ---------------------------------------------------------------------
-- 3. The three dashboard views, back to migrate_loadcell.sql's bodies.
--
-- DROPPED, not replaced: CREATE OR REPLACE VIEW cannot remove the appended
-- columns. Grants restated because a dropped view takes them with it.
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
                false) as missed_last_service,

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
       array_remove(array_agg(distinct s.weight_state)
                    filter (where d.kind = 'scale'), 'ok')     as scale_issues,

       max(m.bowl_weight_g)                                    as bowl_weight_g,
       (sum(s.stack_count) filter (where s.stack_status = 'ok'))
         * max(m.bowl_weight_g)                                as buffer_g,
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
         sum(s.stack_count) filter (where s.stack_status = 'ok') as bowls_trusted,
         sum(s.weight_g) filter (where d.kind = 'scale'
                                   and s.weight_state = 'ok')    as measured_weight_g,
         count(*) filter (where d.kind = 'scale')                as scales,
         count(*) filter (where d.kind = 'scale'
                            and s.weight_state = 'ok')           as scales_ok,
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
per_area_w as (
  select p.*,
         case when p.bowl_weight_g is not null
              then coalesce(p.bowls_trusted, 0)::bigint * p.bowl_weight_g
         end                                                     as est_total_g,
         case when p.bowl_weight_g is not null
               and p.bowls_trusted is not null
              then p.bowls_trusted::bigint * p.bowl_weight_g
         end                                                     as est_line_g,
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
       q.scales,
       q.scales_ok,
       q.measured_weight_g,
       q.weight_g,
       q.buffer_g,
       q.counter_g,
       si.scale_issues
  from per_slot q
  left join slot_issues si on si.food_slot = q.food_slot
 order by q.food_slot;

revoke all on public.device_overview from anon, authenticated, public;
revoke all on public.slot_overview   from anon, authenticated, public;
revoke all on public.slot_quantity   from anon, authenticated, public;
grant select on public.device_overview to authenticated;
grant select on public.slot_overview   to authenticated;
grant select on public.slot_quantity   to authenticated;

comment on view public.slot_quantity is
  'Master Dashboard source: quantity per dish position across every serving '
  'area, in grams. weight_g is the authoritative figure and is a SUM: '
  'buffer_g (bowls waiting, counted x per-bowl weight) plus counter_g (food '
  'on the scales), because those are different food in different places. It '
  'was once measured-beats-estimated per area, which discarded a hall''s '
  'whole buffer whenever a scale there happened to read empty. est_weight_g '
  'and measured_weight_g are the two inputs, kept so a screen can say which '
  'part it is showing.';

-- ---------------------------------------------------------------------
-- 4. tg_weight_samples_kind -- scales only, as migrate_weight_samples.sql
--    wrote it. Restored BEFORE the columns go: a plpgsql body is not
--    dependency-tracked, so leaving the buffer version in place would only
--    fail at the next insert, on a column that no longer exists.
-- ---------------------------------------------------------------------
create or replace function public.tg_weight_samples_kind()
returns trigger language plpgsql security definer set search_path = '' as $$
declare v_kind text;
begin
  select kind into v_kind from public.devices where device_id = new.device_id;
  if v_kind is distinct from 'scale' then
    raise exception
      'weight_samples accepts only devices.kind = ''scale''; % is %',
      new.device_id, coalesce(v_kind, 'not registered')
      using errcode = 'check_violation';
  end if;
  return new;
end $$;

revoke all on function public.tg_weight_samples_kind() from public, anon, authenticated;
comment on function public.tg_weight_samples_kind() is null;

-- ---------------------------------------------------------------------
-- 5. The two guards this migration added.
-- ---------------------------------------------------------------------
drop trigger  if exists device_status_kind on public.device_status;
drop trigger  if exists status_events_kind on public.status_events;
drop function if exists public.tg_device_status_kind();
drop function if exists public.tg_status_events_kind();

-- ---------------------------------------------------------------------
-- 6. The columns and their constraints. Constraints first and by name, so
--    a rollback of a half-applied migration still converges. The anon
--    grants die with their columns.
-- ---------------------------------------------------------------------
alter table public.device_status
  drop constraint if exists device_status_bowls_range_ck,
  drop constraint if exists device_status_bowls_confirmed_ck,
  drop constraint if exists device_status_gross_range_ck,
  drop constraint if exists device_status_bowls_ok_ck;

alter table public.device_status
  drop column if exists bowls,
  drop column if exists bowls_confirmed,
  drop column if exists gross_g;

alter table public.weight_samples
  drop constraint if exists weight_samples_bowls_range_ck,
  drop constraint if exists weight_samples_bowls_confirmed_ck,
  drop constraint if exists weight_samples_gross_range_ck,
  drop constraint if exists weight_samples_bowls_ok_ck;

alter table public.weight_samples
  drop column if exists bowls,
  drop column if exists bowls_confirmed,
  drop column if exists gross_g;

-- ---------------------------------------------------------------------
-- 7. The 100 kg rails, back in the CHECKs. weight_samples gets the name
--    Postgres gave its inline original, so the table is indistinguishable
--    from one migrate_weight_samples.sql just built.
-- ---------------------------------------------------------------------
alter table public.device_status
  drop constraint if exists device_status_weight_range_ck,
  add  constraint device_status_weight_range_ck
       check (weight_g is null or weight_g between -5000 and 100000);

alter table public.weight_samples
  drop constraint if exists weight_samples_weight_range_ck,
  drop constraint if exists weight_samples_weight_g_check,
  add  constraint weight_samples_weight_g_check
       check (weight_g is null or weight_g between -5000 and 100000);

-- ---------------------------------------------------------------------
-- 8. devices.kind -- stack or scale.
-- ---------------------------------------------------------------------
alter table public.devices
  drop constraint if exists devices_kind_ck,
  add  constraint devices_kind_ck check (kind in ('stack','scale'));

comment on column public.devices.kind is
  'What this installation MEASURES. stack = four ToF sensors up a pipe, '
  'reporting a bowl count; scale = three load cells under a platform, '
  'reporting grams. Fixed at registration, never a setting. The views use it '
  'to decide which columns of device_status are a measurement and which are '
  'absent -- a scale leaves every stack_* column NULL, and a stack leaves '
  'every weight_* column NULL.';

comment on column public.device_status.weight_g is
  'What is on the platform, in grams, net of the platform zero and the '
  'session tare. Present ONLY when weight_state = ''ok'' -- enforced by '
  'device_status_weight_agrees_ck. NULL means no trustworthy weight; 0 means '
  'a measured, empty platform. Those are different facts and must not be '
  'collapsed.';

commit;

-- ---------------------------------------------------------------------
--  Verify. One result set; every count must be 0.
-- ---------------------------------------------------------------------
select 'buffer columns remaining' as what,
       count(*) as remaining
  from information_schema.columns
 where table_schema = 'public'
   and table_name in ('device_status','weight_samples','device_overview',
                      'slot_overview','slot_quantity')
   and column_name in ('bowls','bowls_confirmed','gross_g','buffers',
                       'buffers_ok','buffer_measured_g','buffer_bowls',
                       'buffer_unconfirmed','buffer_issues','weight_is_partial')
union all
select 'buffer guard triggers remaining',
       count(*)
  from pg_trigger
 where not tgisinternal
   and tgname in ('device_status_kind','status_events_kind')
union all
select 'kind CHECK still accepting buffer',
       count(*)
  from pg_constraint
 where conname = 'devices_kind_ck'
   and pg_get_constraintdef(oid) like '%buffer%';
