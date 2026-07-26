-- =====================================================================
--  001 -- permanent device assignment + meal-wise food mapping
--
--  Run as owner in the SQL editor. IDEMPOTENT: safe to re-run, and safe to run
--  against a database that has already had it applied. Does not touch telemetry
--  data -- device_status and status_events are untouched, and no firmware change
--  is required or implied.
--
--  WHAT THIS CHANGES AND WHY IT IS NOT COSMETIC
--  --------------------------------------------
--  1. `area` becomes `location`, and gains 'R' for reserved/future stock.
--  2. `item_slot` becomes `food_slot`, and widens from 1-5 to 1-8.
--  3. The UNIQUE index on (area, item_slot) is DROPPED.
--
--  (3) is the substantive one. The old model assumed one stack per serving
--  position, so the pair was unique. The real deployment puts THREE stacks on
--  Darshanarthi slot 1 -- a busy counter needs more than one stack of rice.
--
--  That inverts the primary dashboard number. Remaining stock for a dish is now
--  the SUM of stack_count across every device sharing (location, food_slot), not
--  one device's count. A UI that reads a single device and calls it "Rice
--  remaining" would under-report by 3x on exactly the busiest slots. The new
--  public.slot_overview view exists so that sum is computed in one place.
--
--  4. The pre-existing free-text `location` column is DROPPED to free the name.
--     It is NULL on all 32 rows (register_devices.sql never set it) and was
--     redundant with `label`, so nothing is lost. If a site ever needs a free-text
--     note again, add it back under a name that does not collide.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
--  PART 1 -- devices: location + food_slot
-- ---------------------------------------------------------------------

-- Drop the old free-text location first, so the name is free for the rename
-- below. Guarded on `area` still existing, so a re-run cannot drop the NEW
-- location column: once the rename has happened, `area` is gone and this is a
-- no-op.
do $$
begin
  if exists (select 1 from information_schema.columns
              where table_schema = 'public' and table_name = 'devices'
                and column_name = 'area')
     and exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'devices'
                    and column_name = 'location') then
    alter table public.devices drop column location;
  end if;
end $$;

do $$
begin
  if exists (select 1 from information_schema.columns
              where table_schema = 'public' and table_name = 'devices'
                and column_name = 'area') then
    alter table public.devices rename column area to location;
  end if;
end $$;

do $$
begin
  if exists (select 1 from information_schema.columns
              where table_schema = 'public' and table_name = 'devices'
                and column_name = 'item_slot') then
    alter table public.devices rename column item_slot to food_slot;
  end if;
end $$;

-- Renaming a column does NOT rename its constraints, and the old CHECK still
-- forbids 'R' and slots 6-8. Drop by every name it could carry, then re-add.
alter table public.devices drop constraint if exists devices_area_check;
alter table public.devices drop constraint if exists devices_location_check;
alter table public.devices drop constraint if exists devices_item_slot_check;
alter table public.devices drop constraint if exists devices_food_slot_check;

alter table public.devices
  add constraint devices_location_check
  check (location in ('D', 'M', 'T', 'R'));

alter table public.devices
  add constraint devices_food_slot_check
  check (food_slot between 1 and 8);

-- The constraint that made the new deployment impossible. See the header.
drop index if exists public.devices_position_idx;

comment on column public.devices.location is
  'Serving area: D Darshanarthi, M Mahatma, T Tiffin, R reserved/future. '
  'NULL until deployed.';
comment on column public.devices.food_slot is
  'Dish position 1-8 on the station. NOT unique: several stacks may serve one '
  'slot, so remaining stock for a dish is the SUM over the devices sharing it. '
  'Which food occupies a slot changes per meal -- see meal_food_mapping.';

-- ---------------------------------------------------------------------
--  PART 2 -- meal_food_mapping
--
--  Devices never store food names. A device stores a SLOT; what that slot serves
--  is a property of (area, date, meal) and changes three times a day.
--
--  Keyed by meal_date rather than holding only the current mapping, so history
--  stays attributable: a past bowl count can be joined to the dish that was
--  actually in that slot at the time. Storing only the present would have made
--  every historical count unattributable the moment the menu rotated, and that
--  is not recoverable after the fact.
-- ---------------------------------------------------------------------
create table if not exists public.meal_food_mapping (
  -- Surrogate key alongside the natural one. Redundant, deliberately: PostgREST
  -- and most client tooling want a single-column handle for an update or a
  -- delete, and building it from four columns in every call is worse.
  id         bigint generated always as identity,

  location   text     not null,
  meal_type  text     not null,
  meal_date  date     not null,
  food_slot  smallint not null,
  food_name  text     not null,

  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),

  constraint meal_food_mapping_pk
    primary key (location, meal_date, meal_type, food_slot),
  constraint meal_food_mapping_id_key unique (id),

  constraint meal_food_mapping_location_check
    check (location in ('D', 'M', 'T', 'R')),
  constraint meal_food_mapping_meal_type_check
    check (meal_type in ('Breakfast', 'Lunch', 'Dinner')),
  constraint meal_food_mapping_food_slot_check
    check (food_slot between 1 and 8),

  -- A blank name is not a mapping. Without this, an admin page that submits
  -- empty inputs for untouched slots would fill the table with rows that render
  -- as an unlabelled dish rather than as no dish at all.
  constraint meal_food_mapping_food_name_check
    check (length(btrim(food_name)) > 0)
);

-- Answers "what is in this slot right now" and "what was in it then" -- the
-- lookup the dashboard makes on every render.
create index if not exists meal_food_mapping_lookup_idx
  on public.meal_food_mapping (location, meal_date desc, meal_type);

create or replace function public.tg_meal_food_mapping_touch()
returns trigger language plpgsql set search_path = '' as $$
begin
  new.updated_at := now();
  -- created_at is immutable: an UPDATE must not be able to rewrite when the row
  -- first appeared, or the audit value of the column is nil.
  new.created_at := old.created_at;
  return new;
end $$;

drop trigger if exists meal_food_mapping_touch on public.meal_food_mapping;
create trigger meal_food_mapping_touch
  before update on public.meal_food_mapping
  for each row execute function public.tg_meal_food_mapping_touch();

-- ---------------------------------------------------------------------
--  Which meal is it now?
--
--  service_windows already defines the windows, and they are the same ones that
--  drive `offline` -- so deriving the meal from them keeps one source of truth.
--  Its labels are lowercase ('lunch'); meal_type is capitalised ('Lunch'), so
--  initcap() bridges them. Reads only the fleet-wide rows (device_id is null),
--  because "which meal is it" is a property of the site, not of one unit.
--
--  Returns NULL outside every window, which is the correct answer -- there is no
--  current meal at 3pm, and the UI must show last-known rather than invent one.
-- ---------------------------------------------------------------------
create or replace function public.current_meal_type(
  tz    text        default 'Asia/Kolkata',
  at_ts timestamptz default now()
) returns text language sql stable set search_path = '' as $$
  select initcap(w.label)
    from public.service_windows w
   where w.device_id is null
     and (at_ts at time zone coalesce(tz, 'UTC'))::time
           between w.starts_at and w.ends_at
   order by w.starts_at
   limit 1
$$;

-- The service date in local terms. Not now()::date -- that is the SERVER's date,
-- and a 21:00 dinner in Asia/Kolkata is already the next UTC day.
create or replace function public.current_meal_date(
  tz    text        default 'Asia/Kolkata',
  at_ts timestamptz default now()
) returns date language sql stable set search_path = '' as $$
  select (at_ts at time zone coalesce(tz, 'UTC'))::date
$$;

-- ---------------------------------------------------------------------
--  PART 4 (backend half) -- preload a mapping from the previous same meal
--
--  Opening Lunch for today returns yesterday's Lunch, so the admin edits the
--  differences instead of retyping five dishes. Tomorrow's Breakfast returns
--  today's Breakfast. Nothing at all if there is no history, which the UI shows
--  as an empty form.
--
--  `source_date` is why this is a function and not a plain query: the caller MUST
--  be able to tell an already-saved mapping from a suggestion inherited off an
--  older day. Without it a preloaded form looks identical to a saved one, and an
--  admin who changes nothing and navigates away would believe today's menu was
--  recorded when no row exists.
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
  select * from previous where not exists (select 1 from exact)
  order by food_slot
$$;

-- ---------------------------------------------------------------------
--  Security. RLS and GRANTs are independent gates and BOTH are required.
--  Supabase bootstraps `alter default privileges ... grant all on tables to
--  anon`, so a new table starts fully open -- revoke before granting.
--
--  Devices get nothing here. A device stores a slot number and never learns or
--  needs the menu, so anon has no reason to read or write this table, and giving
--  it access would hand every field unit the whole site's configuration.
-- ---------------------------------------------------------------------
alter table public.meal_food_mapping enable row level security;

drop policy if exists meal_food_mapping_rw_staff on public.meal_food_mapping;
create policy meal_food_mapping_rw_staff on public.meal_food_mapping
  for all to authenticated using (true) with check (true);

revoke all on public.meal_food_mapping from anon, authenticated, public;
grant select, insert, update, delete on public.meal_food_mapping to authenticated;

-- The identity sequence needs no grant: `generated always as identity` skips the
-- sequence ACL check, and the id cannot be supplied by a client.

revoke all on function public.current_meal_type(text, timestamptz)
  from public, anon;
revoke all on function public.current_meal_date(text, timestamptz)
  from public, anon;
revoke all on function public.meal_mapping_preload(text, text, date)
  from public, anon;
grant execute on function public.current_meal_type(text, timestamptz)
  to authenticated;
grant execute on function public.current_meal_date(text, timestamptz)
  to authenticated;
grant execute on function public.meal_mapping_preload(text, text, date)
  to authenticated;

-- The renamed columns must be re-granted: the old grant named `area`,
-- `item_slot` and the dropped free-text `location`.
revoke update on public.devices from authenticated;
grant update (location, food_slot, label, timezone) on public.devices
  to authenticated;

-- ---------------------------------------------------------------------
--  Views. security_invoker is mandatory on both: without it a view runs as its
--  owner and silently bypasses the RLS above.
-- ---------------------------------------------------------------------
drop view if exists public.slot_overview;
drop view if exists public.device_overview;

create view public.device_overview
with (security_invoker = true) as
select d.device_id,
       d.location,
       d.food_slot,
       d.label,
       d.timezone,

       -- What this device's slot is serving RIGHT NOW. NULL outside service
       -- hours, or when nobody has entered a mapping -- both mean "no current
       -- dish", which is not the same as an unnamed one.
       m.food_name        as current_food,
       public.current_meal_type(d.timezone) as current_meal,

       s.reported,
       s.updated_at,
       now() - s.updated_at as stale_for,
       public.in_service_window(now(), d.timezone, d.device_id) as in_service,

       (coalesce(s.reported, false)
        and public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at < now() - interval '5 minutes') as offline,

       (not coalesce(s.reported, false)) as awaiting_deployment,

       (not public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at is not null) as data_is_stale,

       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_level, s.charging,
       s.uptime_s, s.firmware, s.mac
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone);

-- Per-DISH stock, which is the number the kitchen in-charge actually wants.
--
-- This view exists because dropping the unique index made one device's
-- stack_count the wrong answer: three stacks serve Darshanarthi slot 1, so
-- remaining rice is their sum. Computing that in the client would put the same
-- aggregation -- and the same trust rules -- in every screen that shows stock.
--
-- Trust is kept separate from quantity on purpose. `bowls_trusted` counts only
-- devices reporting stack_status 'ok'; a degraded device's count is a lower
-- bound and a discontiguous one is not a count at all, so folding them into the
-- total would silently overstate confidence. The flags say what is wrong; the
-- number says what can be relied on.
create view public.slot_overview
with (security_invoker = true) as
select d.location,
       d.food_slot,
       max(m.food_name)                                        as current_food,
       public.current_meal_type(max(d.timezone))               as current_meal,
       count(*)                                                as devices,
       count(*) filter (where coalesce(s.reported, false))     as devices_reported,

       -- Ceiling for this slot, so a progress bar does not have to hardcode one.
       -- If a fourth stack joins Darshanarthi slot 1 the achievable total rises,
       -- and a UI with "max 12" baked in would silently misreport the busiest
       -- position in the hall.
       (count(*) * 4)::bigint                                  as bowls_capacity,

       -- The trustworthy total: only devices reporting 'ok'. NULL, not 0, when
       -- none of them are -- no data is not an empty counter, and the two call for
       -- opposite actions from the kitchen.
       sum(s.stack_count) filter (where s.stack_status = 'ok') as bowls_trusted,
       sum(s.stack_count)                                      as bowls_reported,
       bool_or(s.stack_status = 'discontiguous')               as any_fault,
       bool_or(s.stack_status = 'degraded')                    as any_degraded,
       bool_or(s.battery_level in ('low', 'critical'))         as any_battery_warn,
       bool_or(coalesce(s.reported, false)
               and public.in_service_window(now(), d.timezone, d.device_id)
               and s.updated_at < now() - interval '5 minutes') as any_offline,
       min(s.updated_at)                                       as oldest_update
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

revoke all on public.device_overview from anon, authenticated, public;
revoke all on public.slot_overview   from anon, authenticated, public;
grant select on public.device_overview to authenticated;
grant select on public.slot_overview   to authenticated;

commit;

-- ---------------------------------------------------------------------
--  Verify
-- ---------------------------------------------------------------------
select column_name, data_type
  from information_schema.columns
 where table_schema = 'public' and table_name = 'devices'
   and column_name in ('location', 'food_slot', 'area', 'item_slot')
 order by column_name;

select 'devices_position_idx dropped' as check,
       not exists (select 1 from pg_indexes
                    where schemaname = 'public'
                      and indexname = 'devices_position_idx') as pass;
