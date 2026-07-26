-- =====================================================================
--  Bowlstack -- diagnose the schema
--
--  Run as owner in the Supabase SQL editor when something is wrong: a smoke-test
--  failure, a device that reports nothing, a dashboard with no dish names.
--
--  Read it top to bottom. Anything marked <<< is wrong; everything else is
--  informational. Returns ONE result set, because the editor displays only the
--  last statement's output -- a file of separate SELECTs would show the final
--  section and silently discard the rest.
--
--  Read-only. Changes nothing.
-- =====================================================================

with

-- 1. Every object the schema should have created.
--    Listed explicitly rather than counted, so a missing one names itself
--    instead of showing up as a number that is one too small.
expected_objects(kind, name) as (
  values ('table','devices'),          ('table','device_status'),
         ('table','status_events'),    ('table','service_windows'),
         ('table','meal_food_mapping'),
         ('view','device_overview'),   ('view','slot_overview'),
         ('function','in_service_window'),
         ('function','current_meal_type'),
         ('function','current_meal_date'),
         ('function','meal_mapping_preload'),
         ('function','tg_devices_create_status'),
         ('function','tg_device_status_stamp'),
         ('function','tg_status_events_stamp'),
         ('function','tg_meal_food_mapping_touch'),
         ('trigger','devices_create_status'),
         ('trigger','device_status_stamp'),
         ('trigger','status_events_stamp'),
         ('trigger','meal_food_mapping_touch')
),
present_objects(kind, name) as (
  select 'table', c.relname from pg_class c
    join pg_namespace n on n.oid = c.relnamespace
   where n.nspname = 'public' and c.relkind = 'r'
  union all
  select 'view', c.relname from pg_class c
    join pg_namespace n on n.oid = c.relnamespace
   where n.nspname = 'public' and c.relkind = 'v'
  union all
  select 'function', p.proname from pg_proc p
    join pg_namespace n on n.oid = p.pronamespace
   where n.nspname = 'public'
  union all
  select 'trigger', t.tgname from pg_trigger t
   where not t.tgisinternal
),
objects as (
  select 1 as sec, 'object' as category,
         e.kind || ' ' || e.name as item,
         case when p.name is null then 'MISSING' else 'present' end as value,
         case when p.name is null
              then '<<< absent - re-run schema.sql' else '' end as problem
    from expected_objects e
    left join present_objects p on p.kind = e.kind and p.name = e.name
),

-- 2. RLS. The GRANTs below would still deny reads on their own, but that is one
--    layer instead of two, and Supabase's linter flags any public table exposed
--    through PostgREST without it.
rls as (
  select 2, 'RLS', c.relname, c.relrowsecurity::text,
         case when c.relrowsecurity then ''
              else '<<< RLS OFF - re-run schema.sql section 7' end
    from pg_class c join pg_namespace n on n.oid = c.relnamespace
   where n.nspname = 'public'
     and c.relname in ('devices','device_status','status_events',
                       'service_windows','meal_food_mapping')
),

-- 3. TABLE-level grants held by anon. Expected: NONE AT ALL.
--
--    Every privilege the device needs is granted per COLUMN (section 4), so a
--    table-level grant here is a REVOKE that did not take. Supabase bootstraps
--    `alter default privileges in schema public grant all on tables to anon`, so
--    a new table starts fully open -- this section is what catches that.
anon_table_grants as (
  select 3, 'anon table grant', g.table_name, g.privilege_type,
         '<<< UNEXPECTED - anon should hold NO table-level grant'
    from information_schema.role_table_grants g
   where g.grantee = 'anon' and g.table_schema = 'public'
),
anon_table_ok as (
  select 3, 'anon table grant', '(none)', 'correct', ''
   where not exists (
     select 1 from information_schema.role_table_grants g
      where g.grantee = 'anon' and g.table_schema = 'public')
),

-- 4. COLUMN-level grants held by anon. This is the real device write path.
--
--    Expected, and nothing else:
--      device_status  SELECT on device_id ONLY, UPDATE on the 13 payload columns
--      status_events  INSERT on 13 columns
--
--    The narrow SELECT is load-bearing. PostgREST issues
--    PATCH ...?device_id=eq.X, i.e. UPDATE ... WHERE device_id = 'X', and
--    evaluating that WHERE is a read. Granting device_id alone lets the filter
--    work while every telemetry column stays unreadable -- and device_id is a
--    value the device already supplies, so it discloses nothing.
--
--    A SELECT here on any other column is a data leak: the anon key sits in
--    every flash image, so one extra column is readable by anyone holding it.
anon_col_grants as (
  select 4, 'anon column grant',
         c.table_name || ' ' || c.privilege_type,
         count(*)::text || ' col(s): ' ||
           string_agg(c.column_name, ',' order by c.column_name),
         case
           when c.table_name = 'device_status' and c.privilege_type = 'SELECT'
                and count(*) = 1
                and bool_and(c.column_name = 'device_id') then ''
           when c.table_name = 'device_status' and c.privilege_type = 'UPDATE'  then ''
           when c.table_name = 'status_events' and c.privilege_type = 'INSERT'  then ''
           when c.table_name = 'device_status' and c.privilege_type = 'SELECT'
                then '<<< LEAK - anon may SELECT more than device_id'
           else '<<< UNEXPECTED - revoke this'
         end
    from information_schema.column_privileges c
   where c.grantee = 'anon' and c.table_schema = 'public'
   group by c.table_name, c.privilege_type
),

-- 5. What staff hold. The UI reads through the views, and may write only the
--    assignment columns plus the menu. device_id is excluded on purpose: it is
--    the installation's identity and the key every history row hangs off.
staff_grants as (
  select 5, 'authenticated grant', g.table_name,
         string_agg(distinct g.privilege_type, ',' order by g.privilege_type), ''
    from information_schema.role_table_grants g
   where g.grantee = 'authenticated' and g.table_schema = 'public'
   group by g.table_name
),
staff_col_grants as (
  select 5, 'authenticated column grant',
         c.table_name || ' ' || c.privilege_type,
         string_agg(c.column_name, ',' order by c.column_name),
         case when c.privilege_type = 'UPDATE'
               and c.table_name = 'devices'
               and 'device_id' = any(array_agg(c.column_name))
              then '<<< device_id must not be updatable from a UI' else '' end
    from information_schema.column_privileges c
   where c.grantee = 'authenticated' and c.table_schema = 'public'
     and c.table_name = 'devices'
   group by c.table_name, c.privilege_type
),

-- 6. Policies. RLS on with no policy for a role means deny-all, which is the
--    correct state for anon on `devices` and on `meal_food_mapping`.
policies as (
  select 6, 'policy', p.tablename || ' / ' || p.policyname,
         p.cmd || ' to ' || array_to_string(p.roles, ','), ''
    from pg_policies p
   where p.schemaname = 'public'
),

-- 7. Views must run as the INVOKER. Without security_invoker a view executes as
--    its owner and silently bypasses every policy above -- the failure mode is a
--    view that cheerfully returns everything to anyone who can call it.
view_security as (
  select 7, 'view security', c.relname,
         case when 'security_invoker=true' = any(c.reloptions)
              then 'security_invoker' else 'OWNER RIGHTS' end,
         case when 'security_invoker=true' = any(c.reloptions) then ''
              else '<<< bypasses RLS - re-run schema.sql section 9' end
    from pg_class c join pg_namespace n on n.oid = c.relnamespace
   where n.nspname = 'public' and c.relkind = 'v'
),

-- 8. Service windows, and what they say right now. A device is expected to be
--    dark outside these, so `offline` is only ever true inside one.
windows as (
  select 8, 'service window',
         coalesce(w.device_id, '(fleet default)') || ' ' || w.label,
         w.starts_at::text || ' - ' || w.ends_at::text, ''
    from public.service_windows w
),
now_state as (
  select 8, 'service window', 'local time now',
         (now() at time zone 'Asia/Kolkata')::time::text || ' -> ' ||
         case when public.in_service_window(now(), 'Asia/Kolkata')
              then 'IN service' else 'outside service' end ||
         ', meal=' || coalesce(public.current_meal_type('Asia/Kolkata'), 'none'),
         ''
),

-- 9. Deployment. Expect 24 deployed across 15 dish positions, 8 reserved, 0
--    unassigned. Anything else means assign_devices.sql has not run, or has run
--    against a registry that does not match the building.
deployment as (
  select 9, 'deployment', 'devices by location',
         coalesce(location, '(unassigned)') || ': ' || count(*)::text, ''
    from public.devices group by location
),
deployment_check as (
  select 9, 'deployment', 'totals',
         'deployed=' || d.deployed || ' reserved=' || d.reserved ||
         ' unassigned=' || d.unassigned || ' positions=' || d.positions,
         case when d.unassigned > 0
              then '<<< run assign_devices.sql'
              when d.deployed = 0
              then '<<< nothing deployed; slot_overview will be empty'
              else '' end
    from (
      select count(*) filter (where location in ('D','M','T'))       as deployed,
             count(*) filter (where location = 'R')                   as reserved,
             count(*) filter (where location is null)                 as unassigned,
             count(distinct (location, food_slot))
               filter (where location in ('D','M','T'))              as positions
        from public.devices
    ) d
),

-- 10. Menu coverage for the CURRENT meal. This is the difference between a
--     dashboard showing "Rice" and one showing a blank where the dish should be,
--     and the cause is almost always that nobody entered today's menu rather
--     than anything broken.
menu as (
  select 10, 'menu', 'mapped slots (all dates)',
         count(*)::text || ' rows over ' ||
           count(distinct meal_date)::text || ' date(s)', ''
    from public.meal_food_mapping
),
menu_gaps as (
  select 10, 'menu',
         'unmapped now: ' || d.location || '/' || d.food_slot,
         'no dish for ' || coalesce(public.current_meal_type(d.timezone), 'no meal now'),
         case when public.current_meal_type(d.timezone) is null then ''
              else '<<< dashboard will show a blank dish here' end
    from public.devices d
   where d.location in ('D','M','T') and d.food_slot is not null
     and not exists (
       select 1 from public.meal_food_mapping m
        where m.location  = d.location
          and m.food_slot = d.food_slot
          and m.meal_date = public.current_meal_date(d.timezone)
          and m.meal_type = public.current_meal_type(d.timezone))
   group by d.location, d.food_slot, d.timezone
),

-- 11. Row counts, and whether the smoke test cleaned up after itself.
counts as (
  select 11, 'row count', 'devices',           count(*)::text, '' from public.devices
  union all
  select 11, 'row count', 'device_status',     count(*)::text, '' from public.device_status
  union all
  select 11, 'row count', 'status_events',     count(*)::text, '' from public.status_events
  union all
  select 11, 'row count', 'meal_food_mapping', count(*)::text, '' from public.meal_food_mapping
  union all
  select 11, 'row count', 'reported at least once',
         count(*)::text, '' from public.device_status where reported
),
leftovers as (
  select 11, 'row count', 'smoke-test leftovers', count(*)::text,
         case when count(*) > 0 then '<<< smoke test did not clean up' else '' end
    from public.devices
   where device_id in ('BWL-SMOKETEST','BWL-SMOKE2','BWL-SMOKE3')
      or (location = 'R' and food_slot is not null)
)

select sec, category, item, value, problem
  from (
    select * from objects
    union all select * from rls
    union all select * from anon_table_grants
    union all select * from anon_table_ok
    union all select * from anon_col_grants
    union all select * from staff_grants
    union all select * from staff_col_grants
    union all select * from policies
    union all select * from view_security
    union all select * from windows
    union all select * from now_state
    union all select * from deployment
    union all select * from deployment_check
    union all select * from menu
    union all select * from menu_gaps
    union all select * from counts
    union all select * from leftovers
  ) t
 order by sec, category, item, value;

-- ---------------------------------------------------------------------
--  What to do about it
--
--    object MISSING            re-run schema.sql (it drops and rebuilds)
--    RLS OFF                   re-run schema.sql section 7
--    unexpected anon grant     re-run section 8 -- REVOKE first, then grant the
--                              minimum. A leftover grant is the easiest way to
--                              believe you have RLS and not.
--    view has OWNER RIGHTS     re-run section 9; security_invoker is mandatory
--    unassigned devices        run assign_devices.sql
--    blank dish on dashboard   enter the menu, or run seed_meal_mapping.sql
--    smoke-test leftovers      delete device_id 'BWL-SMOKETEST' by hand
-- ---------------------------------------------------------------------
