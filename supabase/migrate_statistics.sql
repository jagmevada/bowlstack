-- =====================================================================
--  Statistics: food on hand and consumption per dish slot, per meal, D+M+T
--
--  ADDITIVE and IDEMPOTENT. Two read-only functions for the dashboard's
--  Statistics page; no table, column or row changes. Undo:
--  rollback_statistics.sql.
--
--  WHY FUNCTIONS, NOT VIEWS. slot_stock_series / slot_burn_rate answer "the
--  last hour, now". The Statistics page asks about ANY meal on ANY day -- a
--  date, a meal and a slot are parameters, and a view cannot take them.
--
--  SAME MEANING AS THE MASTER PAGE. Food on hand is buffer + counter (weight_g
--  is food for both kinds), so carrying a bowl from the buffer to the counter
--  reads as flat; a rise of burn_rate_tuning().refill_g or more is a delivery
--  from the kitchen, and consumption is the drop within the stretches between
--  deliveries -- slot_burn_rate's rule, so the two pages never disagree.
--
--  A slot is the same dish in all three areas at a given meal (owner,
--  2026-10-04), so a slot's food on hand is the sum over D, M and T.
--
--    slot_meal_series(date, meal, slot)  -> 5-min points per area: the chart
--    slot_meal_stats(from, to, meal?, slot?) -> one row per meal and slot:
--        consumed, delivered, kg/h, the busiest 15 minutes, ran out, coverage
--
--  ONLY INSTRUMENTS THAT REPORTED DURING THAT MEAL COUNT. The registry gives
--  every slot a counter scale whether or not one is fitted; a platform that sent
--  nothing in the window is not part of the measurement, rather than a gap that
--  makes every point "partial". One that reported and then fell silent or left
--  `ok` is a gap: those points are marked partial.
-- =====================================================================
begin;

do $$
begin
  if to_regclass('public.weight_samples') is null then
    raise exception 'run migrate_weight_samples.sql first -- there is no history to read';
  end if;
  if to_regprocedure('public.burn_rate_tuning()') is null then
    raise exception 'run migrate_burn_rate.sql first -- burn_rate_tuning() is missing';
  end if;
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'devices'
                    and column_name = 'kind') then
    raise exception 'run migrate_loadcell.sql first -- devices.kind is missing';
  end if;
end $$;

-- Dropped, not replaced: a returns-table function cannot gain a column in place.
drop function if exists public.slot_meal_stats(date, date, text, integer);
drop function if exists public.slot_meal_series(date, text, integer);

-- ---------------------------------------------------------------------
-- Food on hand through one meal, per area, on a 5-minute grid.
--
-- The window is the fleet's service window for that meal, in IST, up to now
-- for a meal still running. At each point every platform contributes its
-- newest sample at or before it -- the lateral slot_stock_series uses, served
-- by weight_samples_device_time_idx -- BUT ONLY FROM THE LAST 10 MINUTES:
-- devices are off between meals, and the last reading of the previous meal is
-- not what is on the counter now. A platform reports at least every 2 minutes
-- while on, so 10 minutes without a sample is a gap, not a slow device.
-- ---------------------------------------------------------------------
create function public.slot_meal_series(p_date date, p_meal text, p_slot integer)
returns table (at_ts timestamptz, location text, buffer_g bigint, counter_g bigint,
               total_g bigint, partial boolean)
language sql stable set search_path = '' as $$
  with w as (
    select ((p_date + sw.starts_at) at time zone 'Asia/Kolkata') as t0,
           ((p_date + sw.ends_at)   at time zone 'Asia/Kolkata') as t1
      from public.service_windows sw
     where sw.device_id is null and sw.label = lower(p_meal)
     order by sw.starts_at
     limit 1
  ),
  grid as (
    select g as at_ts
      from w, generate_series(w.t0, least(w.t1, now()), interval '5 minutes') g
  ),
  dev as (
    select d.device_id, d.kind, d.location
      from public.devices d, w
     where d.kind in ('scale', 'buffer')
       and d.location in ('D', 'M', 'T')
       and d.food_slot = p_slot
       and exists (select 1 from public.weight_samples s
                    where s.device_id = d.device_id
                      and s.recorded_at >  w.t0 - interval '10 minutes'
                      and s.recorded_at <= w.t1)
  )
  select g.at_ts,
         v.location,
         sum(ws.weight_g) filter (where v.kind = 'buffer') as buffer_g,
         sum(ws.weight_g) filter (where v.kind = 'scale')  as counter_g,
         -- weight_g is NULL unless ok (the table's own CHECK), so a platform
         -- without a weight adds nothing here and marks the point partial below.
         sum(ws.weight_g)                                  as total_g,
         bool_or(ws.weight_state is distinct from 'ok')    as partial
    from grid g
   cross join dev v
    left join lateral (
      select s.weight_state, s.weight_g
        from public.weight_samples s
       where s.device_id = v.device_id
         and s.recorded_at <= g.at_ts
         and s.recorded_at >  g.at_ts - interval '10 minutes'
       order by s.recorded_at desc
       limit 1
    ) ws on true
   group by g.at_ts, v.location
   order by g.at_ts, v.location
$$;

revoke all on function public.slot_meal_series(date, text, integer) from public, anon;
grant execute on function public.slot_meal_series(date, text, integer) to authenticated;

comment on function public.slot_meal_series(date, text, integer) is
  'Food on hand (buffer + counter, grams of food) for one dish slot through one '
  'meal, per area D/M/T, on a 5-minute grid over the fleet service window (IST), '
  'up to now. Each platform that reported during the meal contributes its newest '
  'sample within the 10 minutes before each point; partial marks a point where one '
  'had no ok sample. Statistics page.';

-- ---------------------------------------------------------------------
-- One row per meal and slot over a date range: what was eaten and how fast.
--
--   consumed_g        the drop within each stretch between deliveries, summed
--                     (slot_burn_rate's rule; noise inside a stretch cancels)
--   delivered_g       the rises of refill_g or more -- the kitchen restocking
--   g_per_hour        consumed over the minutes covered
--   rush_g_per_hour   the largest drop over 15 minutes with no delivery inside
--                     it, x 4; rush_at is the END of that quarter hour
--   ran_out_at        the first point at or under 1 kg after the slot had held
--                     more -- empty, the same 1 kg an empty platform is
--   short_before_close  ran out before the meal's window closed
-- ---------------------------------------------------------------------
create function public.slot_meal_stats(p_from date, p_to date,
                                       p_meal text default null,
                                       p_slot integer default null)
returns table (meal_date date, meal_type text, food_slot integer, food_name text,
               areas text[], start_g bigint, end_g bigint, consumed_g bigint,
               delivered_g bigint, g_per_hour numeric, rush_g_per_hour numeric,
               rush_at timestamptz, ran_out_at timestamptz,
               short_before_close boolean, covered_min integer, partial boolean)
language sql stable set search_path = '' as $$
  with t as (select refill_g from public.burn_rate_tuning()),
  meals as (
    select dd::date as meal_date,
           initcap(sw.label) as meal_type,
           ((dd::date + sw.ends_at) at time zone 'Asia/Kolkata') as ends_ts
      from generate_series(p_from::timestamp, p_to::timestamp, interval '1 day') dd
     cross join public.service_windows sw
     where sw.device_id is null
       and (p_meal is null or sw.label = lower(p_meal))
  ),
  slots as (
    select distinct d.food_slot::integer as food_slot
      from public.devices d
     where d.kind in ('scale', 'buffer')
       and d.location in ('D', 'M', 'T')
       and d.food_slot is not null
       and (p_slot is null or d.food_slot = p_slot)
  ),
  ser as (
    select m.meal_date, m.meal_type, m.ends_ts, s.food_slot,
           x.at_ts, x.location, x.total_g, x.partial
      from meals m
     cross join slots s
     cross join lateral public.slot_meal_series(m.meal_date, m.meal_type, s.food_slot) x
  ),
  -- D + M + T at each point.
  pts as (
    select meal_date, meal_type, ends_ts, food_slot, at_ts,
           sum(total_g)::bigint as total_g,
           bool_or(partial)     as partial
      from ser
     group by meal_date, meal_type, ends_ts, food_slot, at_ts
  ),
  stepped as (
    select p.*,
           p.total_g - lag(p.total_g) over w  as d,       -- change INTO this point
           lead(p.total_g) over w - p.total_g as d_next,  -- change out of it
           max(p.total_g) over w              as peak_so_far
      from pts p
     where p.total_g is not null
    window w as (partition by p.meal_date, p.meal_type, p.food_slot order by p.at_ts)
  ),
  -- THE FOOD EATEN DURING A DELIVERY STEP. A 5-minute step that brings 33 kg in
  -- also carries the ~3 kg eaten in those minutes, so the rise reads 30 kg and,
  -- without this, that 3 kg is missing from consumed -- every delivery day read
  -- low against the days without one (seen on the local seed: 29.9 kg "delivered"
  -- for 33). Estimated as the mean drop of the steps either side, never negative.
  segd as (
    select s.*,
           count(*) filter (where s.d >= (select refill_g from t))
             over (partition by s.meal_date, s.meal_type, s.food_slot order by s.at_ts
                   rows between unbounded preceding and current row) as segment,
           case when s.d >= (select refill_g from t) then
             (greatest(coalesce(-lag(s.d) over n, 0), 0) + greatest(coalesce(-s.d_next, 0), 0))
             / greatest((lag(s.d) over n is not null)::int + (s.d_next is not null)::int, 1)
           end as hidden_g
      from stepped s
    window n as (partition by s.meal_date, s.meal_type, s.food_slot order by s.at_ts)
  ),
  quarter as (
    select q.*,
           case when q.at_ts - lag(q.at_ts, 3) over k = interval '15 minutes'
                then lag(q.total_g, 3) over k - q.total_g end as drop15
      from segd q
    window k as (partition by q.meal_date, q.meal_type, q.food_slot, q.segment
                 order by q.at_ts)
  ),
  seg as (
    select meal_date, meal_type, food_slot, segment,
           (array_agg(total_g order by at_ts))[1]
             - (array_agg(total_g order by at_ts desc))[1] as drop_g,
           count(*) as n
      from segd
     group by meal_date, meal_type, food_slot, segment
  ),
  cons as (
    select meal_date, meal_type, food_slot,
           sum(greatest(drop_g, 0)) filter (where n >= 2) as consumed_g
      from seg
     group by meal_date, meal_type, food_slot
  ),
  agg as (
    select q.meal_date, q.meal_type, q.food_slot,
           min(q.ends_ts)                                          as ends_ts,
           (array_agg(q.total_g order by q.at_ts))[1]              as start_g,
           (array_agg(q.total_g order by q.at_ts desc))[1]         as end_g,
           coalesce(sum(q.d + q.hidden_g) filter (where q.d >= (select refill_g from t)), 0) as delivered_g,
           coalesce(sum(q.hidden_g), 0)                            as hidden_g,
           max(q.drop15)                                           as rush_drop,
           (array_agg(q.at_ts order by q.drop15 desc nulls last))[1] as rush_at,
           min(q.at_ts) filter (where q.total_g <= 1000 and q.peak_so_far > 1000) as ran_out_at,
           extract(epoch from max(q.at_ts) - min(q.at_ts)) / 60.0  as covered_min,
           bool_or(q.partial)                                      as partial
      from quarter q
     group by q.meal_date, q.meal_type, q.food_slot
  )
  select a.meal_date,
         a.meal_type,
         a.food_slot,
         -- The dish from the menus of the areas MEASURED, not of all three: a slot
         -- is one dish everywhere in practice, and a menu typed differently in an
         -- unmeasured area must not rename what was weighed.
         (select string_agg(distinct mf.food_name, ' / ')
            from public.meal_food_mapping mf
           where mf.meal_date = a.meal_date and mf.meal_type = a.meal_type
             and mf.food_slot = a.food_slot
             and mf.location in (select x.location from ser x
                                  where x.meal_date = a.meal_date and x.meal_type = a.meal_type
                                    and x.food_slot = a.food_slot))           as food_name,
         (select array_agg(distinct x.location order by x.location)
            from ser x
           where x.meal_date = a.meal_date and x.meal_type = a.meal_type
             and x.food_slot = a.food_slot)                       as areas,
         a.start_g,
         a.end_g,
         round(coalesce(c.consumed_g, 0) + a.hidden_g)::bigint   as consumed_g,
         round(a.delivered_g)::bigint                             as delivered_g,
         case when a.covered_min >= 10
              then round((coalesce(c.consumed_g, 0) + a.hidden_g) / (a.covered_min / 60.0), 0)
         end                                                      as g_per_hour,
         case when a.rush_drop > 0 then round(a.rush_drop * 4.0, 0) end      as rush_g_per_hour,
         case when a.rush_drop > 0 then a.rush_at end                         as rush_at,
         a.ran_out_at,
         coalesce(a.ran_out_at < a.ends_ts, false)                as short_before_close,
         a.covered_min::integer                                   as covered_min,
         a.partial
    from agg a
    left join cons c using (meal_date, meal_type, food_slot)
   order by a.meal_date, a.meal_type, a.food_slot
$$;

revoke all on function public.slot_meal_stats(date, date, text, integer) from public, anon;
grant execute on function public.slot_meal_stats(date, date, text, integer) to authenticated;

comment on function public.slot_meal_stats(date, date, text, integer) is
  'Per meal and dish slot over a date range, D+M+T summed: food at start and end, '
  'consumed (drops between deliveries), delivered, g/hour, busiest 15 minutes, when '
  'it ran out (<= 1 kg) and whether that was before close. Built on '
  'slot_meal_series. Statistics page.';

commit;

select 'slot_meal_series' as what,
       (to_regprocedure('public.slot_meal_series(date, text, integer)') is not null)::int as present
union all
select 'slot_meal_stats',
       (to_regprocedure('public.slot_meal_stats(date, date, text, integer)') is not null)::int;
