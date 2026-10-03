-- =====================================================================
--  weight_samples: the time index becomes BRIN -- the dashboard's 5 s refresh
--
--  slot_stock_series asks, per platform per 5-minute grid point, for "the
--  newest sample of device X at or before T" (ORDER BY recorded_at DESC
--  LIMIT 1). With a btree on recorded_at alone the planner walked THAT index
--  backwards and filtered on device_id -- cheap when X posts often, but for a
--  platform with NO samples (every counter scale that has never reported) it
--  walked the entire table before giving up. 13 grid points x every silent
--  platform, on every dashboard refresh, growing with the history: ~5 s on
--  live for slot_stock_series and again for slot_burn_rate, which reads it.
--
--  A BRIN on recorded_at serves the range scans (device_burn_rate's last
--  hour, retention) but cannot hand rows back in order, so the only ordered
--  path left for that lookup is weight_samples_device_time_idx -- one probe
--  per point. Locally: slot_stock_series 494 ms -> 6 ms, slot_burn_rate
--  1135 ms -> 22 ms, results identical. Ordering the lookup by
--  (device_id, recorded_at) does not help: device_id is a constant there and
--  the planner drops it from the sort.
--
--  BRIN fits because the table is append-only and recorded_at follows insert
--  order (offline replays are the exception, and only cost a lossy recheck).
--
--  Idempotent. Undo: rollback_weight_samples_brin.sql. migrate_weight_samples.sql
--  creates the BRIN directly on a fresh database.
-- =====================================================================
begin;

drop index if exists public.weight_samples_recorded_at_idx;
create index if not exists weight_samples_recorded_at_brin
  on public.weight_samples using brin (recorded_at);

select indexname, indexdef
  from pg_indexes
 where schemaname = 'public' and tablename = 'weight_samples'
 order by 1;

commit;
