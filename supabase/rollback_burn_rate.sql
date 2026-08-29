-- =====================================================================
--  Undo migrate_burn_rate.sql.
--
--  Run as owner in the Supabase SQL editor. Destroys NOTHING: this migration
--  only ever created views and one function over weight_samples, so removing
--  them loses no data at all. The history stays and the views can be rebuilt
--  by re-running migrate_burn_rate.sql.
--
--  The dashboard keeps working without them. app.js tolerates both views
--  being absent -- it fetches them with a .catch that falls back to an empty
--  array -- so Master simply stops drawing the sparkline and the run-out line
--  and shows stock figures alone. That tolerance is what makes this safe to
--  run mid-service.
-- =====================================================================

begin;

-- Dependency order: slot_burn_rate reads slot_stock_series, and ALL THREE
-- views read burn_rate_tuning(). device_burn_rate is easy to forget because
-- it is the per-station diagnostic rather than the headline -- and leaving it
-- out makes the function undroppable, which is how this was caught.
drop view     if exists public.slot_burn_rate;
drop view     if exists public.slot_stock_series;
drop view     if exists public.device_burn_rate;
drop function if exists public.burn_rate_tuning();

commit;

select count(*)::text || ' burn-rate objects remaining (expect 0)' as result
  from (
    select 1 from pg_views
      where schemaname='public' and viewname in ('slot_burn_rate','slot_stock_series',
                                                 'device_burn_rate')
    union all
    select 1 from pg_proc
      where pronamespace='public'::regnamespace and proname='burn_rate_tuning'
  ) x;
