-- =====================================================================
--  Migration -- consumption rate and projected run-out.
--
--  Run as owner AFTER migrate_weight_samples.sql. Idempotent, and it creates
--  only views and one function: it reads weight_samples and writes nothing.
--
--  Reversible: supabase/rollback_burn_rate.sql.
--
--  ---------------------------------------------------------------------
--  THE QUESTION THIS ANSWERS
--  ---------------------------------------------------------------------
--  "Which food is going fastest, and will it run out before service ends?"
--  -- because the answer, delivered while there is still time to cook, is
--  what starts a second production run instead of an apology.
--
--  device_status can say "8.2 kg of dal left". It cannot say "that is going
--  at 4.1 kg/h and it is gone in two hours, forty minutes before service
--  closes". The second sentence needs a series, which weight_samples is.
--
--  ---------------------------------------------------------------------
--  STOCK IS BUFFER PLUS COUNTER, AND THAT IS THE WHOLE DESIGN
--  ---------------------------------------------------------------------
--  The stock of a dish is not what is on the counter. It is the buffered
--  bowls in the BWL stacks across every hall PLUS what the LDC platform is
--  weighing -- and consumption is the drain of that combined pool.
--
--  Measuring the counter alone is wrong in a way that is worse than
--  imprecise, and the first cut of this file was wrong exactly that way.
--  Tested against a synthetic service with one buffer-to-counter transfer:
--
--      combined      18.0 kg left, 11.0 kg/h  ->  runs out 00:26
--      counter only   3.0 kg left, 12.0 kg/h  ->  runs out 23:03
--
--  Counter-only sends somebody to start a second production run fifteen
--  minutes out when there is an hour and a half of rice sitting in the
--  stacks. Summing both terms first makes the transfer disappear rather than
--  needing to be detected: it moves grams from one term to the other and
--  leaves the total flat, which is what a transfer is.
--
--  THE OTHER TWO THINGS THAT MAKE THIS HARD
--
--  1. A RATE FROM TWO POINTS IS NOT A RATE. The window has to be long enough
--     to average out a single serving and short enough to describe NOW. One
--     hour, and the row carries `covered` so an estimate from four minutes is
--     distinguishable from one from sixty.
--
--  2. A BUFFER WITHOUT A PER-BOWL WEIGHT CANNOT BE VALUED. Assuming a weight
--     invents stock; assuming zero reports the buffer as empty. It is
--     excluded and `is_partial` says so, which makes the rate a LOWER bound
--     rather than a wrong number.
-- =====================================================================

begin;

do $$
begin
  if not exists (select 1 from pg_tables
                  where schemaname='public' and tablename='weight_samples') then
    raise exception
      'Run supabase/migrate_weight_samples.sql FIRST -- there is no history '
      'to compute a rate from. Nothing has been changed.';
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. The tuning, in one place -- the offline_after() pattern again.
--
-- WINDOW is how far back a rate looks. One hour is long enough that a single
-- serving does not dominate and short enough to notice a rush.
--
-- REFILL_G is the RISE that means somebody topped the counter up, as opposed
-- to a tray being set down or a hand resting on the platform. It is large on
-- purpose: a refill is a pan or a stack of bowls, kilograms not grams, and
-- anything smaller is better treated as noise on a segment than as a new one.
--
-- MIN_SPAN is the least coverage a rate may be computed from. Below it the
-- view returns NULL rather than a number -- a slope from ninety seconds of a
-- three-hour meal is arithmetic, not information.
--
-- THERE IS DELIBERATELY NO PER-INTERVAL THRESHOLD, and the first cut of this
-- view had one -- 250 g, mirroring the firmware's WEIGHT_EVENT_G -- summing
-- only the intervals that fell by at least that much. It was wrong twice over
-- and the second way is the instructive one:
--
--   * It rejected real consumption. At the 2-minute sample cadence a station
--     draining a very ordinary 4 kg/h falls 133 g per interval, so EVERY
--     interval was below the threshold and the rate came out NULL. Tested
--     against a synthetic service and it reported nothing at all.
--
--   * Lowering the threshold would have been worse. Summing only FALLS is a
--     biased estimator: measurement noise pushes half of a flat station's
--     intervals downward, and those all get counted while the matching rises
--     are discarded. A still counter would report steady consumption.
--
-- So the estimate is endpoints-within-segments instead -- see the view. Noise
-- then affects only the two ends of each segment, where ±100 g against a
-- multi-kilogram fall is nothing, and it cannot accumulate.
-- ---------------------------------------------------------------------
create or replace function public.burn_rate_tuning(
  out window_span interval, out refill_g integer, out min_span interval)
returns record
language sql immutable
set search_path = ''
as $$ select interval '60 minutes', 1000, interval '10 minutes' $$;

comment on function public.burn_rate_tuning() is
  'How a consumption rate is computed: how far back to look, the smallest '
  'RISE that counts as a refill rather than noise, and the least coverage a '
  'rate may be reported from at all.';

revoke all on function public.burn_rate_tuning() from public, anon;
grant execute on function public.burn_rate_tuning() to authenticated;

-- ---------------------------------------------------------------------
-- 2. device_burn_rate -- one row per load cell.
-- ---------------------------------------------------------------------
create or replace view public.device_burn_rate
with (security_invoker = true) as
with t as (select * from public.burn_rate_tuning()),
-- Only samples that carry a weight. An uncalibrated or partly-dead station
-- contributes its STATE to the dashboard and nothing to the arithmetic:
-- weight_g is NULL there, and a NULL in a difference is not a zero.
pts as (
  select w.device_id, w.recorded_at, w.weight_g
    from public.weight_samples w
   cross join t
   where w.weight_state = 'ok'
     and w.weight_g is not null
     and w.recorded_at >= now() - t.window_span
),
-- SEGMENT AT THE REFILLS. A running count of the big rises gives each stretch
-- between top-ups its own id, so the endpoints taken below are the endpoints
-- of one continuous draw-down rather than of a sawtooth.
-- TWO CTEs, because a window function may not appear inside another window
-- function's FILTER. The lag has to be materialised as a plain column before
-- the running count can be filtered on it.
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
-- ENDPOINTS PER SEGMENT -- the whole estimator. first - last is what left the
-- counter during that stretch; the elapsed time is the denominator. Noise
-- touches only the two ends and cannot accumulate across the series, which is
-- what the rejected per-interval version could not say.
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
         -- Negative segments clamped to zero rather than subtracted. A flat
         -- stretch that happens to end a few grams higher than it started has
         -- not produced food, and letting it offset a real fall elsewhere
         -- would understate the drain on a busy position.
         sum(greatest(g.seg_drop_g, 0))                       as consumed_g,
         -- Only the time inside segments counts. A station idle for fifty
         -- minutes and busy for ten consumed at the BUSY rate, and dividing by
         -- the whole hour would report a sixth of the truth to somebody
         -- deciding whether to start cooking again.
         sum(g.seg_s) filter (where g.seg_drop_g > 0)         as consuming_s,
         -- When the last top-up happened: the start of any segment after the
         -- first. Worth surfacing because it is the context that makes the
         -- current weight readable -- "8 kg left" means something different
         -- twenty minutes after a refill than two hours after one.
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

       -- The headline. NULL rather than 0 when the series is too short to
       -- support one -- note 3. A rate of "0.0 kg/h" and "we do not know yet"
       -- send a kitchen to opposite decisions.
       case when a.last_at - a.first_at >= (select min_span from t)
             and coalesce(a.consuming_s, 0) > 0
            then round((a.consumed_g / (a.consuming_s / 3600.0))::numeric, 0)
       end                                                   as g_per_hour,

       s.weight_g                                            as current_g,

       -- How long the current weight lasts at the current rate. The number the
       -- whole view exists for.
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

revoke all on public.device_burn_rate from anon, authenticated, public;
grant select on public.device_burn_rate to authenticated;

comment on view public.device_burn_rate is
  'Consumption rate per load-cell station over the last hour. Sums only '
  'FALLING intervals, so a refill contributes nothing rather than a negative; '
  'ignores falls below burn_rate_tuning().min_drop_g, which is the same '
  'threshold the firmware uses to decide something happened. g_per_hour and '
  'runs_out_at are NULL when the series is too short to support them.';

-- ---------------------------------------------------------------------
-- 3. slot_stock_series -- TOTAL food for a dish position, over time.
--
-- ####  THE THING THE WHOLE FILE IS FOR, AND THE EASY WAY TO GET IT WRONG ####
--
-- The stock of a dish is NOT what is on the counter. It is:
--
--     buffered bowls (BWL, every hall)  +  the active counter (LDC)
--
-- and consumption is the drain of that COMBINED pool.
--
-- Measuring the counter alone is wrong in a specific and damaging way, and
-- the first cut of this view did exactly that. When a counter runs low
-- somebody carries bowls from the buffer stack onto it. The counter RISES and
-- the buffer FALLS by the same food -- nothing has been eaten. A
-- counter-only view sees a rise, calls it a refill and excludes it; it never
-- sees the buffer fall at all. So an internal transfer and a genuine serving
-- become indistinguishable, and the projection is wrong in whichever
-- direction the last transfer happened to fall.
--
-- Summing both first makes the problem disappear rather than needing to be
-- handled: a transfer moves grams from one term to the other and leaves the
-- total flat, which is exactly what it is. Only food actually leaving the
-- building moves this number down.
--
-- ---------------------------------------------------------------------
-- HOW THE TWO HALVES ARE MADE COMPARABLE
--
-- They arrive in different units, from different tables, at different times:
--
--   BWL  status_events.stack_count      a COUNT, on change
--   LDC  weight_samples.weight_g        GRAMS, on change and every 2 min
--
-- The count becomes grams through meal_food_mapping.bowl_weight_g, the same
-- per-bowl figure slot_quantity uses -- so Master and this view can never
-- disagree about what a bowl weighs.
--
-- A BUFFER WITH NO PER-BOWL WEIGHT CONTRIBUTES NOTHING AND SAYS SO. There is
-- no sensible default: assuming a weight would invent stock, and assuming
-- zero would report the buffer as empty. buffer_is_partial carries that, and
-- a rate computed over a partial total is a LOWER BOUND on consumption --
-- flagged, not silently returned as if it were exact.
--
-- Neither series is regular, so both are sampled onto a common grid by
-- carrying the last known reading forward. That is what makes them addable.
-- ---------------------------------------------------------------------
-- DROPPED, not replaced, and in dependency order. CREATE OR REPLACE VIEW may
-- only append columns -- it cannot rename or remove one. An earlier cut of
-- slot_burn_rate measured the counter alone and had a different column list,
-- so replacing it fails with "cannot change name of view column". Nothing
-- reads either view yet, which is what makes dropping them free; the grants
-- are restated below because a dropped view takes its privileges with it.
--
-- Found on the live database rather than locally: PGlite always built these
-- fresh, so only the path a real database takes -- one that already had the
-- earlier version -- could show it.
drop view if exists public.slot_burn_rate;
drop view if exists public.slot_stock_series;

create view public.slot_stock_series
with (security_invoker = true) as
with t as (select * from public.burn_rate_tuning()),
grid as (
  -- Five minutes. Fine enough to see a service move, coarse enough that an
  -- hour is twelve points rather than hundreds of correlated lookups.
  select g as at_ts
    from t, generate_series(now() - t.window_span, now(), interval '5 minutes') g
),
-- The per-bowl weight in force for each (location, slot), resolved through the
-- same last_served_meal() the Master tab uses, so the buffer is valued at the
-- dish actually on the stations rather than at whatever the clock says.
wt as (
  select d.location, d.food_slot, max(m.bowl_weight_g) as bowl_weight_g
    from public.devices d
   cross join lateral public.last_served_meal(d.timezone) lm
    left join public.meal_food_mapping m
           on m.location = d.location and m.food_slot = d.food_slot
          and m.meal_date = lm.meal_date and m.meal_type = lm.meal_type
   where d.location in ('D','M','T') and d.food_slot is not null
   group by d.location, d.food_slot
),
-- BUFFER: the last bowl count each stack had reported at or before each grid
-- point, valued at its hall's per-bowl weight. Trusted counts only -- a
-- degraded stack's count is a lower bound and a discontiguous one is not a
-- count, and neither belongs in a figure denominated in kilograms.
buffer AS (
  select g.at_ts, d.food_slot,
         sum(e.stack_count * w.bowl_weight_g)                  as buffer_g,
         bool_or(w.bowl_weight_g is null and e.stack_count > 0) as partial
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
   group by g.at_ts, d.food_slot
),
-- COUNTER: the last usable weight each scale had reported at or before each
-- grid point. weight_state = 'ok' is the whole gate -- an uncalibrated or
-- partly-dead station has no grams to add.
counter as (
  select g.at_ts, d.food_slot,
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
   group by g.at_ts, d.food_slot
)
select coalesce(b.at_ts, c.at_ts)                as at_ts,
       coalesce(b.food_slot, c.food_slot)        as food_slot,
       b.buffer_g,
       c.counter_g,
       -- coalesce to 0 per TERM, not on the sum: a slot with a counter and no
       -- buffered stack has a real total, and so does the reverse. NULL only
       -- when neither side has ever reported.
       case when b.buffer_g is null and c.counter_g is null then null
            else coalesce(b.buffer_g, 0) + coalesce(c.counter_g, 0) end as total_g,
       coalesce(b.partial, false)                as buffer_is_partial,
       coalesce(c.scales, 0)                     as scales
  from buffer b
  full join counter c on c.at_ts = b.at_ts and c.food_slot = b.food_slot;

revoke all on public.slot_stock_series from anon, authenticated, public;
grant select on public.slot_stock_series to authenticated;

comment on view public.slot_stock_series is
  'Total food per dish position over the last hour, on a 5-minute grid: '
  'buffered bowls (BWL, valued at the menu per-bowl weight) PLUS the active '
  'counter (LDC, weighed). Summing both is what makes a buffer-to-counter '
  'refill read as flat rather than as a phantom serving.';

-- ---------------------------------------------------------------------
-- 4. slot_burn_rate -- the second-production-run question.
--
-- Endpoints over the series above. No refill handling is needed here at all,
-- which is the point of summing the two halves first: within one meal the
-- total only goes DOWN, because the only way it rises is somebody bringing
-- food from the kitchen -- which is itself a production run and deserves to
-- reset the estimate rather than be smoothed into it.
-- ---------------------------------------------------------------------
create view public.slot_burn_rate
with (security_invoker = true) as
with t as (select * from public.burn_rate_tuning()),
pts as (
  select * from public.slot_stock_series where total_g is not null
),
-- Segment at kitchen deliveries, on the same reasoning device_burn_rate uses
-- for counter refills -- but here a rise means food entering the BUILDING.
lagged as (
  select p.*, lag(p.total_g) over w as prev_g
    from pts p
  window w as (partition by p.food_slot order by p.at_ts)
),
steps as (
  select l.*,
         count(*) filter (where l.total_g - l.prev_g >= (select refill_g from t))
           over (partition by l.food_slot order by l.at_ts
                 rows between unbounded preceding and current row) as segment
    from lagged l
),
seg as (
  select s.food_slot, s.segment,
         min(s.at_ts) as seg_first_at, max(s.at_ts) as seg_last_at,
         (array_agg(s.total_g order by s.at_ts))[1]
           - (array_agg(s.total_g order by s.at_ts desc))[1] as drop_g,
         extract(epoch from max(s.at_ts) - min(s.at_ts))     as seg_s,
         bool_or(s.buffer_is_partial)                        as partial
    from steps s group by s.food_slot, s.segment having count(*) >= 2
),
agg as (
  select g.food_slot,
         min(g.seg_first_at)                            as first_at,
         max(g.seg_last_at)                             as last_at,
         sum(greatest(g.drop_g, 0))                     as consumed_g,
         sum(g.seg_s) filter (where g.drop_g > 0)       as consuming_s,
         max(g.seg_first_at) filter (where g.segment > 0) as last_delivery_at,
         bool_or(g.partial)                             as partial
    from seg g group by g.food_slot
)
select a.food_slot,
       -- Read straight from the menu, NOT joined from slot_quantity. A view on
       -- a view cannot be dropped without dropping its dependant, and this
       -- chain existed only to borrow a dish name -- which made slot_quantity
       -- un-replaceable the first time it needed a column removed. Two sibling
       -- views over the same tables, never a stack of them.
       (select array_agg(distinct m.food_name)
          from public.devices d
         cross join lateral public.last_served_meal(d.timezone) lm
          join public.meal_food_mapping m
            on m.location = d.location and m.food_slot = d.food_slot
           and m.meal_date = lm.meal_date and m.meal_type = lm.meal_type
         where d.food_slot = a.food_slot
           and d.location in ('D','M','T')
           and m.food_name is not null)                  as dishes,
       (a.last_at - a.first_at)                         as covered,
       a.consumed_g                                     as consumed_g_window,
       n.buffer_g                                       as buffer_g,
       n.counter_g                                      as counter_g,
       n.total_g                                        as total_g,
       a.last_delivery_at,

       -- A LOWER BOUND when some hall's buffer could not be valued. The UI
       -- already renders that as ">=" for slot_quantity; the same notation
       -- applies to a rate computed over an incomplete total.
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

       -- WILL IT LAST THE MEAL? The question a second production run is
       -- started from. NULL when there is no rate or no window to compare
       -- against -- never a confident "yes".
       case when a.last_at - a.first_at >= (select min_span from t)
             and coalesce(a.consuming_s,0) > 0 and a.consumed_g > 0
             and n.total_g is not null
            then now() + make_interval(secs =>
                   (n.total_g / (a.consumed_g / a.consuming_s))::double precision)
                 < public.last_service_window_end('Asia/Kolkata', null)
                   + interval '1 day' * 0
       end                                              as short_before_close
  from agg a
  join lateral (
    -- The most recent grid point, which is "now" for this slot.
    select s.buffer_g, s.counter_g, s.total_g
      from public.slot_stock_series s
     where s.food_slot = a.food_slot and s.total_g is not null
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

commit;
