-- =====================================================================
--  Reset the UNDEPLOYED units back to "never heard from"
--
--  A REPAIR TOOL, not part of the normal setup sequence. On a freshly rebuilt
--  database no device has reported yet, so `awaiting_deployment` is already
--  correct and this does nothing. Run it only when something has written to units
--  that were never installed.
--
--  Run as owner, after assign_devices.sql. Idempotent. Safe to re-run.
--
--  WHY THIS IS NEEDED
--  ------------------
--  `awaiting_deployment` in device_overview means "registered, but has never
--  reported". It is the state the front-end must tell apart from `offline`, and
--  getting that wrong is the difference between a dashboard that flags a real
--  failure and one that flags the 8 reserved units nobody has installed yet.
--
--  The flag is sticky by design. tg_device_status_stamp() sets
--  `reported := true` unconditionally on every UPDATE, precisely so a device
--  cannot pin it and hide an outage. That means ANY write to a device --
--  including one stray test round -- permanently retires it from
--  `awaiting_deployment`.
--
--  The realistic cause is a simulator or test script pointed at every registered
--  id rather than only the deployed ones. Point them at BWL-001..024 and this
--  file is never needed.
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
--  Only devices with `location = 'R'` (reserved/future) or no location at all --
--  BWL-025..032 after assign_devices.sql. A deployed unit in D, M or T is never
--  affected, so live telemetry from the prototype or from the simulated fleet
--  survives. It DOES delete history for the spares, which is the intent: a unit
--  that has never been installed should have nothing to show.
-- =====================================================================

begin;

-- Report first, so the operation is visible before it happens.
select count(*) as spares_to_reset
  from public.devices where location = 'R' or location is null;

delete from public.status_events
 where device_id in (select device_id from public.devices
                     where location = 'R' or location is null);

delete from public.device_status
 where device_id in (select device_id from public.devices
                     where location = 'R' or location is null);

-- Re-create with defaults: reported = false, updated_at = null, every telemetry
-- column NULL. Matches exactly what tg_devices_create_status() produces when a
-- unit is first registered.
insert into public.device_status (device_id)
select device_id from public.devices
 where location = 'R' or location is null
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
