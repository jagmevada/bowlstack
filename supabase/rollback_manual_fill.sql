-- =====================================================================
--  Undo supabase/migrate_manual_fill.sql -- the manual-fill trial harness.
--
--  This file is the point of the trial being a trial. Everything the
--  migration added was either a NULLABLE column or a NEW object, so all of
--  it can be taken away and no row that existed beforehand changes.
--
--  IT DELETES THE TRIAL'S RESULTS. trial_vessel_capacity and every
--  manual_fill_pct ever recorded go with it. If the comparison has not been
--  written up yet, export it first:
--
--      \copy (select * from public.trial_fill_vs_weight) to 'trial.csv' csv header
--
--  Run in this order -- the view depends on the columns and the table.
-- =====================================================================

begin;

drop view if exists public.trial_fill_vs_weight;
drop view if exists public.trial_manual_fill;

drop table if exists public.trial_vessel_capacity;

-- The grants go with the columns; naming them anyway so a partially applied
-- migration rolls back as cleanly as a complete one.
revoke update (manual_fill_pct, manual_fill_age_s)
  on public.device_status from anon;
revoke insert (manual_fill_pct)
  on public.weight_samples from anon;

alter table public.device_status
  drop constraint if exists device_status_manual_fill_ck;
alter table public.weight_samples
  drop constraint if exists weight_samples_manual_fill_ck;

alter table public.device_status
  drop column if exists manual_fill_pct,
  drop column if exists manual_fill_age_s;

alter table public.weight_samples
  drop column if exists manual_fill_pct;

commit;

-- --- verification ------------------------------------------------------
-- Every count must be 0: nothing of the harness left anywhere.
select 'device_status columns' as what,
       count(*) as remaining
  from information_schema.columns
 where table_schema = 'public' and table_name = 'device_status'
   and column_name in ('manual_fill_pct', 'manual_fill_age_s')
union all
select 'weight_samples columns',
       count(*)
  from information_schema.columns
 where table_schema = 'public' and table_name = 'weight_samples'
   and column_name = 'manual_fill_pct'
union all
select 'trial objects',
       (to_regclass('public.trial_vessel_capacity') is not null)::int
     + (to_regclass('public.trial_fill_vs_weight') is not null)::int
     + (to_regclass('public.trial_manual_fill') is not null)::int;
