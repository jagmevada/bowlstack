-- =====================================================================
--  Complete inventory of the public schema.
--
--  READ-ONLY. Writes nothing, locks nothing, takes no transaction you could
--  notice. Safe on a live database mid-service.
--
--  WHY IT IS ONE RESULT SET. The Supabase SQL editor displays only the LAST
--  statement's output, so a file with eight SELECTs shows you the eighth and
--  silently discards the seven that mattered. Everything below is therefore
--  one UNION with a `section` column -- ugly to read as SQL, correct to run.
--
--  WHAT IT IS FOR. Run it BEFORE a migration and again AFTER, save both, and
--  diff them. That turns "did we break anything" from a judgement into a
--  comparison: every table, column, view, function, trigger, policy, grant,
--  index and constraint in the schema appears as one line, so anything that
--  changed shows up as a line that changed.
--
--  Row COUNTS are included for the tables a migration could plausibly touch.
--  They are the one part that legitimately differs between two runs on a live
--  database -- devices report while you read this -- so they are marked.
-- =====================================================================

with
-- ---- tables and their shape ----------------------------------------
tabs as (
  select c.relname as tbl
    from pg_class c join pg_namespace n on n.oid = c.relnamespace
   where n.nspname = 'public' and c.relkind = 'r'
),
inv as (

  select '1 table'::text as section,
         t.tbl as object,
         (select count(*)::text from information_schema.columns ic
           where ic.table_schema='public' and ic.table_name=t.tbl) || ' columns'
           || case when c.relrowsecurity then ', RLS on' else ', RLS OFF' end as detail
    from tabs t
    join pg_class c on c.relname = t.tbl
    join pg_namespace n on n.oid = c.relnamespace and n.nspname='public'

  union all
  select '2 column', ic.table_name || '.' || ic.column_name,
         ic.data_type
           || case when ic.is_nullable='NO' then ' NOT NULL' else '' end
           || coalesce(' default ' || ic.column_default, '')
    from information_schema.columns ic
   where ic.table_schema='public'
     and ic.table_name in (select tbl from tabs)

  union all
  select '3 view', v.viewname,
         (select count(*)::text from information_schema.columns ic
           where ic.table_schema='public' and ic.table_name=v.viewname) || ' columns'
    from pg_views v where v.schemaname='public'

  union all
  -- Signature AND volatility AND search_path: a function silently switched from
  -- STABLE to VOLATILE, or one that lost `set search_path = ''`, is a real
  -- change that a name-only listing would miss. The second is a security
  -- property -- every function in this schema pins it.
  select '4 function',
         p.proname || '(' || pg_get_function_identity_arguments(p.oid) || ')',
         case p.provolatile when 'i' then 'immutable' when 's' then 'stable'
                            else 'volatile' end
           || case when p.prosecdef then ', SECURITY DEFINER' else '' end
           || case when exists (select 1 from unnest(coalesce(p.proconfig,'{}'::text[])) cfg
                                 where cfg like 'search_path=%')
                   then ', search_path pinned' else ', SEARCH_PATH NOT PINNED' end
    from pg_proc p
   where p.pronamespace = 'public'::regnamespace

  union all
  select '5 trigger', tg.tgname,
         'on ' || cl.relname || ' -> ' || pr.proname
    from pg_trigger tg
    join pg_class cl on cl.oid = tg.tgrelid
    join pg_namespace n on n.oid = cl.relnamespace and n.nspname='public'
    join pg_proc pr on pr.oid = tg.tgfoid
   where not tg.tgisinternal

  union all
  -- THE SECURITY SURFACE. Policies scope ROWS; the grants below scope COLUMNS.
  -- Both have to be read to know what a role can actually do, which is why they
  -- are separate sections rather than one.
  select '6 policy', pol.polname,
         cl.relname || ': ' || pol.polcmd::text
           || ' to ' || coalesce((select string_agg(r.rolname, '+' order by r.rolname)
                                    from pg_roles r where r.oid = any(pol.polroles)), 'PUBLIC')
    from pg_policy pol
    join pg_class cl on cl.oid = pol.polrelid
    join pg_namespace n on n.oid = cl.relnamespace and n.nspname='public'

  union all
  -- Table-level grants. Anything here that names anon is a whole-table
  -- privilege, which is a bigger thing than the column grants below.
  select '7 grant table', tp.table_name || ' -> ' || tp.grantee,
         tp.privilege_type
    from information_schema.table_privileges tp
   where tp.table_schema='public' and tp.grantee in ('anon','authenticated')

  union all
  -- Column-level grants -- the device write path. This is the section to read
  -- most carefully after any migration: a column accidentally added to a SELECT
  -- grant is how a field unit gains the ability to read the whole site's data.
  select '8 grant column',
         cp.table_name || '.' || cp.column_name || ' -> ' || cp.grantee,
         cp.privilege_type
    from information_schema.column_privileges cp
   where cp.table_schema='public' and cp.grantee in ('anon','authenticated')
     -- Column privileges are also reported for anything covered by a
     -- table-level grant, which would bury the real column grants under every
     -- column of every table `authenticated` can read.
     and not exists (select 1 from information_schema.table_privileges tp
                      where tp.table_schema='public'
                        and tp.table_name = cp.table_name
                        and tp.grantee = cp.grantee
                        and tp.privilege_type = cp.privilege_type)

  union all
  select '9 function grant',
         p.proname || '(' || pg_get_function_identity_arguments(p.oid) || ') -> '
           || coalesce(a.grantee::regrole::text, '?'),
         a.privilege_type
    from pg_proc p
   cross join lateral aclexplode(coalesce(p.proacl, acldefault('f', p.proowner))) a
   where p.pronamespace = 'public'::regnamespace
     and a.grantee::regrole::text in ('anon','authenticated','public','-')

  union all
  select 'A index', i.indexname, i.tablename
    from pg_indexes i where i.schemaname='public'

  union all
  select 'B constraint', co.conname,
         co.conrelid::regclass::text || ': ' || pg_get_constraintdef(co.oid)
    from pg_constraint co
   where co.connamespace = 'public'::regnamespace

  union all
  -- MARKED, because these legitimately move between two runs on a live
  -- database while devices are reporting. Everything above should not.
  select 'C rowcount (varies)', 'devices', count(*)::text from public.devices
  union all
  select 'C rowcount (varies)', 'device_status', count(*)::text from public.device_status
  union all
  select 'C rowcount (varies)', 'status_events', count(*)::text from public.status_events
  union all
  select 'C rowcount (varies)', 'service_windows', count(*)::text from public.service_windows
  union all
  select 'C rowcount (varies)', 'meal_food_mapping', count(*)::text from public.meal_food_mapping
)
select section, object, detail from inv
 order by section, object, detail;

-- ---------------------------------------------------------------------
--  Also useful: the one-line totals. Run this on its own if you just want
--  the shape of the thing rather than every row.
--
--    select
--      (select count(*) from pg_class c join pg_namespace n on n.oid=c.relnamespace
--        where n.nspname='public' and c.relkind='r')                as tables,
--      (select count(*) from pg_views where schemaname='public')    as views,
--      (select count(*) from pg_proc
--        where pronamespace='public'::regnamespace)                 as functions,
--      (select count(*) from pg_policy pol join pg_class cl on cl.oid=pol.polrelid
--        join pg_namespace n on n.oid=cl.relnamespace where n.nspname='public')
--                                                                   as policies,
--      (select count(*) from pg_trigger tg join pg_class cl on cl.oid=tg.tgrelid
--        join pg_namespace n on n.oid=cl.relnamespace
--        where n.nspname='public' and not tg.tgisinternal)          as triggers;
-- ---------------------------------------------------------------------
