-- =====================================================================
--  Diagnose smoke-test failures
--
--  Run as owner in the Supabase SQL editor. Returns ONE result set covering
--  every check -- the editor only displays the last statement's output, so a
--  file of separate SELECTs would show section 7 and silently discard the rest.
--
--  Read it top to bottom; anything marked <<< is wrong.
-- =====================================================================

with
-- 1. RLS must be on for every table.
rls as (
  select 1 as sec,
         'RLS' as category,
         c.relname as item,
         c.relrowsecurity::text as value,
         case when c.relrowsecurity then '' else '<<< RLS OFF - run enable_rls.sql' end as problem
    from pg_class c
    join pg_namespace n on n.oid = c.relnamespace
   where n.nspname = 'public'
     and c.relname in ('devices','device_status','status_events','service_windows')
),

-- 2. Table-level grants held by anon.
--    EXPECTED, and nothing else:
--      device_status -> INSERT, UPDATE
--      status_events -> INSERT
--      devices       -> nothing
--    On Supabase every new public table starts with ALL granted to anon via
--    ALTER DEFAULT PRIVILEGES, so anything extra here is a REVOKE that did not
--    take -- and that alone would let a device read your whole database.
grants as (
  select 2, 'anon grant', g.table_name, g.privilege_type,
         case
           when g.table_name = 'device_status'
                and g.privilege_type in ('INSERT','UPDATE') then ''
           when g.table_name = 'status_events'
                and g.privilege_type = 'INSERT' then ''
           else '<<< UNEXPECTED - revoke this'
         end
    from information_schema.role_table_grants g
   where g.grantee = 'anon' and g.table_schema = 'public'
),

-- 3. Column-level grants (status_events is granted per column, which is why it
--    may not appear in section 2 at all).
colgrants as (
  select 3, 'anon column grant', c.table_name,
         count(*)::text || ' columns: ' ||
         string_agg(distinct c.privilege_type, ','), ''
    from information_schema.column_privileges c
   where c.grantee = 'anon' and c.table_schema = 'public'
   group by c.table_name
),

-- 4. Policies. Absent policies with RLS on means deny-all, which is correct
--    for anon on `devices`.
policies as (
  select 4, 'policy', p.tablename || ' / ' || p.policyname,
         p.cmd || ' to ' || array_to_string(p.roles, ','), ''
    from pg_policies p
   where p.schemaname = 'public'
),

-- 5. Triggers. Both are load-bearing:
--      device_status_stamp -> keeps updated_at current on the UPDATE half of
--                             an upsert, where a column DEFAULT never fires
--      status_events_stamp -> turns age_ms into recorded_at, which is what
--                             lets a device with no RTC replay buffered events
triggers as (
  select 5, 'trigger', t.event_object_table || ' / ' || t.trigger_name,
         t.action_timing || ' ' || t.event_manipulation, ''
    from information_schema.triggers t
   where t.trigger_schema = 'public'
),
trigger_check as (
  select 5, 'trigger', 'MISSING: ' || x.want, '', '<<< trigger absent'
    from (values ('device_status_stamp'), ('status_events_stamp')) as x(want)
   where not exists (
     select 1 from information_schema.triggers t
      where t.trigger_schema = 'public' and t.trigger_name = x.want)
),

-- 6. Service windows and what they say right now.
windows as (
  select 6, 'service window',
         coalesce(w.device_id, '(fleet default)') || ' ' || w.label,
         w.starts_at::text || ' - ' || w.ends_at::text, ''
    from public.service_windows w
),
now_state as (
  select 6, 'service window', 'local time now',
         (now() at time zone 'Asia/Kolkata')::time::text ||
         ' -> ' ||
         case when public.in_service_window(now(), 'Asia/Kolkata')
              then 'IN service' else 'outside service' end, ''
),

-- 7. Row counts, to confirm the smoke test cleaned up after itself.
counts as (
  select 7, 'row count', 'devices',       count(*)::text, '' from public.devices
  union all
  select 7, 'row count', 'device_status', count(*)::text, '' from public.device_status
  union all
  select 7, 'row count', 'status_events', count(*)::text, '' from public.status_events
),
leftovers as (
  select 7, 'row count', 'BWL-SMOKETEST leftovers', count(*)::text,
         case when count(*) > 0 then '<<< smoke test did not clean up' else '' end
    from public.devices where device_id = 'BWL-SMOKETEST'
)

select sec, category, item, value, problem
  from (
    select * from rls
    union all select * from grants
    union all select * from colgrants
    union all select * from policies
    union all select * from triggers
    union all select * from trigger_check
    union all select * from windows
    union all select * from now_state
    union all select * from counts
    union all select * from leftovers
  ) t
 order by sec, category, item, value;

-- ---------------------------------------------------------------------
--  Fixes
--
--  RLS off               -> run supabase/enable_rls.sql
--  Unexpected anon grant -> re-run the REVOKE/GRANT block from schema.sql
--                           section 7 (revoke first, then grant the minimum)
--  Trigger absent        -> re-run schema.sql section 4
-- ---------------------------------------------------------------------
