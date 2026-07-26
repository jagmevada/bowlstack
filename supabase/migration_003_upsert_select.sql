-- =====================================================================
--  Migration 003 -- SELECT privilege required by ON CONFLICT
--
--  Run as owner. Returns one result set explaining what it did.
--
--  WHY THIS EXISTS
--  ---------------
--  smoke_test.sql assertions 3, 5 and 10 failed with
--      42501 permission denied for table device_status / status_events
--  while assertion 4 -- a PLAIN insert into the same table by the same role --
--  passed. The only difference is ON CONFLICT.
--
--  PostgreSQL must probe the arbiter index to find a conflicting row, and that
--  probe is a read: it requires SELECT privilege on the inferred index's
--  columns. anon held INSERT and UPDATE but no SELECT at all, so every upsert
--  the firmware makes would have been rejected. The heartbeat and the event
--  batch both use ON CONFLICT, so nothing would have reached the server.
--
--  This grants SELECT on the KEY COLUMNS ONLY, not the whole table, so the
--  "device cannot read telemetry" property survives: a device can still not
--  see any other installation's bowl counts, battery, or history.
--
--  The script tests the minimal grant first and escalates only if the server
--  rejects it, so you get the least privilege that actually works on your
--  Postgres version rather than the one I assumed.
-- =====================================================================

drop table if exists fix_results;
create temp table fix_results (n int, step text, result text, detail text);

do $$
declare
  res  jsonb := '[]'::jsonb;
  e    jsonb;
  DEV  constant text := 'BWL-UPSERTFIX';
  ok   boolean;
begin
  execute 'reset role';

  -- Clean slate.
  delete from public.status_events where device_id = DEV;
  delete from public.device_status where device_id = DEV;
  delete from public.devices       where device_id = DEV;
  insert into public.devices (device_id, label) values (DEV, 'upsert fix probe');

  ------------------------------------------------------------------
  -- 1. Confirm the diagnosis: ON CONFLICT fails, plain INSERT works.
  ------------------------------------------------------------------
  ok := true;
  begin
    execute 'set local role anon';
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values (DEV, 1, 1, 0, 'ok', array['absent','absent','absent','absent'],
            array[true,true,true,true], 4, 3900, 70, false, 'probe', 'x')
    on conflict (device_id) do update set uptime_s = excluded.uptime_s;
  exception when insufficient_privilege then
    ok := false;
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',1,'step','before fix: upsert as anon',
           'result', case when ok then 'ALREADY WORKS' else 'BLOCKED' end,
           'detail', case when ok
             then 'no fix needed - ON CONFLICT already permitted'
             else '42501, as expected: ON CONFLICT needs SELECT on the arbiter columns'
           end);

  ------------------------------------------------------------------
  -- 2. Minimal fix: SELECT on the key columns only.
  --
  --    device_status arbiter = (device_id)
  --    status_events arbiter = (device_id, boot_id, seq)
  --
  --    These are values the device already knows -- it supplies them in every
  --    request -- so exposing them tells a holder of the anon key nothing it
  --    could not already infer. Telemetry columns stay unreadable.
  ------------------------------------------------------------------
  grant select (device_id) on public.device_status to anon;
  grant select (device_id, boot_id, seq) on public.status_events to anon;

  res := res || jsonb_build_object('n',2,'step','apply column-level SELECT',
           'result','APPLIED',
           'detail','device_status(device_id), status_events(device_id,boot_id,seq)');

  ------------------------------------------------------------------
  -- 3. Retest the upsert.
  ------------------------------------------------------------------
  ok := true;
  begin
    execute 'set local role anon';
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values (DEV, 1, 2, 0, 'ok', array['absent','absent','absent','absent'],
            array[true,true,true,true], 4, 3900, 70, false, 'probe', 'x')
    on conflict (device_id) do update set uptime_s = excluded.uptime_s;
  exception when others then
    ok := false;
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',3,'step','device_status upsert after fix',
           'result', case when ok then 'PASS' else 'STILL BLOCKED' end,
           'detail', case when ok
             then 'column-level SELECT is sufficient'
             else 'escalating to full-table SELECT in step 5' end);

  ------------------------------------------------------------------
  -- 4. Retest the event batch (ON CONFLICT DO NOTHING).
  ------------------------------------------------------------------
  ok := true;
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values (DEV, 1, 1, 0, 'boot', 0, 'ok',
            array['absent','absent','absent','absent'],
            array[true,true,true,true], 4, 70, false, 'probe')
    on conflict (device_id, boot_id, seq) do nothing;
  exception when others then
    ok := false;
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',4,'step','status_events upsert after fix',
           'result', case when ok then 'PASS' else 'STILL BLOCKED' end,
           'detail', case when ok
             then 'column-level SELECT is sufficient'
             else 'escalating to full-table SELECT in step 5' end);

  ------------------------------------------------------------------
  -- 5. Escalate only if the minimal grant was not enough.
  --    Full-table SELECT lets a device read other installations' telemetry,
  --    so it is a genuine loss of the no-read property -- taken only if
  --    required, and flagged clearly when it is.
  ------------------------------------------------------------------
  if not ok then
    grant select on public.device_status to anon;
    grant select on public.status_events to anon;

    ok := true;
    begin
      execute 'set local role anon';
      insert into public.device_status
        (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
         sensors_ok, sensors_online, battery_mv, battery_pct, charging,
         firmware, mac)
      values (DEV, 1, 3, 0, 'ok', array['absent','absent','absent','absent'],
              array[true,true,true,true], 4, 3900, 70, false, 'probe', 'x')
      on conflict (device_id) do update set uptime_s = excluded.uptime_s;
    exception when others then
      ok := false;
    end;
    execute 'reset role';

    res := res || jsonb_build_object('n',5,'step','escalated to full-table SELECT',
             'result', case when ok then 'APPLIED' else 'FAILED' end,
             'detail','SECURITY NOTE: anon can now read all telemetry. '
                      'Revisit with per-device JWTs.');
  else
    res := res || jsonb_build_object('n',5,'step','escalation not needed',
             'result','SKIPPED',
             'detail','minimal column-level grant was sufficient - '
                      'devices still cannot read telemetry');
  end if;

  ------------------------------------------------------------------
  -- 6. Confirm the no-read property still holds.
  ------------------------------------------------------------------
  ok := false;
  begin
    execute 'set local role anon';
    perform stack_count from public.device_status limit 1;
    ok := true;   -- readable: bad
  exception when insufficient_privilege then
    ok := false;  -- denied: good
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',6,'step','telemetry still unreadable by anon',
           'result', case when ok then 'FAIL' else 'PASS' end,
           'detail', case when ok
             then 'anon CAN read stack_count - full-table SELECT was granted'
             else 'anon cannot read stack_count; only key columns are visible'
           end);

  ------------------------------------------------------------------
  -- Cleanup.
  ------------------------------------------------------------------
  execute 'reset role';
  delete from public.status_events where device_id = DEV;
  delete from public.device_status where device_id = DEV;
  delete from public.devices       where device_id = DEV;

  for e in select * from jsonb_array_elements(res) loop
    insert into fix_results
    values ((e->>'n')::int, e->>'step', e->>'result', e->>'detail');
  end loop;
end $$;

select n, result, step, detail from fix_results order by n;
