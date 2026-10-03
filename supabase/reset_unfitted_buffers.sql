-- =====================================================================
--  BWL-002 / BWL-003 -> "awaiting deployment"
--
--  They are the panel's B2/B3 slots and NOT FITTED, but their device_status
--  rows still held the old ToF boards' last report (fw 0.2.0, August 2026), so
--  the dashboard counted them offline forever. A fresh row is the truthful state
--  of a registered platform that has never reported under the load-cell
--  architecture. Nothing a device could POST says that: any PATCH marks the row
--  reported, and an unfitted slot has no reading to send.
--
--  Same move as reset_spares.sql (delete the status row, re-create it empty),
--  narrowed to these two ids. HISTORY IS KEPT: status_events and weight_samples
--  are not touched. A row written within the last day is left alone, so a B2/B3
--  that has since been fitted and is reporting is never reset.
--
--  Owner-run, idempotent. No rollback file: what it removes is a stale ToF
--  status, printed by the first SELECT below before it goes.
-- =====================================================================
begin;

select device_id, firmware, mac, updated_at
  from public.device_status
 where device_id in ('BWL-002', 'BWL-003');

delete from public.device_status
 where device_id in ('BWL-002', 'BWL-003')
   and (updated_at is null or updated_at < now() - interval '1 day');

insert into public.device_status (device_id)
select device_id from public.devices where device_id in ('BWL-002', 'BWL-003')
    on conflict (device_id) do nothing;

select device_id, awaiting_deployment, reported, offline
  from public.device_overview
 where device_id in ('BWL-002', 'BWL-003')
 order by 1;

commit;
