-- =====================================================================
--  Weekly menu template + missed-service visibility
--
--  ADDITIVE and IDEMPOTENT: safe to run on the live database, drops no
--  table, loses no row, and a second run converges to the same state.
--  Run as owner in the Supabase SQL editor. schema.sql has been updated to
--  produce the same result on a fresh rebuild, so nothing here can drift.
--
--  TWO CHANGES, ONE FILE -- because both rewrite the same two views, and
--  two files each holding "the" view body is how definitions fork.
--
--  A. MISSED-SERVICE VISIBILITY.
--     `offline` is gated on in_service_window(now()), which is correct for
--     suppressing between-meal false alarms but means that outside the
--     ~8h20m of daily service the flag is false for EVERY device,
--     unconditionally. A unit that has been dead for six days and a unit
--     between meals were the same row. New per-device flag:
--
--         missed_last_service = reported AND updated_at earlier than the
--                               START of the most recently COMPLETED
--                               service window (device's own timezone)
--
--     Anchoring on the window START, not its end, tolerates staff powering
--     a station down early: a device that reported at any point during the
--     last completed window is not flagged. A device that slept through
--     the whole window is. The two flags are complementary:
--       offline              died mid-window   (acute, in-window alarm)
--       missed_last_service  never showed up   (persists between meals)
--
--     KNOWN LIMIT: a site holiday flags every device until the next served
--     meal, because "the window completed and nobody reported" is exactly
--     what a closure looks like. For this trial that is acceptable -- and
--     arguably true. Revisit if closures become routine (gate the anchor on
--     evidence the service ran, e.g. a menu row for that meal).
--
--  B. WEEKLY MENU TEMPLATE.
--     The menu is fixed per weekday, so staff should enter it once and have
--     each day's meals filled in from it -- while the existing per-date
--     editor keeps working for one-off changes.
--
--     THE ONE DESIGN RULE: the template is configuration that PRODUCES
--     menu rows; it is never itself the menu. slot_overview keeps reading
--     meal_food_mapping and nothing else. A weekday-keyed template has no
--     date, so if the dashboard resolved dishes from it directly, a whole
--     service could pass with a dish name on screen and no dated row in the
--     table -- after which the historical join in docs/meal_mapping.md §6
--     returns NULL for that day forever, and next year's template edit
--     would silently rewrite what "was served" last Tuesday. Not
--     retroactively fixable, which is why the date key exists at all. So
--     the template reaches the dashboard only by being MATERIALISED into
--     meal_food_mapping: by the editor's Save, or by meal_template_apply()
--     over a date range.
--
--  Also fixed here, because the new red "offline" treatment would have
--  drowned in it: in_service_window()'s +/-10 min margin made the whole
--  fleet read as offline for ~10 minutes after every meal (devices
--  legitimately powered down, window still "open") and before every meal
--  (window "open", devices not yet booted). The margin is now a short
--  grace AFTER opening only, and the close is sharp.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 1. in_service_window: asymmetric edges.
--
--    OLD: open from starts_at - margin to ends_at + margin (margin 10 min).
--    NEW: open from starts_at + margin to ends_at, margin default 90 s.
--
--    The old symmetric widening alarmed at both edges of every window: at
--    05:50 the window was "open" but devices had not booted (updated_at =
--    last night -> offline true until first report), and at 21:00-21:10
--    devices were legitimately off but still "expected". Three windows a
--    day, two edges each: the fleet cried wolf six times daily, which
--    trains people to ignore the one alarm that matters.
--
--    `margin` is now the boot grace: how long after the window opens before
--    absence is judged. 90 s covers power-on + WiFi join + first PATCH.
--    The parameter name is unchanged because CREATE OR REPLACE cannot
--    rename parameters; the DEFAULT and the meaning are what changed.
-- ---------------------------------------------------------------------
create or replace function public.in_service_window(
  at_ts  timestamptz,
  tz     text,
  dev_id text     default null,
  margin interval default interval '90 seconds'
) returns boolean language sql stable set search_path = '' as $$
  with local_now as (
    select (at_ts at time zone coalesce(tz, 'UTC'))::time as t
  ),
  applicable as (
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id = dev_id
    union all
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id is null
        and not exists (select 1 from public.service_windows x
                         where x.device_id = dev_id)
  )
  select coalesce(bool_or(
           (select t from local_now) >= (a.starts_at + margin)
       and (select t from local_now) <= a.ends_at), false)
    from applicable a;
$$;

-- ---------------------------------------------------------------------
-- 2. The anchor for missed_last_service: when did the most recently
--    COMPLETED service window start, in absolute time?
--
--    Evaluated in the device's own timezone. (date + time) AT TIME ZONE tz
--    interprets the wall-clock instant in that zone and returns
--    timestamptz, so a 21:00 IST dinner that is already tomorrow in UTC
--    resolves correctly. Today's and yesterday's window instances are
--    enough: service_window_order CHECKs ends_at > starts_at, so every
--    window closes the same local day it opens, and yesterday's dinner is
--    always a completed candidate -- the result is never NULL for a device
--    with any applicable window.
-- ---------------------------------------------------------------------
create or replace function public.last_service_window_start(
  tz     text,
  dev_id text        default null,
  at_ts  timestamptz default now()
) returns timestamptz language sql stable set search_path = '' as $$
  with applicable as (
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id = dev_id
    union all
    select w.starts_at, w.ends_at from public.service_windows w
      where w.device_id is null
        and not exists (select 1 from public.service_windows x
                         where x.device_id = dev_id)
  ),
  local_day as (
    select (at_ts at time zone coalesce(tz, 'UTC'))::date as d
  ),
  candidates as (
    select ((ld.d - n) + a.starts_at) at time zone coalesce(tz, 'UTC') as w_start,
           ((ld.d - n) + a.ends_at)   at time zone coalesce(tz, 'UTC') as w_end
      from applicable a
     cross join local_day ld
     cross join generate_series(0, 1) n
  )
  select max(w_start) from candidates where w_end <= at_ts;
$$;

revoke all on function public.last_service_window_start(text, text, timestamptz)
  from public, anon;
grant execute on function public.last_service_window_start(text, text, timestamptz)
  to authenticated;

-- ---------------------------------------------------------------------
-- 3. meal_menu_template -- what each slot serves, per weekday.
--
--    Mirrors meal_food_mapping's shape and security posture exactly. Keyed
--    by weekday 0-6 where 0 = Sunday: that is Postgres extract(dow) AND
--    JavaScript Date.getDay(), so neither side needs a conversion --
--    isodow (1=Mon..7=Sun) was rejected because the off-by-one serves
--    Monday's menu on Sunday and survives testing until a week boundary.
--
--    Location 'R' is excluded: reserved units occupy no serving position,
--    so a template row there could only ever be a mistake -- and one that
--    would break smoke_test's carry-forward fixture, which deliberately
--    uses 'R' as a location no real menu touches.
-- ---------------------------------------------------------------------
create table if not exists public.meal_menu_template (
  id         bigint generated always as identity,

  location   text     not null check (location in ('D','M','T')),
  weekday    smallint not null check (weekday between 0 and 6),
  meal_type  text     not null check (meal_type in ('Breakfast','Lunch','Dinner')),
  food_slot  smallint not null check (food_slot between 1 and 8),

  -- Same CHECK as the dated table, for the same reason: a blank name is not
  -- a mapping, and clearing a slot is a DELETE.
  food_name  text     not null check (length(btrim(food_name)) > 0),

  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),

  constraint meal_menu_template_pk
    primary key (location, weekday, meal_type, food_slot),
  constraint meal_menu_template_id_key unique (id)
);

comment on table public.meal_menu_template is
  'The fixed weekly menu, per location/weekday/meal/slot. Configuration that '
  'PRODUCES meal_food_mapping rows (via the editor''s Save or '
  'meal_template_apply); never read by the dashboard views directly, so '
  'historical attribution stays anchored to dated rows.';

drop trigger if exists meal_menu_template_touch on public.meal_menu_template;
create trigger meal_menu_template_touch
  before update on public.meal_menu_template
  for each row execute function public.tg_meal_food_mapping_touch();

alter table public.meal_menu_template enable row level security;

-- Devices get nothing here, by the same argument as meal_food_mapping: a
-- unit stores a slot number and never learns the menu, let alone the whole
-- week's. Staff get full CRUD.
do $$ begin
  if not exists (select 1 from pg_policies
                  where schemaname = 'public'
                    and tablename  = 'meal_menu_template'
                    and policyname = 'meal_menu_template_rw_staff') then
    create policy meal_menu_template_rw_staff on public.meal_menu_template
      for all to authenticated using (true) with check (true);
  end if;
end $$;

-- Revoke FIRST: Supabase's ALTER DEFAULT PRIVILEGES grants ALL to anon on
-- every new public table, so a policy without this is RLS you only think
-- you have.
revoke all on public.meal_menu_template from anon, authenticated, public;
grant select, insert, update, delete on public.meal_menu_template to authenticated;

-- ---------------------------------------------------------------------
-- 4. meal_mapping_preload: the editor's draft source, now template-aware.
--
--    Precedence, per (location, meal_type, meal_date):
--      1. saved rows for that exact date        -> is_saved = true
--      2. IF the date is today-or-future: the weekly template for that
--         date's weekday                        -> is_saved = false,
--                                                  source_date = the date
--                                                  itself (the marker the
--                                                  UI reads as "from the
--                                                  template")
--      3. the most recent previous same-meal menu (carry-forward)
--                                               -> is_saved = false,
--                                                  source_date < the date
--
--    The template branch is bounded to today-or-future deliberately: for a
--    PAST gap, what was probably served is what was served around it (carry-
--    forward), not what this week's template says -- the template is
--    present-tense configuration and must not masquerade as history.
-- ---------------------------------------------------------------------
create or replace function public.meal_mapping_preload(
  p_location  text,
  p_meal_type text,
  p_meal_date date
) returns table (
  food_slot   smallint,
  food_name   text,
  source_date date,
  is_saved    boolean
) language sql stable set search_path = '' as $$
  with exact as (
    select m.food_slot, m.food_name, m.meal_date as source_date, true as is_saved
      from public.meal_food_mapping m
     where m.location  = p_location
       and m.meal_type = p_meal_type
       and m.meal_date = p_meal_date
  ),
  templ as (
    select t.food_slot, t.food_name, p_meal_date as source_date, false as is_saved
      from public.meal_menu_template t
     where t.location  = p_location
       and t.meal_type = p_meal_type
       and t.weekday   = extract(dow from p_meal_date)::smallint
       and p_meal_date >= public.current_meal_date('Asia/Kolkata')
  ),
  previous as (
    select m.food_slot, m.food_name, m.meal_date as source_date, false as is_saved
      from public.meal_food_mapping m
     where m.location  = p_location
       and m.meal_type = p_meal_type
       and m.meal_date = (
         select max(m2.meal_date)
           from public.meal_food_mapping m2
          where m2.location  = p_location
            and m2.meal_type = p_meal_type
            and m2.meal_date < p_meal_date
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

-- ---------------------------------------------------------------------
-- 5. meal_template_apply: freeze the template into dated rows.
--
--    "Freeze the week" is this, called with today .. today+6. Rules:
--
--    - Refuses past dates outright. Writing today's template into last
--      month would fabricate history as ordinary, unmarked rows -- the
--      exact corruption the date key exists to prevent.
--    - Skips at MEAL granularity: if ANY row exists for (location, date,
--      meal), that whole meal is left alone. Row-level filling looked
--      friendlier but silently resurrects a slot someone deliberately
--      DELETEd ("no dal today"), and does it as a saved row nobody typed.
--      A meal someone has touched belongs to them.
--    - p_overwrite := true resets each meal in the range to the template
--      exactly (delete then insert). The UI gates it behind an explicit
--      confirmation; the default never destroys anything.
--    - Range capped at 31 days: a fat-fingered year would write ~5k rows.
--
--    Runs as the INVOKER: authenticated's own grants on meal_food_mapping
--    are what authorise the writes, so this adds no privilege anywhere.
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
      -- Nothing in the template for this meal/weekday: report nothing at
      -- all, so the summary counts only meals the template speaks about.
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

      insert into public.meal_food_mapping
             (location, meal_type, meal_date, food_slot, food_name)
      select t.location, t.meal_type, v_date, t.food_slot, t.food_name
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
-- 6. The views, with the new flag. CREATE OR REPLACE keeps every existing
--    column in place; the new column is appended at the end, which is the
--    only shape REPLACE accepts on a live view.
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

       -- Alarm only when the device SHOULD be reporting and is not. Devices
       -- are dark ~16h/day by design, so plain staleness is not a fault.
       (coalesce(s.reported, false)
        and public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at < now() - public.offline_after()) as offline,

       (not coalesce(s.reported, false)) as awaiting_deployment,

       (not public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at is not null) as data_is_stale,

       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_level, s.charging,
       s.uptime_s, s.firmware, s.mac,

       -- Slept through the most recently completed service window. Unlike
       -- `offline` this survives the dark hours: a device dead since
       -- Tuesday stays flagged on Friday morning, instead of becoming
       -- indistinguishable from a healthy unit between meals. Coalesced
       -- because a NULL anchor (a device with no applicable window at all)
       -- must read as "not flagged", not as NULL.
       --
       -- GATED ON DEPLOYMENT: a unit parked at 'R' or stripped of its slot
       -- (the swap-a-failed-board workflow) keeps reported = true with a
       -- frozen updated_at forever, and without this gate it would carry a
       -- permanent fleet-wide alarm that only reset_spares.sql could clear.
       -- A unit that serves no position has no service to miss.
       --
       -- KNOWN LIMITS, both deliberate: a site holiday flags every deployed
       -- device until the next served meal (a closure looks exactly like a
       -- fleet outage, and during a trial arguably is one); and a device
       -- that died MIDWAY through a window is not flagged between that
       -- window's close and the next window's open -- it reported during
       -- the anchored window, and anchoring later (on the window END) would
       -- false-alarm every time staff power a station down early. It was
       -- red while the window ran (`offline`) and goes red again the moment
       -- the next window opens.
       coalesce(coalesce(s.reported, false)
                and d.location in ('D','M','T')
                and d.food_slot is not null
                and s.updated_at <
                    public.last_service_window_start(d.timezone, d.device_id),
                false) as missed_last_service
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone);

create or replace view public.slot_overview
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

       -- Some stack at this position slept through the last completed
       -- window. The stock screen keeps the last count but must say it is
       -- last-known, not live. The deployment gate matches device_overview's
       -- exactly (the WHERE below already restricts to assigned rows; the
       -- gate is restated so the two expressions cannot drift).
       bool_or(coalesce(coalesce(s.reported, false)
               and d.location in ('D','M','T')
               and d.food_slot is not null
               and s.updated_at <
                   public.last_service_window_start(d.timezone, d.device_id),
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

-- The views are new objects as far as fresh grants are concerned only on a
-- rebuild; on REPLACE the existing grants survive. Restated anyway so this
-- file leaves the same state from either starting point.
revoke all on public.device_overview from anon, public;
revoke all on public.slot_overview   from anon, public;
grant select on public.device_overview to authenticated;
grant select on public.slot_overview   to authenticated;

commit;

-- ---------------------------------------------------------------------
--  Verification -- one unioned result set, house style. Expect ALL PASS.
--  Read-only apart from `set local role`, which each block resets.
-- ---------------------------------------------------------------------
do $$ begin end $$;  -- separates the transaction above from the checks below

with checks as (

  -- 1. Both views expose the new columns.
  select 1 as ord, 'device_overview.missed_last_service exists' as what,
         case when exists (select 1 from information_schema.columns
                            where table_schema = 'public'
                              and table_name  = 'device_overview'
                              and column_name = 'missed_last_service')
              then 'PASS' else 'FAIL' end as result
  union all
  select 2, 'slot_overview.any_missed_service exists',
         case when exists (select 1 from information_schema.columns
                            where table_schema = 'public'
                              and table_name  = 'slot_overview'
                              and column_name = 'any_missed_service')
              then 'PASS' else 'FAIL' end
  union all

  -- 2. The anchor resolves for the fleet timezone and is in the past.
  select 3, 'last_service_window_start is non-null and past',
         case when public.last_service_window_start('Asia/Kolkata') < now()
              then 'PASS' else 'FAIL' end
  union all

  -- 3. Window edges: sharp close, 90 s opening grace. Probed at absolute
  --    instants so the answers do not depend on when this file is run.
  select 4, 'in_service_window: 21:05 IST is OUTSIDE (sharp close)',
         case when not public.in_service_window(
                     (current_date + time '21:05') at time zone 'Asia/Kolkata',
                     'Asia/Kolkata')
              then 'PASS' else 'FAIL' end
  union all
  select 5, 'in_service_window: 06:00:30 IST is OUTSIDE (boot grace)',
         case when not public.in_service_window(
                     (current_date + time '06:00:30') at time zone 'Asia/Kolkata',
                     'Asia/Kolkata')
              then 'PASS' else 'FAIL' end
  union all
  select 6, 'in_service_window: 06:02 IST is INSIDE',
         case when public.in_service_window(
                     (current_date + time '06:02') at time zone 'Asia/Kolkata',
                     'Asia/Kolkata')
              then 'PASS' else 'FAIL' end
  union all

  -- 4. Template grants: staff yes, devices no.
  select 7, 'authenticated may SELECT meal_menu_template',
         case when has_table_privilege('authenticated',
                     'public.meal_menu_template', 'select')
              then 'PASS' else 'FAIL' end
  union all
  select 8, 'anon has NO privilege on meal_menu_template',
         case when not has_table_privilege('anon',
                     'public.meal_menu_template', 'select')
               and not has_table_privilege('anon',
                     'public.meal_menu_template', 'insert')
              then 'PASS' else 'FAIL' end
  union all
  select 9, 'anon may not execute meal_template_apply',
         case when not has_function_privilege('anon',
                     'public.meal_template_apply(text, date, date, boolean)',
                     'execute')
              then 'PASS' else 'FAIL' end
  union all

  -- 5. RLS is on.
  select 10, 'meal_menu_template has row security enabled',
         case when (select relrowsecurity from pg_class
                     where oid = 'public.meal_menu_template'::regclass)
              then 'PASS' else 'FAIL' end
)
select ord, what, result from checks
union all
select 99, '== VERDICT ==',
       case when bool_and(result = 'PASS') then 'ALL PASS' else 'FAILURES ABOVE' end
  from checks
order by ord;
