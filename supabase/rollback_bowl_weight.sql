-- =====================================================================
--  Undo migrate_bowl_weight.sql.
--
--  Run as owner in the Supabase SQL editor. Returns the database to exactly
--  the shape it had before the weight work: no slot_quantity view, no
--  last_served_meal(), no bowl_weight_g on either menu table, and both menu
--  functions back to their original signatures.
--
--  WHAT THIS DESTROYS
--  ------------------
--  Every per-bowl weight anyone has typed in. Nothing else -- dish names,
--  device rows, telemetry, history and the whole existing dashboard are
--  untouched, because the migration never modified any of them.
--
--  The Master tab keeps working after this: it detects the missing view and
--  says the migration has not been run, rather than blaming the fleet. Stock,
--  Health, Menu and Devices are unaffected either way.
--
--  ORDER MATTERS. meal_mapping_preload() reads bowl_weight_g, so the column
--  cannot be dropped while that function still selects it -- Postgres tracks
--  the dependency and would refuse (or, with CASCADE, silently take the
--  function with it). So the functions are put back FIRST, then the columns
--  go.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 1. The view and the helper. Both are new objects; nothing else reads them.
-- ---------------------------------------------------------------------
drop view if exists public.slot_quantity;
drop function if exists public.last_served_meal(text, timestamptz);

-- ---------------------------------------------------------------------
-- 2. meal_mapping_preload -- back to name-only.
--    DROP then CREATE, for the same reason the migration did: a `returns
--    table` losing a column is an OUT-parameter change, which
--    CREATE OR REPLACE refuses.
-- ---------------------------------------------------------------------
drop function if exists public.meal_mapping_preload(text, text, date);

create function public.meal_mapping_preload(
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
-- 3. meal_template_apply -- stop carrying the weight into dated rows.
--    Only the INSERT narrows; every other rule in this function (refusing
--    the past, meal-granularity skipping, the 31-day cap, running as
--    invoker) is exactly as it was and stays that way.
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
-- 4. The columns, and every weight stored in them.
--    Nothing reads them by this point, so no CASCADE is needed -- and none
--    is used deliberately: if something unexpected still depends on these,
--    this should FAIL loudly rather than quietly dropping whatever it is.
-- ---------------------------------------------------------------------
alter table public.meal_food_mapping
  drop constraint if exists meal_food_mapping_bowl_weight_ck;
alter table public.meal_menu_template
  drop constraint if exists meal_menu_template_bowl_weight_ck;

alter table public.meal_food_mapping  drop column if exists bowl_weight_g;
alter table public.meal_menu_template drop column if exists bowl_weight_g;

commit;

-- PostgREST answers from a cached picture of the schema, so tell it the
-- shape changed -- otherwise it keeps offering slot_quantity and fails on it.
notify pgrst, 'reload schema';

-- ---------------------------------------------------------------------
--  Verify: no view, no helper, no columns. Expect three rows of 0.
-- ---------------------------------------------------------------------
select 'slot_quantity view'  as object,
       count(*)              as remaining
  from information_schema.views
 where table_schema = 'public' and table_name = 'slot_quantity'
union all
select 'last_served_meal()',
       count(*)
  from pg_proc p join pg_namespace n on n.oid = p.pronamespace
 where n.nspname = 'public' and p.proname = 'last_served_meal'
union all
select 'bowl_weight_g columns',
       count(*)
  from information_schema.columns
 where table_schema = 'public' and column_name = 'bowl_weight_g';
