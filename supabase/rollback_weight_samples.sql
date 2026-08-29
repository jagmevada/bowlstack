-- =====================================================================
--  Undo migrate_weight_samples.sql.
--
--  Run as owner in the Supabase SQL editor.
--
--  WHAT THIS DESTROYS
--  ------------------
--  Every weight sample any load cell has recorded -- the entire consumption
--  history, and with it any burn rate or run-out projection computed from it.
--  Nothing else: device_status keeps its CURRENT weight, the registry, the
--  bowl counters and their telemetry are untouched.
--
--  There is no export step here on purpose. If the history is worth keeping,
--  copy it out BEFORE running this:
--
--      \copy (select * from public.weight_samples) to 'weights.csv' csv header
--
--  ORDER. slot_burn_rate and slot_stock_series READ this table, so they go
--  first -- Postgres tracks the dependency and would otherwise refuse, or with
--  CASCADE take them silently. Re-run migrate_burn_rate.sql after re-applying
--  the table if you want them back.
-- =====================================================================

begin;

drop view if exists public.slot_burn_rate;
drop view if exists public.slot_stock_series;
-- The per-station diagnostic reads weight_samples directly AND holds a
-- reference to burn_rate_tuning(); omitting it blocks both drops below.
drop view if exists public.device_burn_rate;
drop function if exists public.burn_rate_tuning();

drop trigger if exists weight_samples_kind  on public.weight_samples;
drop trigger if exists weight_samples_stamp on public.weight_samples;
drop function if exists public.tg_weight_samples_kind();
drop function if exists public.tg_weight_samples_stamp();

-- The grants and the policies go with the table; naming them separately would
-- be noise, unlike the column constraints in rollback_loadcell.sql, which have
-- to survive a half-applied migration.
drop table if exists public.weight_samples;

commit;

select 'weight_samples' as object,
       (select count(*) from pg_tables
         where schemaname='public' and tablename='weight_samples')::text as remaining
union all
select 'burn-rate views',
       (select count(*) from pg_views
         where schemaname='public'
           and viewname in ('slot_burn_rate','slot_stock_series',
                            'device_burn_rate'))::text;
-- Expect: 0 and 0.
