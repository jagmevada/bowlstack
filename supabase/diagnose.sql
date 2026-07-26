-- =====================================================================
--  Diagnose smoke-test failures
--
--  Run as owner. Shows the actual privilege state, so a FAIL can be traced to
--  a missing REVOKE, a missing GRANT, or RLS being off -- rather than guessed
--  at from the assertion name.
-- =====================================================================

-- 1. Is RLS on? Every row should show rls_enabled = true.
select c.relname as table_name,
       c.relrowsecurity as rls_enabled
  from pg_class c
  join pg_namespace n on n.oid = c.relnamespace
 where n.nspname = 'public'
   and c.relname in ('devices','device_status','status_events','service_windows')
 order by 1;

-- 2. What can anon actually do?
--    EXPECTED, and nothing else:
--      device_status  -> INSERT, UPDATE
--      status_events  -> INSERT
--      devices        -> (no rows at all)
--    Any SELECT or DELETE row here is a REVOKE that did not take. On Supabase
--    every new public table starts with ALL granted to anon via
--    ALTER DEFAULT PRIVILEGES, so a missed revoke leaves it wide open.
select table_name, privilege_type
  from information_schema.role_table_grants
 where grantee = 'anon'
   and table_schema = 'public'
 order by table_name, privilege_type;

-- 3. Column-level grants (status_events is granted per column).
select table_name, column_name, privilege_type
  from information_schema.column_privileges
 where grantee = 'anon'
   and table_schema = 'public'
 order by table_name, column_name;

-- 4. Policies present.
select tablename, policyname, cmd, roles, qual, with_check
  from pg_policies
 where schemaname = 'public'
 order by tablename, policyname;

-- 5. Triggers that must exist.
--    device_status_stamp  -> keeps updated_at current on upsert
--    status_events_stamp  -> converts age_ms into recorded_at
select event_object_table as table_name,
       trigger_name,
       action_timing,
       event_manipulation
  from information_schema.triggers
 where trigger_schema = 'public'
 order by 1, 2;

-- 6. Does the service-window function exist, and what does it say right now?
select public.in_service_window(now(), 'Asia/Kolkata') as in_service_now,
       (now() at time zone 'Asia/Kolkata')::time        as local_time;

-- 7. Configured windows.
select coalesce(device_id, '(fleet default)') as applies_to,
       label, starts_at, ends_at
  from public.service_windows
 order by device_id nulls first, starts_at;

-- ---------------------------------------------------------------------
--  If section 2 shows SELECT or DELETE for anon, re-run these:
--
--    revoke all on public.devices       from anon, authenticated, public;
--    revoke all on public.device_status from anon, authenticated, public;
--    revoke all on public.status_events from anon, authenticated, public;
--
--    grant insert, update on public.device_status to anon;
--    grant insert (device_id, boot_id, seq, age_ms, reason, stack_count,
--                  stack_status, levels, sensors_ok, sensors_online,
--                  battery_pct, charging, firmware)
--      on public.status_events to anon;
--    grant select on public.devices, public.device_status, public.status_events
--      to authenticated;
--
--  If section 1 shows rls_enabled = false, run supabase/enable_rls.sql.
-- ---------------------------------------------------------------------
