-- =====================================================================
--  Undo supabase/migrate_vbus_sense.sql.
--
--  One nullable column and one view, so this restores the schema exactly.
--  A device still running firmware that SENDS external_power will then get a
--  400 on every PATCH and go silent -- reflash before running this, or run it
--  only when the fleet is already on an image that does not send it.
-- =====================================================================

begin;

drop view if exists public.device_power;
revoke update (external_power) on public.device_status from anon;
alter table public.device_status drop column if exists external_power;

commit;

select 'external_power column' as what,
       count(*) as remaining
  from information_schema.columns
 where table_schema = 'public' and table_name = 'device_status'
   and column_name = 'external_power'
union all
select 'device_power view', (to_regclass('public.device_power') is not null)::int;
