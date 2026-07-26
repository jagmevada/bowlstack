-- =====================================================================
--  Enable RLS (idempotent, safe to re-run)
--
--  schema.sql already does this, but run it if the dashboard shows RLS off
--  for any of the three tables -- smoke_test.sql assertion 0 checks it.
--
--  Worth understanding what is protecting you right now: the GRANTs are.
--  schema.sql revokes everything from anon and grants only INSERT/UPDATE on
--  device_status and INSERT on status_events, so anon cannot read your data
--  even with RLS off. RLS is the second layer, and it is the one that
--  restricts WHICH rows -- which matters as soon as policies stop being
--  `using (true)`, e.g. when per-device JWTs land.
--
--  Supabase's linter also flags any table in `public` exposed through
--  PostgREST without RLS, so leaving it off will keep raising advisories.
-- =====================================================================

alter table public.devices       enable row level security;
alter table public.device_status enable row level security;
alter table public.status_events enable row level security;

-- Added by migration_002; ignore the error if that has not been run yet.
alter table public.service_windows enable row level security;

-- Verify.
select c.relname            as table_name,
       c.relrowsecurity     as rls_enabled,
       c.relforcerowsecurity as rls_forced
  from pg_class c
  join pg_namespace n on n.oid = c.relnamespace
 where n.nspname = 'public'
   and c.relname in ('devices','device_status','status_events','service_windows')
 order by c.relname;

-- Policies actually present.
select tablename, policyname, cmd, roles
  from pg_policies
 where schemaname = 'public'
 order by tablename, policyname;
