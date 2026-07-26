-- =====================================================================
--  Bowlstack schema smoke test
--
--  Run AFTER schema.sql and BEFORE flashing any device. Paste the whole file
--  into the Supabase SQL editor and run once; it prints a PASS/FAIL line per
--  assertion and rolls everything back, leaving no test data behind.
--
--  Several assertions are SUPPOSED to fail (a device must NOT be able to read
--  your data). They are wrapped in exception handlers so the run continues --
--  a bare failing statement would abort the transaction and stop the script at
--  the first one.
--
--  The assertion that matters most is #3: a device can INSERT a row carrying a
--  foreign key into `devices` while having no permission to READ `devices`.
--  The whole privilege design rests on that, and it is the thing most likely
--  to be wrong.
-- =====================================================================

begin;

-- Register a test installation (as owner). Rolled back with everything else.
insert into public.devices (device_id, label, location)
values ('BWL-007', 'bench rig', 'workshop')
on conflict (device_id) do nothing;

do $$
declare
  out_text text := E'\n===== Bowlstack schema smoke test =====';
  n_pass   int  := 0;
  n_fail   int  := 0;
  v_age    interval;
  v_gap    interval;

  procedure_note text;
begin
  ------------------------------------------------------------------
  -- Become the device.
  ------------------------------------------------------------------
  execute 'set local role anon';
  perform set_config('request.jwt.claims', '{"role":"anon"}', true);

  ------------------------------------------------------------------
  -- 1. The registry must be invisible to devices.
  ------------------------------------------------------------------
  begin
    perform 1 from public.devices limit 1;
    out_text := out_text || E'\nFAIL  1  devices IS readable by anon (expected denial)';
    n_fail := n_fail + 1;
  exception when insufficient_privilege then
    out_text := out_text || E'\nPASS  1  devices not readable by anon';
    n_pass := n_pass + 1;
  end;

  ------------------------------------------------------------------
  -- 2. A device must not be able to read back any state.
  ------------------------------------------------------------------
  begin
    perform 1 from public.device_status limit 1;
    out_text := out_text || E'\nFAIL  2  device_status IS readable by anon (expected denial)';
    n_fail := n_fail + 1;
  exception when insufficient_privilege then
    out_text := out_text || E'\nPASS  2  device_status not readable by anon';
    n_pass := n_pass + 1;
  end;

  ------------------------------------------------------------------
  -- 3. THE KEY ASSERTION. The upsert must succeed, including its foreign-key
  --    probe into a table anon cannot read. Referential integrity checks
  --    bypass row security and run as the table owner, so this works -- but if
  --    that were wrong, every device write would fail in the field.
  ------------------------------------------------------------------
  begin
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values
      ('BWL-007', 12345, 100, 3, 'ok',
       array['present','present','present','absent'],
       array[true,true,true,true], 4, 3980, 76, false,
       '0.2.0', '8C:94:DF:4C:7A:04')
    on conflict (device_id) do update set uptime_s = excluded.uptime_s;

    out_text := out_text || E'\nPASS  3  upsert succeeded through an unreadable FK';
    n_pass := n_pass + 1;
  exception when others then
    out_text := out_text || E'\nFAIL  3  upsert rejected: ' || sqlstate || ' ' || sqlerrm;
    n_fail := n_fail + 1;
  end;

  ------------------------------------------------------------------
  -- 4. Event insert. age_ms = 300000 means "this happened 5 minutes ago";
  --    the trigger must backdate recorded_at accordingly.
  ------------------------------------------------------------------
  begin
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      ('BWL-007', 12345, 1, 300000, 'change', 2, 'ok',
       array['present','present','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0');

    out_text := out_text || E'\nPASS  4  event accepted';
    n_pass := n_pass + 1;
  exception when others then
    out_text := out_text || E'\nFAIL  4  event rejected: ' || sqlstate || ' ' || sqlerrm;
    n_fail := n_fail + 1;
  end;

  ------------------------------------------------------------------
  -- 5. Idempotency. Replaying the same (device_id, boot_id, seq) must be a
  --    no-op, not a duplicate -- this is what makes a retry after a lost
  --    response safe.
  ------------------------------------------------------------------
  begin
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      ('BWL-007', 12345, 1, 300000, 'change', 2, 'ok',
       array['present','present','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0')
    on conflict (device_id, boot_id, seq) do nothing;

    out_text := out_text || E'\nPASS  5  duplicate event ignored (idempotent retry)';
    n_pass := n_pass + 1;
  exception when others then
    out_text := out_text || E'\nFAIL  5  duplicate handling broken: ' || sqlstate || ' ' || sqlerrm;
    n_fail := n_fail + 1;
  end;

  ------------------------------------------------------------------
  -- 6. Provisioning gate. BWL-000 is the firmware default in version.h and is
  --    deliberately never registered, so a unit flashed without
  --    -DBOWLSTACK_DEVICE_ID fails loudly instead of writing to a real slot.
  ------------------------------------------------------------------
  begin
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      ('BWL-000', 1, 1, 0, 'boot', 0, 'degraded',
       array['unknown','unknown','unknown','unknown'],
       array[false,false,false,false], 0, null, false, '0.2.0');

    out_text := out_text || E'\nFAIL  6  unregistered BWL-000 was ACCEPTED';
    n_fail := n_fail + 1;
  exception when foreign_key_violation then
    out_text := out_text || E'\nPASS  6  unregistered device_id rejected (23503)';
    n_pass := n_pass + 1;
  end;

  ------------------------------------------------------------------
  -- 7. Wire vocabulary is pinned. "OK" (uppercase) is the plotter display
  --    string; only lowercase "ok" is valid on the wire. This is what stops a
  --    cosmetic edit to the log format silently corrupting telemetry.
  ------------------------------------------------------------------
  begin
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      ('BWL-007', 12345, 2, 0, 'change', 1, 'OK',
       array['present','absent','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0');

    out_text := out_text || E'\nFAIL  7  uppercase stack_status was ACCEPTED';
    n_fail := n_fail + 1;
  exception when check_violation then
    out_text := out_text || E'\nPASS  7  bad vocabulary rejected by CHECK';
    n_pass := n_pass + 1;
  end;

  ------------------------------------------------------------------
  -- 8. Devices must not be able to delete history.
  ------------------------------------------------------------------
  begin
    delete from public.status_events where device_id = 'BWL-007';
    out_text := out_text || E'\nFAIL  8  anon CAN delete events';
    n_fail := n_fail + 1;
  exception when insufficient_privilege then
    out_text := out_text || E'\nPASS  8  anon cannot delete events';
    n_pass := n_pass + 1;
  end;

  ------------------------------------------------------------------
  -- Back to owner to inspect what the device wrote.
  ------------------------------------------------------------------
  execute 'reset role';

  ------------------------------------------------------------------
  -- 9. The clock-free timestamp. recorded_at must sit ~300 s in the past even
  --    though the row was inserted just now. This is what lets a device with
  --    no RTC replay buffered events with correct times.
  ------------------------------------------------------------------
  begin
    select received_at - recorded_at into v_age
      from public.status_events
     where device_id = 'BWL-007' and boot_id = 12345 and seq = 1;

    v_gap := v_age - interval '300 seconds';

    if v_age is null then
      out_text := out_text || E'\nFAIL  9  event row not found';
      n_fail := n_fail + 1;
    elsif abs(extract(epoch from v_gap)) < 2 then
      out_text := out_text || E'\nPASS  9  recorded_at backdated by age_ms (' ||
                  round(extract(epoch from v_age))::text || 's, expected 300s)';
      n_pass := n_pass + 1;
    else
      out_text := out_text || E'\nFAIL  9  recorded_at off by ' ||
                  round(extract(epoch from v_gap))::text || 's';
      n_fail := n_fail + 1;
    end if;
  exception when others then
    out_text := out_text || E'\nFAIL  9  ' || sqlstate || ' ' || sqlerrm;
    n_fail := n_fail + 1;
  end;

  ------------------------------------------------------------------
  -- 10. updated_at must advance on the UPDATE half of an upsert. A column
  --     DEFAULT does not fire there, so without the trigger this column would
  --     freeze at first contact -- and it is what the dashboard uses to decide
  --     a device has gone offline.
  ------------------------------------------------------------------
  begin
    update public.device_status
       set updated_at = now() - interval '1 hour'
     where device_id = 'BWL-007';

    execute 'set local role anon';
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values
      ('BWL-007', 12345, 200, 3, 'ok',
       array['present','present','present','absent'],
       array[true,true,true,true], 4, 3980, 76, false,
       '0.2.0', '8C:94:DF:4C:7A:04')
    on conflict (device_id) do update set uptime_s = excluded.uptime_s;
    execute 'reset role';

    select now() - updated_at into v_age
      from public.device_status where device_id = 'BWL-007';

    if extract(epoch from v_age) < 5 then
      out_text := out_text || E'\nPASS 10  updated_at refreshed on upsert';
      n_pass := n_pass + 1;
    else
      out_text := out_text || E'\nFAIL 10  updated_at stale by ' ||
                  round(extract(epoch from v_age))::text || 's (trigger not firing)';
      n_fail := n_fail + 1;
    end if;
  exception when others then
    execute 'reset role';
    out_text := out_text || E'\nFAIL 10  ' || sqlstate || ' ' || sqlerrm;
    n_fail := n_fail + 1;
  end;

  ------------------------------------------------------------------
  out_text := out_text || E'\n---------------------------------------';
  out_text := out_text || E'\n' || n_pass::text || ' passed, ' ||
              n_fail::text || ' failed';
  if n_fail = 0 then
    out_text := out_text || E'\nSchema is ready. Safe to flash devices.';
  else
    out_text := out_text || E'\nDo NOT flash until the failures above are fixed.';
  end if;
  out_text := out_text || E'\n=======================================';

  raise notice '%', out_text;
end $$;

rollback;

-- ---------------------------------------------------------------------
--  Results appear in the editor's messages/notices panel, not as a table.
--  Everything above is rolled back, so no test rows survive.
--
--  After a real device has reported, these are useful as owner:
--
--    select * from public.device_overview order by device_id;
--
--    select device_id, reason, age_ms, recorded_at, received_at,
--           received_at - recorded_at as reconstructed_age
--      from public.status_events order by id desc limit 20;
--
--    -- must return no rows: idempotency is holding
--    select device_id, boot_id, seq, count(*)
--      from public.status_events group by 1,2,3 having count(*) > 1;
-- ---------------------------------------------------------------------
