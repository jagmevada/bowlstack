-- =====================================================================
--  Bowlstack -- apply the load-cell augmentation.  GENERATED FILE.
--
--  Regenerate with:  node tools/build_apply_loadcell.mjs
--  Do not edit by hand -- edit the four files it fuses and re-run that.
--
--  ---------------------------------------------------------------------
--  WHAT THIS IS FOR
--  ---------------------------------------------------------------------
--  Paste the whole file into the Supabase SQL editor and run it ONCE. It
--  does what these four do, in the only order that works:
--
--    1. migrate_bowl_weight.sql    per-bowl weight, and the Master Dashboard view
--    2. migrate_loadcell.sql       load-cell stations alongside the bowl counters
--    3. register_loadcells.sql     LDC-001..032 into the devices registry
--    4. assign_loadcells.sql       each LDC onto the position of its BWL
--
--  ---------------------------------------------------------------------
--  WHY IT IS SAFE TO RUN MID-SERVICE
--  ---------------------------------------------------------------------
--  ONE TRANSACTION. All four parts and the verification run inside a single
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
--  existing column changes name, type, nullability or default.
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
create temp table _before_devices on commit drop as
  select device_id, location, food_slot, label, timezone from public.devices;

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
-- ##  PART 1 of 4:  migrate_bowl_weight.sql
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
-- ##  PART 2 of 4:  migrate_loadcell.sql
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
-- ---------------------------------------------------------------------
create or replace view public.slot_overview
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
-- ##  PART 3 of 4:  register_loadcells.sql
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
-- ##  PART 4 of 4:  assign_loadcells.sql
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

  -- 4. Every pre-existing device is still classified as a bowl counter.
  select count(*) into v_n from public.devices d
    join _before_devices b using (device_id)
   where d.kind <> 'stack';
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
    (1, 'bowl counters (BWL-*)',
        (select count(*)::text from public.devices where kind = 'stack')
          || ' registered, unchanged'),
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
        'run supabase/smoke_test.sql -- expect 32 assertions, 0 FAIL')
) as t(item, name, status)
order by item;

-- CHANGE THIS TO rollback; TO REHEARSE WITHOUT KEEPING ANYTHING.
commit;
