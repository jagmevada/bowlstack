-- Undo migrate_weight_samples_brin.sql: back to the btree on recorded_at.
-- The dashboard's slot_stock_series / slot_burn_rate go back to seconds.
begin;

drop index if exists public.weight_samples_recorded_at_brin;
create index if not exists weight_samples_recorded_at_idx
  on public.weight_samples (recorded_at);

commit;
