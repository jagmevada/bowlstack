-- =====================================================================
--  Undo cutover_buffers.sql -- every BWL buffer goes back to a stack.
--
--  Run as owner in the Supabase SQL editor. Run it BEFORE
--  rollback_buffer.sql, which refuses while any device is a buffer.
--
--  WHAT IT DOES, in one transaction:
--
--    1. NULLs the load-cell half -- weight_g, weight_state, cells_online,
--       counts_per_gram, net_counts, bowls, bowls_confirmed, gross_g -- on
--       every BWL-% status row that is a buffer and holds any of it.
--    2. Sets devices.kind = 'stack' on every BWL-% buffer.
--
--  WHAT IT DOES NOT DO:
--
--    * Restore the bowl counts the cut-over cleared. They were the last
--      reading of hardware that has since been replaced; a stack row reads
--      "--" until a stack reports again, which is the truth.
--    * Delete the buffers' weight_samples. That history is real. The views
--      read weight_samples only for kind 'scale' and 'buffer', so once these
--      are stacks it simply stops being read. BUT rollback_buffer.sql
--      refuses while any sample is above 100 kg -- a full buffer is -- so
--      if you are going on to that, decide about those rows first.
--
--  BEFORE RUNNING: stop every writer of buffer weights -- the kg fleet
--  simulator and the LDC panel's buffer uplink. From the moment this
--  commits the guards refuse a weight on a stack, with 23514.
-- =====================================================================

begin;

do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'device_status'
                    and column_name = 'bowls') then
    raise exception
      'device_status has no bowls column, so migrate_buffer.sql is not '
      'applied (or was already rolled back) and there is no cut-over to '
      'undo. Nothing has been changed.';
  end if;
end $$;

drop table if exists _uncut;
create temp table _uncut as
  select d.device_id,
         (s.weight_g is not null or s.weight_state is not null
          or s.cells_online is not null or s.counts_per_gram is not null
          or s.net_counts is not null or s.bowls is not null
          or s.bowls_confirmed is not null or s.gross_g is not null) as had_weight
    from public.devices d
    left join public.device_status s using (device_id)
   where d.device_id like 'BWL-%'
     and d.kind = 'buffer';

-- Without stamping, for the reason cutover_buffers.sql section 2 gives.
alter table public.device_status disable trigger device_status_stamp;

update public.device_status
   set weight_g        = null,
       weight_state    = null,
       cells_online    = null,
       counts_per_gram = null,
       net_counts      = null,
       bowls           = null,
       bowls_confirmed = null,
       gross_g         = null
 where device_id in (select device_id from _uncut where had_weight);

alter table public.device_status enable trigger device_status_stamp;

update public.devices
   set kind = 'stack'
 where device_id in (select device_id from _uncut);

commit;

select 'BWL buffers re-kinded to stack' as what,
       (select count(*) from _uncut)::text as detail
union all
select 'BWL status rows whose weight columns were cleared',
       (select count(*) from _uncut where had_weight)::text
union all
select 'remaining kind = buffer devices',
       (select count(*) from public.devices where kind = 'buffer')::text
union all
select 'weight_samples rows above 100 kg (rollback_buffer.sql refuses on these)',
       (select count(*) from public.weight_samples where weight_g > 100000)::text;
-- Expect: 32, however many had reported, 0, and 0 before rollback_buffer.sql.
