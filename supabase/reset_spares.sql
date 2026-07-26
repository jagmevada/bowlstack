-- =====================================================================
--  Reset the UNDEPLOYED units back to "never heard from"
--
--  Run as owner, after deploy_devices.sql. Idempotent. Safe to re-run.
--
--  WHY THIS IS NEEDED
--  ------------------
--  `awaiting_deployment` in device_overview means "registered, but has never
--  reported". It is the state the front-end must tell apart from `offline`, and
--  getting that wrong is the difference between a dashboard that flags a real
--  failure and one that flags 17 units nobody has installed yet.
--
--  The flag is sticky by design. tg_device_status_stamp() sets
--  `reported := true` unconditionally on every UPDATE, precisely so a device
--  cannot pin it and hide an outage. That means ANY write to a device -- one
--  stray test round included -- permanently retires it from
--  `awaiting_deployment`.
--
--  This happened during bring-up: a fleet-simulator run wrote to all 32
--  registered ids rather than only the deployed ones, so every unit reported and
--  the state became untestable. This script restores it.
--
--  HOW
--  ---
--  The stamp trigger is BEFORE UPDATE only, so an UPDATE cannot clear the flag --
--  the trigger would set it straight back, and would stamp updated_at to now()
--  as well. Deleting the row and letting it be re-created with column defaults
--  is the only path that actually resets it.
--
--  WHAT IT TOUCHES
--  ---------------
--  Only devices with `area IS NULL`, i.e. the spares. A deployed unit is never
--  affected, so live telemetry from the prototype or from the simulated fleet
--  survives. It DOES delete history for the spares -- which is the intent: a unit
--  that has never been installed should have nothing to show.
-- =====================================================================

begin;

-- Report first, so the operation is visible before it happens.
select count(*) as spares_to_reset
  from public.devices where area is null;

delete from public.status_events
 where device_id in (select device_id from public.devices where area is null);

delete from public.device_status
 where device_id in (select device_id from public.devices where area is null);

-- Re-create with defaults: reported = false, updated_at = null, every telemetry
-- column NULL. Matches exactly what tg_devices_create_status() produces when a
-- unit is first registered.
insert into public.device_status (device_id)
select device_id from public.devices where area is null
 on conflict (device_id) do nothing;

commit;

-- ---------------------------------------------------------------------
--  Result: the three states the front-end must distinguish.
-- ---------------------------------------------------------------------
select case
         when awaiting_deployment then 'awaiting_deployment'
         when offline             then 'offline'
         when data_is_stale       then 'data_is_stale'
         else                          'live'
       end                        as ui_state,
       count(*)                   as devices,
       string_agg(device_id, ', ' order by device_id) as which
  from public.device_overview
 group by 1
 order by 1;
