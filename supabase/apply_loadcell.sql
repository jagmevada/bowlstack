-- =====================================================================
--  Bowlstack -- apply the load-cell augmentation.  GENERATED FILE.
--
--  Regenerate with:  node tools/build_apply_loadcell.mjs
--  Do not edit by hand -- edit the files it fuses and re-run that.
--
--  ---------------------------------------------------------------------
--  WHAT THIS IS FOR
--  ---------------------------------------------------------------------
--  Paste the whole file into the Supabase SQL editor and run it ONCE. It
--  does what these 9 do, in the only order that works:
--
--    1. migrate_bowl_weight.sql    per-bowl weight, and the Master Dashboard view
--    2. migrate_loadcell.sql       load-cell stations alongside the bowl counters
--    3. register_loadcells.sql     LDC-001..032 into the devices registry
--    4. assign_loadcells.sql       each LDC onto the position of its BWL
--    5. migrate_weight_samples.sql the analog history a scale appends to
--    6. migrate_burn_rate.sql      consumption rate, and when a dish runs out
--    7. migrate_manual_fill.sql    TRIAL HARNESS -- the manual fill estimate
--    8. migrate_vbus_sense.sql     mains presence, from the VBUS divider
--    9. migrate_buffer.sql         BWL buffer platforms: kind buffer, bowls, kg views
--
--  ---------------------------------------------------------------------
--  IF YOU NEED TO UNDO IT AFTER IT HAS COMMITTED
--  ---------------------------------------------------------------------
--  Run these 6, IN THIS ORDER -- they undo in the reverse of the
--  order applied, because each drops objects the one before it depends on:
--
--      rollback_buffer.sql
--      rollback_vbus_sense.sql
--      rollback_manual_fill.sql
--      rollback_burn_rate.sql
--      rollback_weight_samples.sql
--      rollback_loadcell.sql
--
--  Taking them out of order fails on a dependency rather than doing
--  damage, so a mistake here is loud. Note that migrate_bowl_weight.sql
--  (part 1) has NO rollback and does not need one: it predates the load
--  cells and the bowl counters depend on it.
--
--  If supabase/cutover_buffers.sql has ever run, run
--  rollback_cutover_buffers.sql BEFORE all of them: rollback_buffer.sql
--  refuses while any device is still a buffer.
--
--  ---------------------------------------------------------------------
--  WHY IT IS SAFE TO RUN MID-SERVICE
--  ---------------------------------------------------------------------
--  ONE TRANSACTION. All 9 parts and the verification run inside a single
--  BEGIN. If ANY of it fails -- a missing prerequisite, a constraint, a
--  verification check -- the whole thing rolls back and your database is
--  exactly as it was. There is no half-applied state to recover from.
--
--  IT PROVES IT DID NOT TOUCH THE BOWL COUNTERS. Before changing anything
--  it snapshots every existing device, its status row, and the whole of
--  slot_overview -- the numbers the Stock screen renders. After the
--  migration it compares them and RAISES if a single figure moved, which
--  aborts the transaction. "It should not affect bowl counting" becomes a
--  check the database performs rather than a claim in a comment.
--
--  IT ADDS AND NEVER REMOVES. No table is dropped, no row is rewritten, no
--  existing column changes name, type, nullability or default, and the only
--  CHECKs it changes, it widens.
--
--  IT IS SAFE TO RE-RUN, before or after the buffer cut-over: no device's
--  kind may change (check #4), and every view ends on its newest body because
--  migrate_buffer.sql runs last. The cut-over itself is NOT in here -- it is
--  supabase/cutover_buffers.sql, run on its own.
--
--  ---------------------------------------------------------------------
--  TO REHEARSE FIRST
--  ---------------------------------------------------------------------
--  Change the LAST line of this file from
--
--      commit;
--  to
--      rollback;
--
--  and run it. Everything executes, every check runs, the report prints --
--  and nothing is kept. Then change it back and run it for real.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
--  BEFORE: snapshot what must not change.
--
--  Temp tables, so they vanish with the session and cannot collide with
--  anything. Captured INSIDE the transaction, so what they record is
--  precisely the state this transaction started from.
-- ---------------------------------------------------------------------
-- kind through to_jsonb() rather than by name: on a database that predates
-- migrate_loadcell.sql the column does not exist, and naming it would be a
-- parse error. Such a row is a bowl counter, which is what the column's
-- default will call it -- so 'stack' is the honest snapshot.
create temp table _before_devices on commit drop as
  select device_id, location, food_slot, label, timezone,
         coalesce(to_jsonb(d) ->> 'kind', 'stack') as kind
    from public.devices d;

create temp table _before_status on commit drop as
  select device_id, reported, boot_id, uptime_s, stack_count, stack_status,
         levels, sensors_ok, sensors_online, battery_mv, battery_level,
         charging, firmware, mac
    from public.device_status;

create temp table _before_stock on commit drop as
  select location, food_slot, devices, devices_reported, bowls_capacity,
         bowls_trusted, bowls_reported, any_fault, any_degraded
    from public.slot_overview;

create temp table _before_counts on commit drop as
  select (select count(*) from public.devices)            as devices,
         (select count(*) from public.device_status)      as device_status,
         (select count(*) from public.status_events)      as status_events,
         (select count(*) from public.service_windows)    as service_windows,
         (select count(*) from public.meal_food_mapping)  as meal_food_mapping;

-- #####################################################################
-- ##  PART 1 of 9:  migrate_bowl_weight.sql
-- ##  per-bowl weight, and the Master Dashboard view
-- #####################################################################

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

-- #####################################################################
-- ##  PART 2 of 9:  migrate_loadcell.sql
-- ##  load-cell stations alongside the bowl counters
-- #####################################################################

-- ---------------------------------------------------------------------
-- 0. Prerequisites.
--
-- Checked BEFORE anything is written, so an out-of-order run leaves the
-- database exactly as it found it and says what to do about it.
--
-- Without this the file gets as far as section 7 and aborts on
--
--     function public.last_served_meal(text) does not exist
--
-- which names a symptom two hundred lines from its cause and reads like a
-- corrupt schema rather than a missing prerequisite. slot_quantity below is a
-- CREATE OR REPLACE of the view migrate_bowl_weight.sql introduces, and its
-- body reads meal_food_mapping.bowl_weight_g and last_served_meal() -- both
-- of which that migration adds.
-- ---------------------------------------------------------------------
do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'meal_food_mapping'
                    and column_name = 'bowl_weight_g') then
    raise exception
      'Run supabase/migrate_bowl_weight.sql FIRST. This migration rewrites '
      'slot_quantity, whose body reads meal_food_mapping.bowl_weight_g and '
      'public.last_served_meal() -- both added by that file. Nothing has been '
      'changed; re-run this one afterwards.';
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. devices.kind -- the discriminator.
--
-- A HARDWARE FACT FIXED AT REGISTRATION, never a setting. It says what the
-- installation MEASURES, which decides which columns of device_status are a
-- measurement and which are simply absent. Nothing in the firmware sends it
-- and no device may write it -- `devices` has no anon policy at all.
--
-- DEFAULT 'stack', so every row that exists today is correct without being
-- rewritten. That is what makes this safe mid-trial: 24 registered bowl
-- counters keep their meaning and the column is true of them the moment it
-- appears.
--
-- The vocabulary is a closed lowercase enum pinned by CHECK, like location and
-- stack_status. 'stack' echoes stack_count/stack_status, which are exactly the
-- columns a 'scale' leaves NULL.
-- ---------------------------------------------------------------------
alter table public.devices
  add column if not exists kind text not null default 'stack';

do $$
begin
  alter table public.devices
    add constraint devices_kind_ck check (kind in ('stack','scale'));
exception when duplicate_object then null;
end $$;

comment on column public.devices.kind is
  'What this installation MEASURES. stack = four ToF sensors up a pipe, '
  'reporting a bowl count; scale = three load cells under a platform, '
  'reporting grams. Fixed at registration, never a setting. The views use it '
  'to decide which columns of device_status are a measurement and which are '
  'absent -- a scale leaves every stack_* column NULL, and a stack leaves '
  'every weight_* column NULL.';

-- ---------------------------------------------------------------------
-- 2. device_status -- the weight columns.
--
-- device_status is ALREADY the right home for this and status_events is
-- already the wrong one, which is why current state goes here and history
-- waits for its own table. Every payload column here is nullable by
-- construction (the row is created at registration, before the device has
-- ever spoken) and `reported` -- not a non-null value -- is what separates
-- "never heard from" from "reported zero". status_events is the opposite:
-- stack_count, stack_status, levels, sensors_ok, sensors_online and reason are
-- all NOT NULL, so a load cell could only insert there by fabricating a bowl
-- count. docs/PHASE1_BOWL_WEIGHT.md says exactly this at Phase 4.
--
-- THE STATE IS ALWAYS KNOWN; THE NUMBER IS NOT. That inversion is the whole
-- design. A scale that has never seen a known mass has counts and no grams --
-- deliberately, because inventing a factor produces a confident wrong weight,
-- which is the one failure this codebase refuses everywhere. So weight_state
-- is what the device always sends, and weight_g is present only when the state
-- is 'ok'. The CHECK below makes that structural rather than a convention.
--
-- ZERO IS A REAL WEIGHT, and it must never collide with "unknown". An empty,
-- tared, calibrated platform reads 0 g and that is a measurement -- it means
-- the counter is empty and needs refilling. NULL means nobody knows. The two
-- send staff to opposite places, so the schema keeps them apart the same way
-- bowls_trusted already does.
--
-- GRAMS, integer, for the reason migrate_bowl_weight.sql gives: the dashboard
-- sums products across several devices and areas per slot, and binary floating
-- point puts visible drift on a screen showing one decimal. The UI divides by
-- 1000 exactly once, at render.
-- ---------------------------------------------------------------------
alter table public.device_status
  add column if not exists weight_g        integer,
  add column if not exists weight_state    text,
  add column if not exists cells_online    smallint,
  add column if not exists counts_per_gram numeric(9,3),
  add column if not exists net_counts      bigint;

-- The vocabulary, in the priority order the firmware reports it: the fault
-- NEAREST THE HARDWARE wins, because that is the one somebody can act on.
--
--   no_cells       no converter is producing conversions at all
--   cells_partial  fewer than three are. Cells under one platform SUM, so a
--                  missing cell makes the total silently LOW rather than
--                  noisy -- which is why this is not a usable weight
--   over_range     a converter has hit the end of its 24-bit range and is
--                  reporting its own ceiling, so the total stopped rising
--   settling       the automatic power-up tare has not concluded yet
--   uncalibrated   no counts-to-gram factor on this unit; there are no grams
--   untared        calibrated, so the number is right about CHANGE and wrong
--                  about the absolute -- it still includes the platform
--   ok             a weight
do $$
begin
  alter table public.device_status
    add constraint device_status_weight_state_ck
    check (weight_state is null or weight_state in
           ('ok','uncalibrated','untared','settling',
            'cells_partial','no_cells','over_range'));
exception when duplicate_object then null;
end $$;

-- THE HONESTY RULE, MADE STRUCTURAL. A gram figure exists exactly when the
-- state says it does -- not merely usually. Without this, a firmware bug that
-- published weight_g alongside 'uncalibrated' would put an invented number on
-- the Master Dashboard and nothing would object; and 'ok' with a NULL weight
-- would be a state claiming a measurement it did not carry.
--
-- Written as an equality of two booleans rather than two separate CHECKs so
-- both directions fail loudly at the edge, as a 400 from PostgREST.
do $$
begin
  alter table public.device_status
    add constraint device_status_weight_agrees_ck
    check (weight_state is null
           or (weight_state = 'ok') = (weight_g is not null));
exception when duplicate_object then null;
end $$;

-- Sanity rails, not policy -- the same role bowl_weight_g's 100..50000 plays.
-- Three 20 kg cells is 60 kg of platform rating, so 100 kg is unreachable by
-- any real load and identifies a units mix-up or a wild calibration factor.
-- The negative floor exists because a tared platform legitimately reads a few
-- grams below zero as the cells drift with temperature, and because removing
-- something that was on the platform at tare time is a real thing to do; -5 kg
-- is far beyond either and means the tare and the load disagree about which
-- platform they are on.
--
-- COUPLED TO FIRMWARE, exactly as battery_mv's 0..6000 bound is: a value the
-- device can produce but this CHECK rejects is not a rejected weight, it is a
-- 400 that fails the whole PATCH -- so the station would stop reporting
-- anything at all, including the state that would have explained why.
do $$
begin
  alter table public.device_status
    add constraint device_status_weight_range_ck
    check (weight_g is null or weight_g between -5000 and 100000);
exception when duplicate_object then null;
end $$;

do $$
begin
  alter table public.device_status
    add constraint device_status_cells_online_ck
    check (cells_online is null or cells_online between 0 and 8);
exception when duplicate_object then null;
end $$;

comment on column public.device_status.weight_g is
  'What is on the platform, in grams, net of the platform zero and the '
  'session tare. Present ONLY when weight_state = ''ok'' -- enforced by '
  'device_status_weight_agrees_ck. NULL means no trustworthy weight; 0 means '
  'a measured, empty platform. Those are different facts and must not be '
  'collapsed.';

comment on column public.device_status.weight_state is
  'Why weight_g is or is not a number. One of ok / uncalibrated / untared / '
  'settling / cells_partial / no_cells / over_range, reported in that '
  'priority order with the fault nearest the hardware winning. Always sent, '
  'even when weight_g is NULL -- it is the half of the report that is always '
  'knowable.';

comment on column public.device_status.cells_online is
  'Load cells currently producing conversions, of three. Fewer than all of '
  'them is NOT a partial weight: cells under one platform sum, so a missing '
  'cell makes the total low rather than noisy.';

comment on column public.device_status.counts_per_gram is
  'The calibration factor in force on this unit, or NULL if it has never been '
  'calibrated. Published so a wrong weight can be told apart from a wrong '
  'factor without visiting the station.';

comment on column public.device_status.net_counts is
  'Raw converter counts summed over the online cells, with the platform zero '
  'and the session tare already subtracted -- the figure the panel shows when '
  'it has no calibration. Sent in every state but no_cells, so there is '
  'always evidence the cells are moving even when there is no gram figure.';

-- ---------------------------------------------------------------------
-- 3. The anon write path -- extended, not replaced.
--
-- Column grants ACCUMULATE, so this is a second GRANT on the same table and
-- re-running it is a no-op. Nothing else on this path changes:
--
--   * NO policy change. device_status_update_device is
--     `for update to anon using (true) with check (true)` -- policies scope
--     ROWS and grants scope COLUMNS, so columns added later are already
--     covered.
--   * `grant select (device_id) ... to anon` deliberately stays at one column.
--     A device still cannot read its own weight back, exactly as it cannot
--     read its own bowl count. Smoke assertion 26 is what proves that stayed
--     true.
--   * `grant select on ... device_status to authenticated` is TABLE-level, so
--     the dashboard sees the new columns with no change at all.
-- ---------------------------------------------------------------------
grant update (weight_g, weight_state, cells_online, counts_per_gram, net_counts)
  on public.device_status to anon;

-- ---------------------------------------------------------------------
-- 4. device_overview -- the discriminator and the weight, APPENDED.
--
-- CREATE OR REPLACE VIEW may only ADD columns at the end; it cannot drop,
-- reorder or retype. schema.sql already records that convention on
-- missed_last_service, so the new columns go after it and schema.sql is
-- edited to match exactly -- otherwise the next from-scratch rebuild produces
-- a different column order from the migrated database.
--
-- NOTE FOR ANYONE RE-RUNNING weekly_menu_and_offline.sql: that file carries
-- its own full copies of this view and of slot_overview, because it rewrote
-- both. It has been updated in step with this migration. If you add a column
-- here and not there, running it again silently reverts you -- which is the
-- fork its own header warns about.
--
-- DROPPED AND RECREATED, not replaced -- this view and slot_overview below.
-- migrate_buffer.sql appends columns to both, and so does schema.sql, so a
-- database that has them would refuse this file's CREATE OR REPLACE with
-- 42P16 "cannot drop columns from view": a re-run of apply_loadcell.sql, or a
-- fresh rebuild from schema.sql followed by it, would abort at part 2. Nothing
-- in the database reads either view, so dropping them is free, and
-- migrate_buffer.sql -- the last part of apply_loadcell.sql -- appends its
-- columns again in the same transaction. The grants are restated below
-- because a dropped view takes its privileges with it.
--
-- The price: run ON ITS OWN after migrate_buffer.sql, this file now removes
-- the buffer columns from both views (and from slot_quantity, which it always
-- dropped) until migrate_buffer.sql runs again. Re-run apply_loadcell.sql
-- instead, which ends with it.
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

       -- Appended below the existing list, in one block, so the diff against
       -- the pre-migration body is purely additive and a reader can see at a
       -- glance which half of this view describes which product.
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

-- ---------------------------------------------------------------------
-- 5. slot_overview -- stop counting scales as stacks.
--
-- THIS IS THE CHANGE THAT MATTERS MOST, and it is the one that would have been
-- missed by adding the column and nothing else. Both stock views derive the
-- device count and the capacity from a bare `count(*)` over every device at
-- the (location, food_slot). A load cell sharing a served position is a fourth
-- device there -- so capacity would jump from 12 to 16 bowls, the progress bar
-- would read 9 of 16 instead of 9 of 12, and the numerator would be RIGHT the
-- whole time, because a scale writes NULL to stack_count and is already
-- excluded from the sum. A wrong denominator with a correct numerator is the
-- hardest kind of wrong to notice: the bar simply reads a bit low, forever.
--
-- The filters are what keep "how many bowl counters serve this dish position"
-- answering that question rather than "how many boxes are bolted here".
--
-- Dropped and recreated for the reason given above device_overview.
-- ---------------------------------------------------------------------
drop view if exists public.slot_overview;

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

-- Restated because both views were dropped above, and a dropped view takes
-- its grants with it. Same posture as schema.sql section 9.
revoke all on public.device_overview from anon, authenticated, public;
revoke all on public.slot_overview   from anon, authenticated, public;
grant select on public.device_overview to authenticated;
grant select on public.slot_overview   to authenticated;

-- ---------------------------------------------------------------------
-- 6. weight_mismatch_tolerance -- one threshold, one place.
--
-- Modelled on offline_after(), and for the same reason: a number that decides
-- whether the dashboard raises a flag belongs in exactly one object, where it
-- can be found and changed, rather than inline in a view and again in the
-- client.
--
-- WHAT A MISMATCH MEANS. When a position has BOTH a load cell and bowl
-- counters with a per-bowl weight typed in, there are two independent answers
-- to the same question. Agreement is worth nothing; DISAGREEMENT is the
-- signal, and it has exactly two causes worth chasing -- a miscalibrated cell,
-- or a wrong per-bowl weight in the menu. Neither is visible from either
-- number alone.
--
-- Returns a pair because a single percentage misfires at both ends: at 2 kg,
-- 25% is 500 g and any half-full bowl trips it; at 80 kg an absolute 2 kg
-- gramme band would trip on rounding. The rule is "outside BOTH bands".
--
-- 2000 g is roughly the lightest thing anybody stacks in these bowls, so a
-- difference under one bowl's worth cannot distinguish a calibration error
-- from an integer bowl count. 0.25 is one bowl in four, which is about the
-- resolution a bowl count has when the menu weight is typed by hand.
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
-- was, briefly, the only function in the schema anon could call.
--
-- It returns two constants and leaks nothing, which is exactly why it is worth
-- fixing rather than excusing: the value of a uniform posture is that the one
-- exception does not have to be reasoned about.
revoke all on function public.weight_mismatch_tolerance() from public, anon;
grant execute on function public.weight_mismatch_tolerance() to authenticated;

-- ---------------------------------------------------------------------
-- 7. slot_quantity -- measured weight, and the precedence rule.
--
-- Phase 3 of docs/PHASE1_BOWL_WEIGHT.md. Three things happen here.
--
-- A. THE SAME kind FILTERS AS slot_overview. This view carries its OWN copy of
--    the device count and the capacity arithmetic, in its per_area CTE -- so
--    fixing slot_overview alone would have left the Master Dashboard reading
--    "9 of 16 bowls" while the Stock screen beside it read "9 of 12".
--
-- B. THE PRECEDENCE RULE, DECIDED HERE AND NOWHERE ELSE. A measurement beats
--    an estimate: the estimate is (bowls a sensor counted) x (a number
--    somebody typed), and the measurement is a measurement. Deciding it in the
--    client would mean every screen deciding it again, and two screens that
--    decide separately eventually decide differently.
--
--    IT IS APPLIED PER AREA, never per slot. Slot 3 is Curry at Darshanarthi,
--    Sabzi at Mahatma and Bhaji at Tiffin -- so a load cell at Darshanarthi
--    must not cost Mahatma its estimate, and Mahatma's estimate must not
--    dilute Darshanarthi's measurement. Same sum-then-multiply argument
--    migrate_bowl_weight.sql makes about the weights, one layer up.
--
-- C. THE MISMATCH. Where a position has BOTH a measurement and an estimate
--    they are two independent answers to one question, and only their
--    DISAGREEMENT carries information -- a miscalibrated cell, or a wrong
--    per-bowl weight in the menu. Neither is visible from either number alone
--    and neither surfaces as a fault anywhere else.
--
--    Compared PAIRWISE, over the areas that have both. Comparing slot totals
--    would flag every mixed slot: one measured hall against a total that
--    includes estimated halls is not a comparison, it is two different sums.
-- ---------------------------------------------------------------------
-- DROPPED, not replaced: this removes weight_source and the three mismatch
-- columns, and CREATE OR REPLACE VIEW may only append.
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
           -- THE HALL'S OWN MASS: its buffered bowls PLUS whatever its scales
           -- weigh. Same key, better number. It was briefly the precedence
           -- answer -- measured beats estimated -- which is how a hall with an
           -- empty counter reported 18.0 kg while holding 54.0 kg. Still NULL
           -- when the hall has neither a measurement nor a reading to multiply
           -- -- never 0, which beside a bowl count of "--" is a contradiction
           -- that sends somebody to refill a station nobody has heard from.
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

revoke all on public.slot_quantity from anon, authenticated, public;
grant select on public.slot_quantity to authenticated;

comment on view public.slot_quantity is
  'Master Dashboard source: quantity per dish position across every serving '
  'area, in grams. weight_g is the authoritative figure and is a SUM: '
  'buffer_g (bowls waiting, counted x per-bowl weight) plus counter_g (food '
  'on the scales), because those are different food in different places. It '
  'was once measured-beats-estimated per area, which discarded a hall''s '
  'whole buffer whenever a scale there happened to read empty. est_weight_g '
  'and measured_weight_g are the two inputs, kept so a screen can say which '
  'part it is showing.';

-- #####################################################################
-- ##  PART 3 of 9:  register_loadcells.sql
-- ##  LDC-001..032 into the devices registry
-- #####################################################################

-- THIRTY-TWO, MIRRORING BWL-001..032 ONE FOR ONE. Not twenty, which is what
-- the Phase 2 sketch in docs/PHASE1_BOWL_WEIGHT.md guessed at before the fleet
-- was laid out: LDC-nnn is the load cell at the same physical position as
-- BWL-nnn, so the series has to match or the correspondence has holes in it.
-- assign_loadcells.sql relies on exactly this pairing.
--
-- Only device_id, timezone and kind are named here. Position comes from
-- assign_loadcells.sql, which reads it off the bowl counter of the same number
-- rather than repeating it.
insert into public.devices (device_id, timezone, kind)
select 'LDC-' || lpad(n::text, 3, '0'), 'Asia/Kolkata', 'scale'
  from generate_series(1, 32) as n
 on conflict (device_id) do nothing;

-- Absolute, not conditional, so a row created before migrate_loadcell.sql ran
-- -- and therefore defaulted to 'stack' -- is corrected rather than left
-- misclassified. A scale filed as a stack is worse than an unregistered one:
-- it would add four phantom bowls of capacity to whatever position it sits at,
-- and the numerator would stay right, so the only symptom is a progress bar
-- that reads low forever.
update public.devices
   set kind = 'scale'
 where device_id like 'LDC-%'
   and kind is distinct from 'scale';

-- #####################################################################
-- ##  PART 4 of 9:  assign_loadcells.sql
-- ##  each LDC onto the position of its BWL
-- #####################################################################

update public.devices d
   set location  = b.location,
       food_slot = b.food_slot,
       -- The bowl counter's label with the measurement named, so the two are
       -- recognisably the same position in a device list sorted by label.
       -- Still names the PHYSICAL position and never the dish -- what sits in
       -- slot 3 changes with the meal, and meal_food_mapping owns that.
       label     = case
                     when b.label is null then null
                     when b.location = 'R' then 'Reserved (scale)'
                     else b.label || ' scale'
                   end
  from public.devices b
 where d.device_id like 'LDC-%'
   and d.kind = 'scale'
   -- The pairing. substring from position 5 is the three-digit tail, so
   -- 'LDC-014' finds 'BWL-014'. An LDC with no BWL of the same number simply
   -- does not match and is left unassigned rather than being given a guess.
   and b.device_id = 'BWL-' || substring(d.device_id from 5)
   and (d.location  is distinct from b.location
     or d.food_slot is distinct from b.food_slot);

-- #####################################################################
-- ##  PART 5 of 9:  migrate_weight_samples.sql
-- ##  the analog history a scale appends to
-- #####################################################################

-- ---------------------------------------------------------------------
-- 0. Prerequisite.
-- ---------------------------------------------------------------------
do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'devices'
                    and column_name = 'kind') then
    raise exception
      'Run supabase/migrate_loadcell.sql FIRST -- this table is only '
      'meaningful for devices.kind = ''scale'', and the guard trigger below '
      'reads that column. Nothing has been changed.';
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. The table.
-- ---------------------------------------------------------------------
create table if not exists public.weight_samples (
  -- identity, not bigserial: serial compiles to nextval(), whose ACL is
  -- checked against the invoker, so anon would need GRANT USAGE ON SEQUENCE.
  -- Identity is evaluated internally with that check skipped, and the id
  -- cannot be spoofed by the client either. Same reasoning as status_events.
  id             bigint generated always as identity primary key,

  device_id      text        not null
                   references public.devices(device_id) on delete restrict,

  -- Idempotency without ON CONFLICT: a retried batch violates this and raises
  -- 23505, which the firmware already treats as "already recorded". Granting
  -- anon the SELECT that ON CONFLICT needs would let any device read the whole
  -- fleet's history.
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

  -- THE INVERSION, AND IT IS THE WHOLE DESIGN. The STATE is always known; the
  -- NUMBER is not. A scale that has never seen a known mass has counts and no
  -- grams -- so weight_state is NOT NULL and weight_g is nullable, which is
  -- the opposite way round from every instinct.
  --
  -- NOT NULL here where device_status leaves it nullable, because that row
  -- exists before the device has ever spoken and this one is only ever
  -- created BY a device that is speaking.
  weight_state   text        not null
                   check (weight_state in ('ok','uncalibrated','untared',
                                           'settling','cells_partial',
                                           'no_cells','over_range')),
  weight_g       integer     check (weight_g is null
                                    or weight_g between -5000 and 100000),

  -- The honesty rule, made structural -- and UNCONDITIONAL here, unlike
  -- device_status, because weight_state cannot be null. A gram figure exists
  -- exactly when the state says it does.
  constraint weight_samples_agrees_ck
    check ((weight_state = 'ok') = (weight_g is not null)),

  cells_online   smallint    check (cells_online is null
                                    or cells_online between 0 and 8),

  -- Raw counts net of the platform zero and the session tare. Kept even when
  -- there are no grams: it is the evidence that the cells are alive and
  -- moving, and on an uncalibrated unit it is the ONLY measurement there is.
  net_counts     bigint,
  counts_per_gram numeric(9,3),

  -- Same 6000 mV ceiling as device_status, and coupled to
  -- config::BATTERY_PUBLISH_MAX_MV for the same reason: a value the firmware
  -- can produce but this rejects is a 400 that fails the whole INSERT.
  battery_mv     integer     check (battery_mv is null
                                    or battery_mv between 0 and 6000),
  battery_level  text        check (battery_level in ('good','medium','low','critical')),

  firmware       text        not null,

  constraint weight_samples_once unique (device_id, boot_id, seq)
);

-- The read path the burn-rate question will take: one device, most recent
-- first. Mirrors status_events_device_time_idx exactly.
create index if not exists weight_samples_device_time_idx
  on public.weight_samples (device_id, recorded_at desc);
create index if not exists weight_samples_recorded_at_idx
  on public.weight_samples (recorded_at);

comment on table public.weight_samples is
  'Append-only weight history for devices.kind = ''scale''. Separate from '
  'status_events because that table is NOT NULL on five bowl-shaped columns '
  'a load cell cannot supply. Appended on CHANGE, not on the heartbeat: the '
  'current figure lives in device_status, and this is what makes burn rate '
  'and projected run-out answerable.';

comment on column public.weight_samples.weight_state is
  'Why weight_g is or is not a number. NOT NULL -- the state is always '
  'knowable even when the weight is not, which is the inversion this whole '
  'table is built around.';

-- ---------------------------------------------------------------------
-- 2. Clock-free timestamps.
--
-- The device has no RTC and no guaranteed NTP: it reports how long ago each
-- buffered sample happened and the server supplies the absolute reference.
-- This is what lets offline samples replay with correct times.
--
-- Byte-for-byte the same logic as tg_status_events_stamp, deliberately -- two
-- histories that timestamp differently would be impossible to read together,
-- and reading them together is the entire point of having both.
-- ---------------------------------------------------------------------
create or replace function public.tg_weight_samples_stamp()
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


-- CREATE FUNCTION grants EXECUTE to PUBLIC, and a later GRANT to named roles
-- does NOT take it back -- only an explicit revoke does. Both of these were
-- left with it. Neither is directly callable (Postgres refuses to invoke a
-- trigger function outside a trigger, 0A000) and tg_weight_samples_kind is
-- SECURITY DEFINER, which is exactly the shape of object that should not be
-- sitting there with a PUBLIC grant regardless of today's reachability.
--
-- The real cost of leaving it is to the signal: inventory.sql exists to make
-- "which functions can PUBLIC execute" a question with the answer NONE, and
-- two permanent false positives are how the next genuine one goes unnoticed.
-- apply_loadcell.sql now aborts the whole transaction on a non-empty answer,
-- which is what found these.

revoke all on function public.tg_weight_samples_stamp() from public, anon, authenticated;

drop trigger if exists weight_samples_stamp on public.weight_samples;
create trigger weight_samples_stamp
  before insert on public.weight_samples
  for each row execute function public.tg_weight_samples_stamp();

-- ---------------------------------------------------------------------
-- 3. THE GUARD status_events NEVER HAD.
--
-- A bowl counter must not be able to write here, and the foreign key cannot
-- say so -- it only checks the device exists. A CHECK cannot say so either:
-- it may not reference another table.
--
-- This matters because the whole value of two tables is that each one is
-- ENTIRELY one kind of reading. A stray stack row in here would silently
-- corrupt any burn-rate calculation built on it, and would do so while
-- looking perfectly well-formed.
--
-- The mirror guard on status_events -- rejecting scales -- is deliberately
-- NOT added by this migration. It would be correct, but status_events is the
-- live bowl-counter path with 7,898 rows and 15 devices writing to it, and a
-- new trigger there is a change to a working product for no benefit today: no
-- scale can reach it anyway, because it cannot satisfy the NOT NULLs.
-- ---------------------------------------------------------------------
-- SECURITY DEFINER, and it is not optional. The trigger reads public.devices,
-- and the role doing the INSERT is anon -- which has NO SELECT on that table,
-- deliberately: a device must not be able to enumerate the fleet.
--
-- Without this the guard does not merely fail to guard, it REJECTS EVERY
-- INSERT with 42501 "permission denied for table devices". Caught locally
-- before this reached anything; the symptom in the field would have been a
-- load cell reporting current state perfectly while its history stayed
-- permanently empty, backing off from a permission error nobody was watching
-- for. tg_devices_create_status() carries the same modifier for exactly this
-- reason -- it inserts into device_status on behalf of a caller who cannot.
--
-- Safe because it reads one column of one row by primary key and returns no
-- data to the caller: the only thing it can leak is whether an insert
-- succeeds, which the caller learns anyway.
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

drop trigger if exists weight_samples_kind on public.weight_samples;
create trigger weight_samples_kind
  before insert on public.weight_samples
  for each row execute function public.tg_weight_samples_kind();

-- ---------------------------------------------------------------------
-- 4. Security -- the same posture as status_events.
--
-- INSERT only for anon, on named columns, with no SELECT of any kind: a
-- device can append its own history and cannot read anybody's. The duplicate
-- that idempotency relies on surfaces as 23505 from the unique constraint
-- rather than needing ON CONFLICT, which would require a SELECT grant.
-- ---------------------------------------------------------------------
alter table public.weight_samples enable row level security;

revoke all on public.weight_samples from anon, authenticated, public;

drop policy if exists weight_samples_insert_device on public.weight_samples;
create policy weight_samples_insert_device on public.weight_samples
  for insert to anon with check (true);

drop policy if exists weight_samples_select_staff on public.weight_samples;
create policy weight_samples_select_staff on public.weight_samples
  for select to authenticated using (true);

-- recorded_at and received_at are NOT granted: the trigger sets them, and a
-- device that could write them could backdate its own history.
grant insert (device_id, boot_id, seq, age_ms, reason,
              weight_state, weight_g, cells_online, net_counts,
              counts_per_gram, battery_mv, battery_level, firmware)
  on public.weight_samples to anon;

grant select on public.weight_samples to authenticated;

-- #####################################################################
-- ##  PART 6 of 9:  migrate_burn_rate.sql
-- ##  consumption rate, and when a dish runs out
-- #####################################################################

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
  select d.location, d.food_slot, max(m.bowl_weight_g) as bowl_weight_g,
         -- WHETHER THIS HALL IS SERVING THIS SLOT AT ALL, which `partial` below
         -- needs and did not have. bool_or over the left join gives false --
         -- not NULL -- for a hall with no mapping row, because the all-NULL
         -- side makes `m.food_name is not null` false, so it is safe to test
         -- bare without a coalesce.
         bool_or(m.food_name is not null)      as is_serving
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
-- KEYED BY HALL AS WELL AS SLOT. location was a filter here and never a key,
-- but meal_food_mapping is keyed by (location, meal_type, meal_date,
-- food_slot) -- so slot 3 is a DIFFERENT DISH in each hall, and summing the
-- three together produced one curve, one burn rate and one runs_out_at for
-- three unrelated dishes. The number looked entirely reasonable and described
-- nothing. Adding location to the key gives one series per hall per slot; the
-- dashboard reports the earliest deadline among them.
buffer AS (
  select g.at_ts, d.location, d.food_slot,
         sum(e.stack_count * w.bowl_weight_g)                  as buffer_g,
         -- THREE CONDITIONS, NOT TWO, matching slot_quantity. Without the
         -- third, a hall holding leftover bowls in a slot it is NOT serving
         -- this meal flagged the whole slot's total as partial -- which is the
         -- exact wrong answer: those bowls have no per-bowl weight because
         -- nothing is being served there, not because somebody forgot to type
         -- one. The flag exists for the case it still catches: a hall that IS
         -- serving with no kg per bowl entered.
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
-- COUNTER: the last usable weight each scale had reported at or before each
-- grid point. weight_state = 'ok' is the whole gate -- an uncalibrated or
-- partly-dead station has no grams to add.
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
       -- coalesce to 0 per TERM, not on the sum: a slot with a counter and no
       -- buffered stack has a real total, and so does the reverse. NULL only
       -- when neither side has ever reported.
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
  -- BY HALL AND SLOT. slot_stock_series is one series per (location,
  -- food_slot) now, so partitioning on the slot alone would interleave three
  -- halls' points into one sequence and read every hop between them as a
  -- kitchen delivery.
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
       -- Read straight from the menu, NOT joined from slot_quantity. A view on
       -- a view cannot be dropped without dropping its dependant, and this
       -- chain existed only to borrow a dish name -- which made slot_quantity
       -- un-replaceable the first time it needed a column removed. Two sibling
       -- views over the same tables, never a stack of them.
       -- SCOPED TO THIS HALL now that a row IS a hall. It used to aggregate
       -- every hall's dish for the slot into one array, which was the only
       -- honest thing to do while the row covered all three -- and was also the
       -- clearest symptom that the row should not have.
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
       --
       -- AGAINST THE WINDOW NOW IN PROGRESS, not the last COMPLETED one. This
       -- compared last_service_window_end(), whose body is
       -- `max(w_end) where w_end <= at_ts` -- so DURING service it returned the
       -- close of the PREVIOUS meal, hours in the past, and every runs_out_at
       -- was later than it. The flag was therefore false throughout the only
       -- period it is read in, which is a confident "the food will last" during
       -- lunch on the strength of breakfast having ended.
       --
       -- The scalar subquery below is NULL outside every window, so the flag
       -- goes NULL there rather than false -- which is what the comment above
       -- already promises and what current_meal_type() does for the same
       -- reason. The `+ interval '1 day' * 0` that used to sit here was a
       -- no-op and is gone with it.
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
    -- The most recent grid point, which is "now" for this slot.
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

-- #####################################################################
-- ##  PART 7 of 9:  migrate_manual_fill.sql
-- ##  TRIAL HARNESS -- the manual fill estimate
-- #####################################################################

-- --- 1. prerequisites -------------------------------------------------
-- This sits on top of the load-cell migration; it has nothing to add to a
-- database that has no scales in it.
do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'device_status'
                    and column_name = 'weight_state') then
    raise exception
      'run migrate_loadcell.sql first -- device_status has no weight columns';
  end if;
  if to_regclass('public.weight_samples') is null then
    raise exception
      'run migrate_weight_samples.sql first -- there is no history to append to';
  end if;
end $$;

-- --- 2. the estimate, on current state and on history -----------------
-- NULLABLE AND NULL BY DEFAULT, on every device. Thirty-one load cells and
-- thirty-two bowl counters have no knob, and `0` would say their vessels are
-- empty. The absence of an estimate is not an estimate of zero -- the same
-- rule that keeps weight_g NULL until a scale is calibrated.
alter table public.device_status
  add column if not exists manual_fill_pct smallint,
  add column if not exists manual_fill_age_s integer;

alter table public.weight_samples
  add column if not exists manual_fill_pct smallint;

do $$
begin
  if not exists (select 1 from pg_constraint
                  where conname = 'device_status_manual_fill_ck') then
    alter table public.device_status add constraint device_status_manual_fill_ck
      check (manual_fill_pct is null or manual_fill_pct between 0 and 100);
  end if;
  if not exists (select 1 from pg_constraint
                  where conname = 'weight_samples_manual_fill_ck') then
    alter table public.weight_samples add constraint weight_samples_manual_fill_ck
      check (manual_fill_pct is null or manual_fill_pct between 0 and 100);
  end if;
end $$;

comment on column public.device_status.manual_fill_pct is
  'TRIAL HARNESS. Vessel fullness as judged by eye and dialled in on the knob, '
  '0-100. NULL on every device without one. Never feeds slot_quantity or '
  'slot_burn_rate -- it is the subject of a comparison against weight_g, not an '
  'input to anything the kitchen acts on.';

comment on column public.device_status.manual_fill_age_s is
  'TRIAL HARNESS. Seconds since the attendant last touched the knob. The '
  'staleness IS a result: an estimate nobody refreshes is the failure mode a '
  'knob-based system actually has, so it is recorded rather than hidden.';

-- --- 3. the device may write them -------------------------------------
grant update (manual_fill_pct, manual_fill_age_s)
  on public.device_status to anon;
grant insert (manual_fill_pct)
  on public.weight_samples to anon;

-- --- 4. the vessel capacity -------------------------------------------
-- ITS OWN TABLE, deliberately, rather than a column on meal_food_mapping.
--
-- The obvious place is beside bowl_weight_g, and that is exactly why it is not
-- there: meal_food_mapping is production, it is edited by kitchen staff every
-- day, and it is keyed per location and slot. This is one number for one
-- prototype vessel that will be gone in a few weeks. Widening a live table for
-- a temporary experiment leaves a column nobody dares drop.
--
-- SERVER-SIDE rather than in the browser, because the reviewer, the kitchen and
-- whoever is collating results will each open the dashboard somewhere else and
-- must see the same figure. A number in local storage is a different experiment
-- per browser.
create table if not exists public.trial_vessel_capacity (
  meal_type   text        primary key
              check (meal_type in ('Breakfast','Lunch','Dinner')),
  capacity_g  integer     not null check (capacity_g between 100 and 200000),
  note        text,
  updated_at  timestamptz not null default now()
);

-- updated_at DID NOT ADVANCE ON AN EDIT. The column defaults to now() at
-- insert and nothing moved it afterwards: the dashboard upserts only
-- {meal_type, capacity_g}, so PostgREST's DO UPDATE never touched it.
--
-- A DEDICATED FUNCTION, not the existing tg_meal_food_mapping_touch(): that one
-- assigns new.created_at as well, and this table has no such column, so reusing
-- it would raise on every save. search_path is pinned to '' the same way every
-- other trigger function here is.
--
-- Worth having even though nothing reads the column yet. The capacity is the
-- denominator of every kilogram figure on the trial card, so "when was this
-- last changed" is the first question to ask when two people disagree about
-- what the dashboard said.
create or replace function public.tg_trial_capacity_touch()
returns trigger language plpgsql set search_path = '' as $$
begin
  new.updated_at := now();
  return new;
end $$;

-- LOCKED DOWN LIKE EVERY OTHER TRIGGER FUNCTION HERE. Postgres grants EXECUTE
-- to PUBLIC on a new function by default, and apply_loadcell.sql's own
-- verification refuses to commit when it finds one -- which is exactly how this
-- omission was caught, before it shipped, rather than after.
revoke all on function public.tg_trial_capacity_touch() from public, anon, authenticated;

drop trigger if exists trial_vessel_capacity_touch on public.trial_vessel_capacity;
create trigger trial_vessel_capacity_touch
  before update on public.trial_vessel_capacity
  for each row execute function public.tg_trial_capacity_touch();

comment on table public.trial_vessel_capacity is
  'TRIAL HARNESS. Full-vessel mass per meal, so a manual percentage can be '
  'rendered in kilograms: 50% of an 18 kg vessel is 9 kg. Separate from '
  'meal_food_mapping on purpose -- that table is production and this is a '
  'prototype that will be deleted.';

alter table public.trial_vessel_capacity enable row level security;

revoke all on public.trial_vessel_capacity from anon, authenticated, public;

-- Staff read and write it; devices never see it. The conversion to kilograms
-- happens on the dashboard, not on the board -- a station that knew its vessel
-- capacity could report kilograms it had not measured, which is the one thing
-- the honesty rule exists to prevent.
drop policy if exists trial_capacity_staff on public.trial_vessel_capacity;
create policy trial_capacity_staff on public.trial_vessel_capacity
  for all to authenticated using (true) with check (true);

grant select, insert, update, delete on public.trial_vessel_capacity to authenticated;

-- --- 4b. what the dashboard reads -------------------------------------
-- ITS OWN VIEW RATHER THAN A WIDER device_overview, and the reason is drift.
-- device_overview is defined in THREE files -- schema.sql, migrate_loadcell.sql
-- and weekly_menu_and_offline.sql -- because a live database cannot be rebuilt
-- and each path has to arrive at the same view. Appending a trial column means
-- editing all three and keeping them in step, and the last time two of those
-- copies disagreed the result was a file that could not run at all.
--
-- A separate view costs nothing, joins in one line on the dashboard, and is
-- deleted by the rollback along with everything else.
-- security_invoker, which schema.sql calls mandatory and which every other view
-- in this repo carries. Without it a view runs with the OWNER's rights, so RLS
-- is evaluated as the owner rather than as the caller -- harmless for these
-- three, which are granted to `authenticated` only and select nothing an
-- authenticated user cannot already read, and still wrong to leave off: the
-- next grant added to one of them would quietly bypass a policy.
create or replace view public.trial_manual_fill
  with (security_invoker = true) as
select d.device_id,
       d.location,
       d.food_slot,
       s.manual_fill_pct,
       s.manual_fill_age_s,
       -- The measured figure travels WITH it, so a screen showing both cannot
       -- pair an estimate against a weight from a different poll.
       s.weight_state,
       s.weight_g,
       s.updated_at
  from public.devices d
  join public.device_status s using (device_id)
 where d.kind = 'scale';

comment on view public.trial_manual_fill is
  'TRIAL HARNESS. Current manual estimate beside the current measured weight, '
  'per scale. Separate from device_overview so the trial can be dropped without '
  'touching a view that three migration paths all have to agree about.';

revoke all on public.trial_manual_fill from anon, authenticated, public;
grant select on public.trial_manual_fill to authenticated;

-- --- 5. the comparison ------------------------------------------------
-- ONE ROW PER SAMPLE THAT HAS BOTH NUMBERS, which is the only shape an error
-- analysis can use. Rows with one or the other are excluded rather than
-- coalesced: a missing manual estimate is not 0%, and pairing a measurement
-- against an assumption would manufacture agreement.
create or replace view public.trial_fill_vs_weight
  with (security_invoker = true) as
select s.device_id,
       s.recorded_at,
       d.location,
       d.food_slot,
       s.manual_fill_pct,
       s.weight_g                                        as measured_g,
       c.capacity_g,
       -- What the attendant's estimate implies, in the same unit as the scale.
       (s.manual_fill_pct::numeric / 100) * c.capacity_g  as manual_g,
       -- THE NUMBER THE WHOLE EXERCISE EXISTS TO PRODUCE. Signed, so a
       -- systematic lean -- people rounding up because a full-looking vessel is
       -- the safe answer -- is visible as a bias rather than averaged away by
       -- an absolute value.
       (s.manual_fill_pct::numeric / 100) * c.capacity_g - s.weight_g
                                                          as error_g,
       -- NULL BELOW A FLOOR, because a percentage of almost nothing is not a
       -- percentage. A 9 kg discrepancy against a scale reading 4 g is
       -- +224900%: arithmetically right, and a statement about the denominator
       -- rather than about anybody's judgement.
       --
       -- Two ways to reach it and both are real -- an idle counter with no
       -- vessel on it, or a full-vessel capacity somebody set wrong. Neither is
       -- an estimation error, and leaving them in would wreck any mean taken
       -- over this column: one such row outweighs a thousand honest ones.
       --
       -- error_g is still reported for those rows. The kilograms remain true;
       -- only the ratio stops meaning anything.
       case when s.weight_g >= 200
            then round((((s.manual_fill_pct::numeric / 100) * c.capacity_g
                         - s.weight_g) / s.weight_g) * 100, 1)
       end                                                as error_pct,
       -- So the exclusion is visible rather than looking like missing data, and
       -- so "how often was the counter idle when an estimate was standing?" is
       -- itself answerable -- which is a result about how the knob gets used.
       (s.weight_g < 200)                                 as scale_near_empty
  from public.weight_samples s
  join public.devices d on d.device_id = s.device_id
  -- THE MEAL OF THE SAMPLE, NOT THE MEAL OF THE QUERY. This joined
  -- current_meal_type(d.timezone), whose at_ts defaults to now(), so every
  -- historical row was priced at whatever vessel capacity happens to apply when
  -- somebody opens the page -- and outside every service window, which is about
  -- fifteen hours of the day, it matched nothing at all and error_pct came back
  -- NULL for the entire table.
  --
  -- That is the one view this whole trial exists to produce, so it was silently
  -- answering a different question from the one asked, and answering nothing
  -- for most of the day.
  --
  -- last_served_meal() rather than the two-argument current_meal_type(), and
  -- the difference matters here: it looks BACKWARD to the most recently started
  -- window, so a sample taken in the lull after lunch is still attributed to
  -- lunch instead of dropping out. 36ca30d found 53 of 119 paired rows were
  -- taken with the counter near empty, which is exactly when an attendant is
  -- between services -- so the boundary case is the common case. The lateral
  -- form is the pattern the repo already uses for historical attribution; see
  -- the rationale at schema.sql:1307.
  left join lateral public.last_served_meal(d.timezone, s.recorded_at) lm on true
  left join public.trial_vessel_capacity c
         on c.meal_type = lm.meal_type
 where s.manual_fill_pct is not null
   and s.weight_state = 'ok'
   and s.weight_g is not null
 order by s.recorded_at desc;

comment on view public.trial_fill_vs_weight is
  'TRIAL HARNESS. Manual estimate against measured weight, per sample, with the '
  'signed error. Only rows carrying BOTH figures appear -- a missing estimate is '
  'not 0%, and pairing a measurement with an assumption would manufacture '
  'agreement.';

revoke all on public.trial_fill_vs_weight from anon, authenticated, public;
grant select on public.trial_fill_vs_weight to authenticated;

-- #####################################################################
-- ##  PART 8 of 9:  migrate_vbus_sense.sql
-- ##  mains presence, from the VBUS divider
-- #####################################################################

do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'device_status'
                    and column_name = 'weight_state') then
    raise exception
      'run migrate_loadcell.sql first -- device_status has no weight columns';
  end if;
end $$;

alter table public.device_status
  add column if not exists external_power boolean;

comment on column public.device_status.external_power is
  'Mains present, from the VBUS divider. NOT `charging`: the charger stops when '
  'the cell is full and 5 V remains, so this stays true where charging goes '
  'false. NULL means unreadable -- no divider fitted -- and never "on battery".';

grant update (external_power) on public.device_status to anon;

-- What the dashboard reads. Every device, not only scales: a bowl counter with
-- the divider fitted would report it too, and one without simply carries NULL.
-- security_invoker, as schema.sql requires of every view here. It was omitted
-- when this went in; nothing leaks without it, because the view is granted to
-- `authenticated` only, but a view running with the owner's rights evaluates
-- RLS as the owner and the next grant added would quietly bypass a policy.
create or replace view public.device_power
  with (security_invoker = true) as
select d.device_id,
       d.kind,
       s.external_power,
       s.charging,
       s.battery_mv,
       s.battery_level,
       s.updated_at
  from public.devices d
  join public.device_status s using (device_id);

comment on view public.device_power is
  'Power state per device -- mains presence beside charge state, which are '
  'different facts. Separate from device_overview only because that view is '
  'defined in three files that must agree; folding this in is a follow-up.';

revoke all on public.device_power from anon, authenticated, public;
grant select on public.device_power to authenticated;

-- #####################################################################
-- ##  PART 9 of 9:  migrate_buffer.sql
-- ##  BWL buffer platforms: kind buffer, bowls, kg views
-- #####################################################################

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

-- #####################################################################
-- ##  VERIFICATION -- runs before the commit, and aborts it on failure.
-- #####################################################################
do $$
declare
  v_n int;
  v_txt text;
begin
  -- 1. Every pre-existing device kept its identity and its posting.
  select count(*) into v_n from (
    select device_id, location, food_slot, label, timezone from _before_devices
    except
    select device_id, location, food_slot, label, timezone from public.devices
  ) x;
  if v_n > 0 then
    raise exception 'ABORTED: % existing device row(s) changed or vanished', v_n;
  end if;

  select count(*) into v_n from (
    select * from _before_status
    except
    select device_id, reported, boot_id, uptime_s, stack_count, stack_status,
           levels, sensors_ok, sensors_online, battery_mv, battery_level,
           charging, firmware, mac
      from public.device_status
  ) x;
  if v_n > 0 then
    raise exception 'ABORTED: % existing device_status row(s) changed', v_n;
  end if;

  -- 2. THE ONE THAT MATTERS. Every figure the Stock screen renders, unmoved.
  --    A load cell registered at a served position must not become a fourth
  --    bowl counter -- that would inflate capacity with a correct numerator,
  --    which is a bar that reads low forever and looks like nothing at all.
  select count(*) into v_n from (
    (select * from _before_stock
      except
     select location, food_slot, devices, devices_reported, bowls_capacity,
            bowls_trusted, bowls_reported, any_fault, any_degraded
       from public.slot_overview)
    union all
    (select location, food_slot, devices, devices_reported, bowls_capacity,
            bowls_trusted, bowls_reported, any_fault, any_degraded
       from public.slot_overview
      except
     select * from _before_stock)
  ) x;
  if v_n > 0 then
    raise exception
      'ABORTED: % row(s) of slot_overview moved -- the bowl counters are '
      'not supposed to change. Nothing has been committed.', v_n;
  end if;

  -- 3. Nothing deleted anywhere. Only devices and device_status may GROW,
  --    and only by the 32 scales being registered.
  select count(*) into v_n from _before_counts b
   where (select count(*) from public.status_events)     <> b.status_events
      or (select count(*) from public.service_windows)   <> b.service_windows
      or (select count(*) from public.meal_food_mapping) <> b.meal_food_mapping
      or (select count(*) from public.devices)           <  b.devices
      or (select count(*) from public.device_status)     <  b.device_status;
  if v_n > 0 then
    raise exception 'ABORTED: a table gained or lost rows it should not have';
  end if;

  -- 4. No pre-existing device changed what it measures.
  --
  --    COMPARED TO THE SNAPSHOT, not to 'stack'. This used to assert that
  --    every pre-existing device was a bowl counter, which was true until the
  --    buffer cut-over made every BWL a 'buffer' -- after which a re-run of
  --    this file would abort on a database that was exactly right. What the
  --    check is for is "applying this did not reclassify anything", and that
  --    is a comparison, whatever the kinds happen to be.
  select count(*) into v_n from public.devices d
    join _before_devices b using (device_id)
   where d.kind is distinct from b.kind;
  if v_n > 0 then
    raise exception 'ABORTED: % pre-existing device(s) were reclassified', v_n;
  end if;

  -- 5. The device write path did not widen. anon must still be able to read
  --    exactly one column of device_status, whatever columns were added.
  select count(*) into v_n from information_schema.column_privileges
   where table_schema='public' and table_name='device_status'
     and grantee='anon' and privilege_type='SELECT';
  if v_n <> 1 then
    raise exception
      'ABORTED: anon can now SELECT % columns of device_status, expected 1', v_n;
  end if;

  -- 6. No function became callable by PUBLIC. A fresh CREATE FUNCTION carries
  --    that by default and a bare GRANT does not remove it -- which is exactly
  --    how weight_mismatch_tolerance briefly became the only one in the schema
  --    that anon could call.
  select string_agg(proname, ', ') into v_txt from (
    select p.proname from pg_proc p
     cross join lateral aclexplode(coalesce(p.proacl, acldefault('f', p.proowner))) a
     where p.pronamespace = 'public'::regnamespace
       and a.privilege_type = 'EXECUTE' and a.grantee = 0
     group by p.proname
  ) x;
  if v_txt is not null then
    raise exception 'ABORTED: function(s) executable by PUBLIC: %', v_txt;
  end if;

  raise notice 'All verification checks passed. Committing.';
end $$;

-- ---------------------------------------------------------------------
--  The report. One result set, because the Supabase editor shows only the
--  last statement's output.
-- ---------------------------------------------------------------------
select item as step, name as detail, status from (
  values
    -- BWL-* by kind rather than "kind = 'stack'": after the cut-over they are
    -- buffers, and a report reading "0 registered" would look like a wiped
    -- registry.
    (1, 'BWL-* (bowl counters / buffer platforms)',
        (select count(*) filter (where kind = 'stack') || ' stack, '
                || count(*) filter (where kind = 'buffer') || ' buffer'
           from public.devices where device_id like 'BWL-%')
          || ' -- no kind changed'),
    (2, 'load cells (LDC-*)',
        (select count(*)::text from public.devices where kind = 'scale')
          || ' registered, '
          || (select count(*)::text from public.devices
               where kind='scale' and location in ('D','M','T')) || ' deployed'),
    (3, 'Stock screen (slot_overview)',
        (select count(*)::text from public.slot_overview)
          || ' dish positions, every figure identical to before'),
    (4, 'Master screen (slot_quantity)',
        'view present, ' ||
        (select count(*)::text from information_schema.columns
          where table_schema='public' and table_name='slot_quantity')
          || ' columns'),
    (5, 'measured weight',
        'no station has reported one yet -- expected until a scale is flashed'),
    (6, 'next',
        'run supabase/smoke_test.sql -- expect 40 assertions, 0 FAIL; then, '
        'when the web is live, supabase/cutover_buffers.sql')
) as t(item, name, status)
order by item;

-- CHANGE THIS TO rollback; TO REHEARSE WITHOUT KEEPING ANYTHING.
commit;
