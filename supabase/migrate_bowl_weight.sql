-- =====================================================================
--  Migration -- per-bowl weight, and the Master Dashboard's view.
--
--  Run as owner in the Supabase SQL editor, ONCE, on a database already
--  built by schema.sql. Idempotent and NON-destructive: it adds a column,
--  widens two functions and creates one view. No table is dropped and no
--  existing row is rewritten, so it is safe to run mid-trial.
--
--  schema.sql carries the same changes for a from-scratch rebuild. The two
--  must agree; this file exists only because a live trial database cannot
--  be dropped and rebuilt.
--
--  WHAT THIS ADDS
--  --------------
--    1. meal_food_mapping.bowl_weight_g  -- what ONE bowl of that dish weighs
--    2. meal_menu_template.bowl_weight_g -- the same, on the weekly plan
--    3. meal_mapping_preload()           -- returns the weight alongside the dish
--    4. meal_template_apply()            -- carries the weight into dated rows
--    5. public.slot_quantity             -- the Master Dashboard's source
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 1 + 2. The weight column.
--
-- NULLABLE, and deliberately NOT defaulted to zero. Everywhere else in this
-- schema NULL means "no reading" and 0 means "measured, and it is empty" --
-- see slot_overview.bowls_trusted. A default of 0 here would render a full
-- counter as "0.0 kg remaining" the moment someone typed a dish name and
-- forgot the weight, which sends staff to refill something that is full.
-- NULL renders as "weight not set" and prompts instead.
--
-- GRAMS, as an integer, not kilograms as numeric. The dashboard's arithmetic
-- is sum-then-multiply across up to five devices and three areas per slot;
-- doing that in binary floating point accumulates visible drift on a screen
-- that shows one decimal. Integer grams cannot drift, and the UI divides by
-- 1000 exactly once, at render.
--
-- The bounds are sanity rails, not policy: 100 g is below any real serving
-- bowl and 50 kg is above any bowl a person can lift onto a counter. They
-- exist to turn a units mix-up (kilograms typed into a grams field) into a
-- 400 at the edge rather than a dashboard reading 14 000 kg.
-- ---------------------------------------------------------------------
alter table public.meal_food_mapping
  add column if not exists bowl_weight_g integer;

alter table public.meal_menu_template
  add column if not exists bowl_weight_g integer;

do $$
begin
  alter table public.meal_food_mapping
    add constraint meal_food_mapping_bowl_weight_ck
    check (bowl_weight_g is null or bowl_weight_g between 100 and 50000);
exception when duplicate_object then null;
end $$;

do $$
begin
  alter table public.meal_menu_template
    add constraint meal_menu_template_bowl_weight_ck
    check (bowl_weight_g is null or bowl_weight_g between 100 and 50000);
exception when duplicate_object then null;
end $$;

comment on column public.meal_food_mapping.bowl_weight_g is
  'Mass of ONE full bowl of this dish, in grams. NULL means not configured -- '
  'never zero, which would read as an empty counter. The Master Dashboard '
  'multiplies it by the trusted bowl count for this (location, food_slot).';

comment on column public.meal_menu_template.bowl_weight_g is
  'Per-bowl mass carried by the weekly plan, in grams. Materialised into '
  'meal_food_mapping by the menu editor''s Save or by meal_template_apply().';

-- No GRANT is needed for either column. Both tables carry TABLE-level grants
-- to `authenticated` (schema.sql section 8), which cover columns added later.
-- Contrast device_status, whose device-facing grants are column-level and
-- would need extending -- devices never touch the menu, so nothing to do.

-- ---------------------------------------------------------------------
-- 3. meal_mapping_preload -- now returns the weight too.
--
-- DROP then CREATE, not CREATE OR REPLACE: Postgres refuses to replace a
-- function whose OUT parameters change, and adding a column to a `returns
-- table` is exactly that. The signature is unchanged, so every existing
-- caller and the GRANT below still match.
--
-- Weight rides with the dish through all three branches, so an inherited or
-- templated menu arrives weighed as well as named. Carrying the name forward
-- but not the weight would have meant re-typing a weight that never changes.
-- ---------------------------------------------------------------------
drop function if exists public.meal_mapping_preload(text, text, date);

create function public.meal_mapping_preload(
  p_location  text,
  p_meal_type text,
  p_meal_date date
) returns table (
  food_slot     smallint,
  food_name     text,
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
  templ as (
    select t.food_slot, t.food_name, t.bowl_weight_g,
           p_meal_date as source_date, false as is_saved
      from public.meal_menu_template t
     where t.location  = p_location
       and t.meal_type = p_meal_type
       and t.weekday   = extract(dow from p_meal_date)::smallint
       and p_meal_date >= public.current_meal_date('Asia/Kolkata')
  ),
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

revoke all on function public.meal_mapping_preload(text, text, date)
  from public, anon;
grant execute on function public.meal_mapping_preload(text, text, date)
  to authenticated;

-- ---------------------------------------------------------------------
-- 4. meal_template_apply -- carry the weight into the dated rows.
--
-- CREATE OR REPLACE is fine here: the return table is unchanged, only the
-- INSERT inside it widens. Everything else about the function -- the refusal
-- to write the past, meal-granularity skipping, the 31-day cap, running as
-- invoker -- is untouched.
-- ---------------------------------------------------------------------
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

      -- THE ONLY CHANGE IN THIS FUNCTION: bowl_weight_g rides along, so a
      -- materialised week arrives weighed as well as named.
      insert into public.meal_food_mapping
             (location, meal_type, meal_date, food_slot, food_name, bowl_weight_g)
      select t.location, t.meal_type, v_date, t.food_slot, t.food_name, t.bowl_weight_g
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

revoke all on function public.meal_template_apply(text, date, date, boolean)
  from public, anon;
grant execute on function public.meal_template_apply(text, date, date, boolean)
  to authenticated;

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
-- 5. public.slot_quantity -- the Master Dashboard's source.
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
         count(*)                                                as devices,
         bool_or(coalesce(s.reported, false))                    as any_reported,
         (count(*) * 4)::bigint                                  as bowls_capacity,

         -- Trusted only: a degraded stack's count is a lower bound and a
         -- discontiguous one is not a count at all. Folding either into a
         -- WEIGHT would dress an unreliable number up as kilograms, which
         -- reads far more precise than it is.
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
    -- The meal whose menu describes the food actually on the stations --
    -- the current one during service, the one that just finished outside it.
    -- See last_served_meal(): a NULL current meal used to strip every dish
    -- name and weight off this screen for two thirds of the day.
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

       -- Which meal the dishes and weights below actually came from, and
       -- whether that meal is the one running now. When it is not, the UI
       -- must name it ("Dinner, 22 Aug") rather than letting a figure from
       -- last night read as live.
       max(p.menu_meal_date)                                     as menu_meal_date,
       max(p.menu_meal_type)                                     as menu_meal_type,
       coalesce(max(p.menu_meal_type)
                  = public.current_meal_type(max(p.timezone))
                and max(p.menu_meal_date)
                  = public.current_meal_date(max(p.timezone)), false)
                                                                 as menu_is_live,

       -- The distinct dishes at this position, sorted. Usually one. More than
       -- one is not an error -- the areas genuinely differ at slot 3 -- so the
       -- UI lists them rather than flagging them.
       array_agg(distinct p.food_name)
         filter (where p.food_name is not null)                  as dishes,

       -- The per-bowl weight, but ONLY when every weighed area agrees on it.
       -- NULL when they differ, because there is no single "kg per bowl" to
       -- print for the slot -- the total is still exact (each area was weighed
       -- against its own dish), it just has no one-line unit.
       (case when count(distinct p.bowl_weight_g) = 1
             then max(p.bowl_weight_g) end)                      as bowl_weight_g,

       sum(p.devices)                                            as devices,
       sum(p.bowls_capacity)                                     as bowls_capacity,

       -- NULL, not 0, when no area reported: sum() over all-NULL is NULL,
       -- which is the answer we want. No data is not an empty counter.
       sum(p.bowls_trusted)                                      as bowls_trusted,

       -- The headline. NULL when not one contributing area has a weight;
       -- otherwise the sum of what CAN be weighed.
       sum(coalesce(p.bowls_trusted, 0)::bigint * p.bowl_weight_g)
         filter (where p.bowl_weight_g is not null)              as est_weight_g,

       sum(p.bowls_capacity * p.bowl_weight_g)
         filter (where p.bowl_weight_g is not null)              as capacity_weight_g,

       -- The total above is a LOWER BOUND: some area is holding bowls we
       -- cannot weigh. The UI renders these as ">=45.0 kg", the same way a
       -- degraded stack's count already renders as ">=3" -- this codebase
       -- already has a vocabulary for a number that is real but incomplete,
       -- and inventing a second one would be worse than reusing it.
       bool_or(p.bowl_weight_g is null
               and coalesce(p.bowls_trusted, 0) > 0)             as est_is_partial,

       -- Which areas need a weight typed in, so the UI can name them instead
       -- of saying "somewhere".
       array_remove(array_agg(
         case when p.bowl_weight_g is null
               and coalesce(p.bowls_trusted, 0) > 0
              then p.location end), null)                        as areas_without_weight,

       -- Per-area breakdown: the "D 9 / M 3 / T 2" line, and the dish list
       -- when the areas diverge. Assembled here rather than by a second query
       -- because the client must never re-derive an area's share of a total
       -- it did not compute.
       jsonb_agg(jsonb_build_object(
         'location',       p.location,
         'food_name',      p.food_name,
         'bowl_weight_g',  p.bowl_weight_g,
         'bowls_trusted',  p.bowls_trusted,
         'bowls_capacity', p.bowls_capacity,
         'devices',        p.devices,
         -- This hall's own mass. Computed HERE rather than multiplied in the
         -- client for the same reason the slot total is: the moment a screen
         -- does its own arithmetic on stock, it can disagree with the screen
         -- next to it. NULL when the hall has no weight -- never 0, which
         -- would read as an empty counter.
         -- NULL when this hall has no weight OR no reading. Not zero: a hall
         -- whose stack is silent has not reported an empty counter, and
         -- "0.0 kg" beside a bowl count of "—" is a contradiction that sends
         -- someone to refill a station nobody has heard from.
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

revoke all on public.slot_quantity from anon, authenticated, public;
grant select on public.slot_quantity to authenticated;

comment on view public.slot_quantity is
  'Master Dashboard source. One row per food_slot across ALL serving areas, '
  'with remaining stock in grams. Weight is summed per area against that '
  'area''s own dish, so a slot serving different dishes in different halls '
  'still totals correctly. Slots whose devices have never reported are '
  'excluded, which hides the undeployed backup units.';

commit;

-- ---------------------------------------------------------------------
--  Tell PostgREST the schema changed.
--
--  PostgREST answers from a CACHED picture of the schema, so a view created
--  a moment ago is not visible to the API until that cache is rebuilt -- the
--  request fails with PGRST205 "Could not find the table ... in the schema
--  cache", which reads exactly like the migration never ran. Supabase
--  usually reloads within a minute by itself; this makes it immediate.
-- ---------------------------------------------------------------------
notify pgrst, 'reload schema';

-- ---------------------------------------------------------------------
--  Verify. Expect one row per slot that has ever reported -- currently
--  slots 1-4 -- and NO row for slot 5 while the backups stay dark.
-- ---------------------------------------------------------------------
select food_slot,
       array_to_string(dishes, ' / ')                        as dishes,
       bowls_trusted || ' / ' || bowls_capacity              as bowls,
       coalesce(round(bowl_weight_g / 1000.0, 1)::text, 'mixed') || ' kg' as per_bowl,
       case when est_weight_g is null then 'no weight set'
            else case when est_is_partial then '>=' else '' end
                 || round(est_weight_g / 1000.0, 1)::text || ' kg'
       end                                                   as remaining,
       coalesce(array_to_string(areas_without_weight, ','), '-') as needs_weight
  from public.slot_quantity
 order by food_slot;
