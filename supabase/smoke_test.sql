-- =====================================================================
--  Run this AFTER schema.sql and BEFORE flashing any device.
--
--  It proves the one thing most likely to be wrong: that a device can insert
--  rows carrying a foreign key into `devices` while having no permission to
--  read `devices` at all. If that assumption were false, every device write
--  would fail in the field with a confusing permission error.
--
--  Everything runs inside a transaction that is rolled back.
-- =====================================================================

-- Register a test installation first (as owner).
insert into public.devices (device_id, label, location)
values ('BWL-007', 'bench rig', 'workshop')
on conflict (device_id) do nothing;

begin;

-- Become the device.
set local role anon;
select set_config('request.jwt.claims', '{"role":"anon"}', true);

-- --- MUST FAIL: the registry is invisible to devices -----------------------
-- Expect: ERROR permission denied for table devices
select count(*) from public.devices;

-- --- MUST FAIL: devices cannot read back their own state -------------------
-- Expect: ERROR permission denied for table device_status
select count(*) from public.device_status;

-- --- MUST SUCCEED: the upsert, including an FK probe into an unreadable
--     table. This is the assertion the whole design rests on.
-- Expect: INSERT 0 1
insert into public.device_status
  (device_id, boot_id, uptime_s, stack_count, stack_status, levels, sensors_ok,
   sensors_online, battery_mv, battery_pct, charging, firmware, mac)
values
  ('BWL-007', 12345, 100, 3, 'ok',
   '{present,present,present,absent}', '{t,t,t,t}', 4,
   3980, 76, false, '0.3.0', '24:6F:28:AA:BB:CC')
on conflict (device_id) do update set uptime_s = excluded.uptime_s;

-- --- MUST SUCCEED: an event, and recorded_at must land 5 minutes in the past
--     because age_ms says the event happened 300000 ms ago.
insert into public.status_events
  (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
   levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
values
  ('BWL-007', 12345, 1, 300000, 'change', 2, 'ok',
   '{present,present,absent,absent}', '{t,t,t,t}', 4, 76, false, '0.3.0');

-- --- MUST FAIL 23503: unregistered device_id.
--     This is the provisioning gate; it also proves a unit flashed without
--     -DBOWLSTACK_DEVICE_ID (which defaults to BWL-000) cannot write.
insert into public.status_events
  (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
   levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
values
  ('BWL-000', 1, 1, 0, 'boot', 0, 'degraded',
   '{unknown,unknown,unknown,unknown}', '{f,f,f,f}', 0, null, false, '0.3.0');

-- --- MUST FAIL: bad vocabulary is rejected at the edge.
insert into public.status_events
  (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
   levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
values
  ('BWL-007', 12345, 2, 0, 'change', 1, 'OK',   -- uppercase: not in the CHECK
   '{present,absent,absent,absent}', '{t,t,t,t}', 4, 76, false, '0.3.0');

rollback;

-- ---------------------------------------------------------------------
-- Owner-side checks. Run these after a real device has reported.
-- ---------------------------------------------------------------------
reset role;

-- recorded_at should sit age_ms in the past, NOT at insertion time.
-- select device_id, reason, age_ms, recorded_at, received_at,
--        received_at - recorded_at as reconstructed_age
--   from public.status_events order by id desc limit 10;

-- Fleet overview.
-- select * from public.device_overview order by device_id;

-- Idempotency: re-running an identical batch must be a no-op, not a duplicate.
-- select device_id, boot_id, seq, count(*)
--   from public.status_events group by 1,2,3 having count(*) > 1;
