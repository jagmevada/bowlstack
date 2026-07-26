-- =====================================================================
--  Bowlstack schema smoke test
--
--  Run AFTER schema.sql and migration_002_service_windows.sql, and BEFORE
--  flashing any device. Paste the whole file into the Supabase SQL editor and
--  run once. It returns a table of PASS/FAIL rows.
--
--  Several assertions are SUPPOSED to fail -- a device must NOT be able to
--  read your data. They are wrapped in exception handlers so the run
--  continues; a bare failing statement would abort the transaction and stop
--  the script at the first one, which is exactly what "permission denied for
--  table devices" means if you see it as a top-level error.
--
--  The assertion that matters most is #3: a device can INSERT a row carrying a
--  foreign key into `devices` while having NO permission to READ `devices`.
--  The entire privilege design rests on that.
--
--  Uses a throwaway device id (BWL-SMOKETEST) and deletes everything it
--  creates, so it will not disturb real installations. Safe to re-run.
-- =====================================================================

drop table if exists smoke_results;
create temp table smoke_results (
  n       int,
  check_name text,
  result  text,
  detail  text
);

do $$
declare
  r_n    int[]  := '{}';
  r_name text[] := '{}';
  r_res  text[] := '{}';
  r_det  text[] := '{}';
  i      int;
  v_age  interval;
  v_gap  numeric;
  DEV    constant text := 'BWL-SMOKETEST';

  procedure_placeholder text;
begin
  ------------------------------------------------------------------
  -- Clean slate, in case a previous run died before its cleanup.
  ------------------------------------------------------------------
  delete from public.status_events where device_id = DEV;
  delete from public.device_status where device_id = DEV;
  delete from public.devices       where device_id = DEV;

  insert into public.devices (device_id, label, location)
  values (DEV, 'smoke test', 'transient');

  ------------------------------------------------------------------
  -- Become the device.
  ------------------------------------------------------------------
  execute 'set local role anon';
  perform set_config('request.jwt.claims', '{"role":"anon"}', true);

  -- 1. The registry must be invisible to devices.
  begin
    perform 1 from public.devices limit 1;
    r_n:=r_n||1; r_name:=r_name||'devices unreadable by anon'; r_res:=r_res||'FAIL';
    r_det:=r_det||'anon CAN read devices; expected denial';
  exception when insufficient_privilege then
    r_n:=r_n||1; r_name:=r_name||'devices unreadable by anon'; r_res:=r_res||'PASS';
    r_det:=r_det||'permission denied, as intended';
  end;

  -- 2. A device must not read back any state.
  begin
    perform 1 from public.device_status limit 1;
    r_n:=r_n||2; r_name:=r_name||'device_status unreadable by anon'; r_res:=r_res||'FAIL';
    r_det:=r_det||'anon CAN read device_status; expected denial';
  exception when insufficient_privilege then
    r_n:=r_n||2; r_name:=r_name||'device_status unreadable by anon'; r_res:=r_res||'PASS';
    r_det:=r_det||'permission denied, as intended';
  end;

  -- 3. THE KEY ASSERTION: upsert succeeds through an FK into an unreadable
  --    table. Referential integrity checks bypass row security and run as the
  --    table owner. If this were wrong, every device write would fail.
  begin
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values
      (DEV, 12345, 100, 3, 'ok',
       array['present','present','present','absent'],
       array[true,true,true,true], 4, 3980, 76, false,
       '0.2.0', '8C:94:DF:4C:7A:04')
    on conflict (device_id) do update set uptime_s = excluded.uptime_s;
    r_n:=r_n||3; r_name:=r_name||'upsert through unreadable FK'; r_res:=r_res||'PASS';
    r_det:=r_det||'device_status accepted';
  exception when others then
    r_n:=r_n||3; r_name:=r_name||'upsert through unreadable FK'; r_res:=r_res||'FAIL';
    r_det:=r_det||(sqlstate||' '||sqlerrm);
  end;

  -- 4. Event insert with age_ms = 300000 ("this happened 5 minutes ago").
  begin
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      (DEV, 12345, 1, 300000, 'change', 2, 'ok',
       array['present','present','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0');
    r_n:=r_n||4; r_name:=r_name||'event insert'; r_res:=r_res||'PASS';
    r_det:=r_det||'accepted';
  exception when others then
    r_n:=r_n||4; r_name:=r_name||'event insert'; r_res:=r_res||'FAIL';
    r_det:=r_det||(sqlstate||' '||sqlerrm);
  end;

  -- 5. Idempotency: replaying (device_id, boot_id, seq) must be a no-op. This
  --    is what makes a retry after a lost response safe.
  begin
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      (DEV, 12345, 1, 300000, 'change', 2, 'ok',
       array['present','present','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0')
    on conflict (device_id, boot_id, seq) do nothing;
    r_n:=r_n||5; r_name:=r_name||'duplicate event ignored'; r_res:=r_res||'PASS';
    r_det:=r_det||'idempotent retry';
  exception when others then
    r_n:=r_n||5; r_name:=r_name||'duplicate event ignored'; r_res:=r_res||'FAIL';
    r_det:=r_det||(sqlstate||' '||sqlerrm);
  end;

  -- 6. Provisioning gate: BWL-000 is the version.h default and is deliberately
  --    never registered, so a unit flashed without -DBOWLSTACK_DEVICE_ID fails
  --    loudly instead of writing into a real installation's row.
  begin
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      ('BWL-000', 1, 1, 0, 'boot', 0, 'degraded',
       array['unknown','unknown','unknown','unknown'],
       array[false,false,false,false], 0, null, false, '0.2.0');
    r_n:=r_n||6; r_name:=r_name||'unregistered id rejected'; r_res:=r_res||'FAIL';
    r_det:=r_det||'BWL-000 was ACCEPTED; register nothing under that id';
  exception when foreign_key_violation then
    r_n:=r_n||6; r_name:=r_name||'unregistered id rejected'; r_res:=r_res||'PASS';
    r_det:=r_det||'23503, provisioning gate works';
  end;

  -- 7. Wire vocabulary is pinned. "OK" is the plotter display string; only
  --    lowercase "ok" is valid on the wire.
  begin
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      (DEV, 12345, 2, 0, 'change', 1, 'OK',
       array['present','absent','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0');
    r_n:=r_n||7; r_name:=r_name||'bad vocabulary rejected'; r_res:=r_res||'FAIL';
    r_det:=r_det||'uppercase stack_status was ACCEPTED';
  exception when check_violation then
    r_n:=r_n||7; r_name:=r_name||'bad vocabulary rejected'; r_res:=r_res||'PASS';
    r_det:=r_det||'CHECK constraint held';
  end;

  -- 8. Devices must not be able to delete history.
  begin
    delete from public.status_events where device_id = DEV;
    r_n:=r_n||8; r_name:=r_name||'anon cannot delete events'; r_res:=r_res||'FAIL';
    r_det:=r_det||'anon CAN delete';
  exception when insufficient_privilege then
    r_n:=r_n||8; r_name:=r_name||'anon cannot delete events'; r_res:=r_res||'PASS';
    r_det:=r_det||'permission denied, as intended';
  end;

  ------------------------------------------------------------------
  -- Back to owner to inspect what the device wrote.
  ------------------------------------------------------------------
  execute 'reset role';

  -- 9. Clock-free timestamps: recorded_at must sit ~300 s in the past even
  --    though the row was inserted a moment ago. This is what lets a device
  --    with no RTC replay buffered events with correct times.
  begin
    select received_at - recorded_at into v_age
      from public.status_events
     where device_id = DEV and boot_id = 12345 and seq = 1;

    if v_age is null then
      r_n:=r_n||9; r_name:=r_name||'recorded_at backdated by age_ms'; r_res:=r_res||'FAIL';
      r_det:=r_det||'event row not found';
    else
      v_gap := abs(extract(epoch from v_age) - 300);
      if v_gap < 2 then
        r_n:=r_n||9; r_name:=r_name||'recorded_at backdated by age_ms'; r_res:=r_res||'PASS';
        r_det:=r_det||(round(extract(epoch from v_age))::text||'s back, expected 300s');
      else
        r_n:=r_n||9; r_name:=r_name||'recorded_at backdated by age_ms'; r_res:=r_res||'FAIL';
        r_det:=r_det||('off by '||round(v_gap)::text||'s; trigger not applying age_ms');
      end if;
    end if;
  exception when others then
    r_n:=r_n||9; r_name:=r_name||'recorded_at backdated by age_ms'; r_res:=r_res||'FAIL';
    r_det:=r_det||(sqlstate||' '||sqlerrm);
  end;

  -- 10. updated_at must advance on the UPDATE half of an upsert. A column
  --     DEFAULT does not fire there, so without the trigger it would freeze at
  --     first contact -- and that column is what marks a device offline.
  begin
    update public.device_status
       set updated_at = now() - interval '1 hour'
     where device_id = DEV;

    execute 'set local role anon';
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values
      (DEV, 12345, 200, 3, 'ok',
       array['present','present','present','absent'],
       array[true,true,true,true], 4, 3980, 76, false,
       '0.2.0', '8C:94:DF:4C:7A:04')
    on conflict (device_id) do update set uptime_s = excluded.uptime_s;
    execute 'reset role';

    select now() - updated_at into v_age
      from public.device_status where device_id = DEV;

    if extract(epoch from v_age) < 5 then
      r_n:=r_n||10; r_name:=r_name||'updated_at refreshed on upsert'; r_res:=r_res||'PASS';
      r_det:=r_det||'trigger fired';
    else
      r_n:=r_n||10; r_name:=r_name||'updated_at refreshed on upsert'; r_res:=r_res||'FAIL';
      r_det:=r_det||('stale by '||round(extract(epoch from v_age))::text||'s; trigger not firing');
    end if;
  exception when others then
    execute 'reset role';
    r_n:=r_n||10; r_name:=r_name||'updated_at refreshed on upsert'; r_res:=r_res||'FAIL';
    r_det:=r_det||(sqlstate||' '||sqlerrm);
  end;

  -- 11. Service windows (migration_002). Skipped cleanly if not yet applied.
  begin
    perform public.in_service_window(now(), 'Asia/Kolkata', DEV);
    r_n:=r_n||11; r_name:=r_name||'service windows installed'; r_res:=r_res||'PASS';
    r_det:=r_det||(case when public.in_service_window(now(),'Asia/Kolkata',DEV)
                        then 'in service now' else 'outside service now' end);
  exception when undefined_function then
    r_n:=r_n||11; r_name:=r_name||'service windows installed'; r_res:=r_res||'SKIP';
    r_det:=r_det||'run migration_002_service_windows.sql';
  when others then
    r_n:=r_n||11; r_name:=r_name||'service windows installed'; r_res:=r_res||'FAIL';
    r_det:=r_det||(sqlstate||' '||sqlerrm);
  end;

  ------------------------------------------------------------------
  -- Cleanup. No enclosing ROLLBACK here on purpose: the results have to
  -- survive so the editor can display them.
  ------------------------------------------------------------------
  delete from public.status_events where device_id = DEV;
  delete from public.device_status where device_id = DEV;
  delete from public.devices       where device_id = DEV;

  for i in 1 .. coalesce(array_length(r_n, 1), 0) loop
    insert into smoke_results values (r_n[i], r_name[i], r_res[i], r_det[i]);
  end loop;
end $$;

-- The results.
select n,
       result,
       check_name,
       detail
  from smoke_results
 order by n;

-- Verdict.
select case
         when count(*) filter (where result = 'FAIL') = 0
           then 'ALL PASS - schema is ready, safe to flash devices'
         else count(*) filter (where result = 'FAIL')::text ||
              ' FAILED - do not flash until fixed'
       end as verdict
  from smoke_results;
