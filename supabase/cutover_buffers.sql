-- =====================================================================
--  Cut-over -- every BWL-xxx becomes a buffer platform.
--
--  Run as owner in the Supabase SQL editor, ONCE, after migrate_buffer.sql
--  AND after the web that reads buffers has been pushed. Step 3 of the
--  rollout in migrate_buffer.sql's header; the kg fleet simulator (step 4)
--  starts only after this has committed.
--
--  Undo: supabase/rollback_cutover_buffers.sql.
--
--  WHAT IT DOES, in one transaction:
--
--    1. NULLs stack_count, stack_status, levels, sensors_ok and
--       sensors_online on every BWL-% status row that still holds any.
--    2. Sets devices.kind = 'buffer' on every BWL-% device.
--
--  WHY THIS IS NOT PART OF apply_loadcell.sql. That file proves it changed
--  nothing about the bowl counters -- it snapshots every status row and
--  every kind and aborts if one moved. This file exists to move exactly
--  those, so it would trip both checks by design. It is also the one step
--  of the rollout that changes what the dashboard shows, and that deserves
--  to be a decision somebody takes on its own rather than a side effect of
--  re-running a migration.
--
--  WHY THE stack_* ARE CLEARED, when the views already ignore them on a
--  buffer: device_overview passes them straight through, so a buffer would
--  otherwise show its last bowl count from the ToF days beside its weight,
--  forever, and nothing would say which of the two was current.
--
--  WHAT IT DOES NOT TOUCH: status_events. That history is real -- it is
--  what the stacks counted -- and slot_stock_series reads it only for
--  devices still kind = 'stack', so it simply stops being read. Nor
--  updated_at / reported: see section 2.
--
--  BEFORE RUNNING: stop every writer of BWL stack data -- the stack fleet
--  simulator, any esp32dev / esp32dev-fleet board. From the moment this
--  commits, device_status_kind refuses a stack_* write to a buffer and
--  status_events_kind refuses its history, with 23514.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 0. Prerequisite: migrate_buffer.sql. Without it the kind CHECK refuses
--    'buffer', and a buffer would have no columns to report into and no
--    guard keeping stack writers out.
-- ---------------------------------------------------------------------
do $$
begin
  if to_regprocedure('public.tg_device_status_kind()') is null
     or not exists (select 1 from pg_constraint
                     where conname = 'devices_kind_ck'
                       and pg_get_constraintdef(oid) like '%buffer%') then
    raise exception
      'Run supabase/migrate_buffer.sql FIRST -- devices.kind does not accept '
      '''buffer'' yet. Nothing has been changed.';
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. What is about to change, recorded so the report can say it.
--
-- Session-lived rather than ON COMMIT DROP: the report below runs after the
-- commit, because the Supabase editor shows only the last statement.
-- ---------------------------------------------------------------------
drop table if exists _cutover;
create temp table _cutover as
  select d.device_id,
         d.kind                                                  as old_kind,
         (s.stack_count is not null or s.stack_status is not null
          or s.levels is not null or s.sensors_ok is not null
          or s.sensors_online is not null)                       as had_stack
    from public.devices d
    left join public.device_status s using (device_id)
   where d.device_id like 'BWL-%';

-- ---------------------------------------------------------------------
-- 2. Clear the stack half -- WITHOUT stamping the rows.
--
-- device_status_stamp sets updated_at := now() and reported := true on
-- every UPDATE, which is right for a device and wrong for this: it would
-- make every BWL look as if it had just reported. A dead BWL-001 would
-- read live for forty seconds and its missed_last_service would clear, and
-- a spare that had ever been written would never be "awaiting deployment"
-- again. Neither is a measurement, so the stamp is switched off for this
-- one statement, inside this transaction, and back on before the commit --
-- if anything fails, the rollback restores it with everything else.
--
-- ALTER TABLE ... DISABLE TRIGGER needs table ownership, which the owner
-- running this in the SQL editor has. It locks device_status for the
-- length of this transaction -- milliseconds -- so device PATCHes arriving
-- meanwhile wait rather than fail.
--
-- Only rows that HOLD something are touched. A row whose stack columns are
-- already NULL has nothing to clear, and leaving it alone is what keeps a
-- re-run a no-op.
-- ---------------------------------------------------------------------
alter table public.device_status disable trigger device_status_stamp;

update public.device_status
   set stack_count    = null,
       stack_status   = null,
       levels         = null,
       sensors_ok     = null,
       sensors_online = null
 where device_id like 'BWL-%'
   and (stack_count is not null or stack_status is not null
        or levels is not null or sensors_ok is not null
        or sensors_online is not null);

alter table public.device_status enable trigger device_status_stamp;

-- ---------------------------------------------------------------------
-- 3. Re-kind. AFTER the clear, though the guard does not need that order:
--    it tests only columns that change to a non-null value, and clearing
--    sets NULL. The order is for a reader -- the row is consistent at every
--    step, a stack with no bowl count and then a buffer with no bowl count.
-- ---------------------------------------------------------------------
update public.devices
   set kind = 'buffer'
 where device_id like 'BWL-%'
   and kind is distinct from 'buffer';

commit;

-- ---------------------------------------------------------------------
--  Report. One result set.
-- ---------------------------------------------------------------------
select 'BWL devices re-kinded to buffer' as what,
       (select count(*) from _cutover where old_kind <> 'buffer')::text as detail
union all
select 'BWL status rows whose stack_* were cleared',
       (select count(*) from _cutover where had_stack)::text
union all
select 'BWL devices now buffer / total BWL',
       (select count(*) filter (where kind = 'buffer') || ' / ' || count(*)
          from public.devices where device_id like 'BWL-%')
union all
select 'BWL status rows still holding stack_*',
       (select count(*) from public.device_status
         where device_id like 'BWL-%'
           and (stack_count is not null or stack_status is not null
                or levels is not null or sensors_ok is not null
                or sensors_online is not null))::text
union all
select 'remaining kind = stack devices',
       (select count(*) from public.devices where kind = 'stack')::text;
-- Expect on the first run: 32, however many had reported, 32 / 32, 0, 0.
-- On a re-run: 0, 0, 32 / 32, 0, 0.
