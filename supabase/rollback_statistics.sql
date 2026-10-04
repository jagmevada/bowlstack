-- Undo migrate_statistics.sql: the Statistics page's two read-only functions.
-- Nothing else depends on them; the page shows an error until they return.
begin;

drop function if exists public.slot_meal_stats(date, date, text, integer);
drop function if exists public.slot_meal_series(date, text, integer);

commit;
