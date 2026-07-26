-- =====================================================================
--  Why does ON CONFLICT fail for anon?
--
--  Run as owner. Returns one result set.
--
--  migration_003 tested two theories and both were wrong: neither
--  column-level SELECT nor full-table SELECT made the upsert work. It also
--  swallowed the real error with a bare `when others`, so the actual cause is
--  still unknown.
--
--  FIRST ACTION: this script revokes the full-table SELECT that migration_003
--  wrongly left behind, restoring the property that a device cannot read
--  telemetry. It does that before anything else, so even if the rest fails you
--  are back to a safe state.
--
--  Then it captures the EXACT SQLSTATE and message for each variant, and tests
--  the remaining hypotheses one at a time.
-- =====================================================================

drop table if exists upsert_diag;
create temp table upsert_diag (n int, hypothesis text, result text, detail text);

do $$
declare
  res  jsonb := '[]'::jsonb;
  e    jsonb;
  DEV  constant text := 'BWL-UPSERTFIX';
  st   text;
  msg  text;
  okc  boolean;
begin
  execute 'reset role';

  ------------------------------------------------------------------
  -- 0. UNDO the over-grant from migration_003 step 5.
  ------------------------------------------------------------------
  revoke select on public.device_status from anon;
  revoke select on public.status_events from anon;
  -- Re-apply key-column SELECT only; harmless either way and needed if the
  -- arbiter theory turns out to be partly right.
  grant select (device_id) on public.device_status to anon;
  grant select (device_id, boot_id, seq) on public.status_events to anon;

  -- Prove the no-read property is back.
  okc := false;
  begin
    execute 'set local role anon';
    perform stack_count from public.device_status limit 1;
    okc := true;
  exception when insufficient_privilege then
    okc := false;
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',0,
           'h','SECURITY: revoke full-table SELECT left by migration_003',
           'r', case when okc then 'STILL EXPOSED' else 'FIXED' end,
           'd', case when okc
                then 'anon can STILL read stack_count - revoke did not apply'
                else 'anon cannot read stack_count again' end);

  -- Clean slate.
  delete from public.status_events where device_id = DEV;
  delete from public.device_status where device_id = DEV;
  delete from public.devices       where device_id = DEV;
  insert into public.devices (device_id, label) values (DEV, 'upsert probe');

  ------------------------------------------------------------------
  -- 1. THE ACTUAL ERROR, with SQLSTATE and message. This is what
  --    migration_003 discarded.
  ------------------------------------------------------------------
  st := null; msg := null;
  begin
    execute 'set local role anon';
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values (DEV, 1, 1, 0, 'ok', array['absent','absent','absent','absent'],
            array[true,true,true,true], 4, 3900, 70, false, 'probe', 'x')
    on conflict (device_id) do update set uptime_s = excluded.uptime_s;
    st := 'NO ERROR';
  exception when others then
    st := sqlstate; msg := sqlerrm;
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',1,
           'h','device_status: INSERT ... ON CONFLICT DO UPDATE',
           'r', case when st = 'NO ERROR' then 'PASS' else 'FAIL' end,
           'd', st || coalesce(' | ' || msg, ''));

  ------------------------------------------------------------------
  -- 2. Plain INSERT, same table, same role. Isolates ON CONFLICT as the
  --    variable -- if this passes and 1 fails, nothing else is to blame.
  ------------------------------------------------------------------
  st := null; msg := null;
  begin
    execute 'set local role anon';
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values (DEV || '2', 1, 1, 0, 'ok', array['absent','absent','absent','absent'],
            array[true,true,true,true], 4, 3900, 70, false, 'probe', 'x');
    st := 'NO ERROR';
  exception when others then
    st := sqlstate; msg := sqlerrm;
  end;
  execute 'reset role';
  delete from public.device_status where device_id = DEV || '2';

  res := res || jsonb_build_object('n',2,
           'h','device_status: plain INSERT (control)',
           'r', case when st = 'NO ERROR' then 'PASS' else 'FAIL' end,
           'd', st || coalesce(' | ' || msg, ''));

  ------------------------------------------------------------------
  -- 3. ON CONFLICT DO NOTHING -- reads nothing from the existing row, so if
  --    this also fails the cause is the conflict PROBE, not the UPDATE.
  ------------------------------------------------------------------
  st := null; msg := null;
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values (DEV, 1, 1, 0, 'boot', 0, 'ok',
            array['absent','absent','absent','absent'],
            array[true,true,true,true], 4, 70, false, 'probe')
    on conflict (device_id, boot_id, seq) do nothing;
    st := 'NO ERROR';
  exception when others then
    st := sqlstate; msg := sqlerrm;
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',3,
           'h','status_events: ON CONFLICT DO NOTHING',
           'r', case when st = 'NO ERROR' then 'PASS' else 'FAIL' end,
           'd', st || coalesce(' | ' || msg, ''));

  ------------------------------------------------------------------
  -- 4. HYPOTHESIS: RLS needs a SELECT POLICY, not just a SELECT grant.
  --    The grant is the privilege layer; RLS is a second, independent gate,
  --    and the conflict probe is a read subject to both.
  ------------------------------------------------------------------
  drop policy if exists device_status_probe_device on public.device_status;
  create policy device_status_probe_device on public.device_status
    for select to anon using (true);
  drop policy if exists status_events_probe_device on public.status_events;
  create policy status_events_probe_device on public.status_events
    for select to anon using (true);

  st := null; msg := null;
  begin
    execute 'set local role anon';
    insert into public.device_status
      (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
       sensors_ok, sensors_online, battery_mv, battery_pct, charging,
       firmware, mac)
    values (DEV, 2, 5, 0, 'ok', array['absent','absent','absent','absent'],
            array[true,true,true,true], 4, 3900, 70, false, 'probe', 'x')
    on conflict (device_id) do update set uptime_s = excluded.uptime_s;
    st := 'NO ERROR';
  exception when others then
    st := sqlstate; msg := sqlerrm;
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',4,
           'h','+ RLS SELECT policy for anon (key-column grant only)',
           'r', case when st = 'NO ERROR' then 'PASS' else 'FAIL' end,
           'd', st || coalesce(' | ' || msg, ''));

  ------------------------------------------------------------------
  -- 5. If 4 still failed, add full-table SELECT on top of the policy.
  --    Tests whether policy AND full grant together are required.
  ------------------------------------------------------------------
  if st <> 'NO ERROR' then
    grant select on public.device_status to anon;
    grant select on public.status_events to anon;

    st := null; msg := null;
    begin
      execute 'set local role anon';
      insert into public.device_status
        (device_id, boot_id, uptime_s, stack_count, stack_status, levels,
         sensors_ok, sensors_online, battery_mv, battery_pct, charging,
         firmware, mac)
      values (DEV, 3, 9, 0, 'ok', array['absent','absent','absent','absent'],
              array[true,true,true,true], 4, 3900, 70, false, 'probe', 'x')
      on conflict (device_id) do update set uptime_s = excluded.uptime_s;
      st := 'NO ERROR';
    exception when others then
      st := sqlstate; msg := sqlerrm;
    end;
    execute 'reset role';

    res := res || jsonb_build_object('n',5,
             'h','+ full-table SELECT as well as the policy',
             'r', case when st = 'NO ERROR' then 'PASS' else 'FAIL' end,
             'd', st || coalesce(' | ' || msg, ''));

    -- Whatever the outcome, do not leave the broad grant in place. If it was
    -- genuinely required, migration_004 will re-apply it deliberately with the
    -- security trade-off written down.
    revoke select on public.device_status from anon;
    revoke select on public.status_events from anon;
    grant select (device_id) on public.device_status to anon;
    grant select (device_id, boot_id, seq) on public.status_events to anon;
  else
    res := res || jsonb_build_object('n',5,
             'h','full-table SELECT not needed',
             'r','SKIPPED',
             'd','the RLS SELECT policy alone fixed it');
  end if;

  ------------------------------------------------------------------
  -- 6. Final security state.
  ------------------------------------------------------------------
  okc := false;
  begin
    execute 'set local role anon';
    perform stack_count from public.device_status limit 1;
    okc := true;
  exception when insufficient_privilege then
    okc := false;
  end;
  execute 'reset role';

  res := res || jsonb_build_object('n',6,
           'h','FINAL: can anon read telemetry?',
           'r', case when okc then 'YES - exposed' else 'NO - safe' end,
           'd', case when okc
                then 'a SELECT policy plus grant makes stack_count readable; '
                     'acceptable only until per-device JWTs land'
                else 'only key columns are visible to anon' end);

  ------------------------------------------------------------------
  -- Cleanup of probe data. Policies added above are LEFT IN PLACE if they
  -- helped -- migration_004 will formalise them.
  ------------------------------------------------------------------
  execute 'reset role';
  delete from public.status_events where device_id like 'BWL-UPSERTFIX%';
  delete from public.device_status where device_id like 'BWL-UPSERTFIX%';
  delete from public.devices       where device_id like 'BWL-UPSERTFIX%';

  for e in select * from jsonb_array_elements(res) loop
    insert into upsert_diag values ((e->>'n')::int, e->>'h', e->>'r', e->>'d');
  end loop;
end $$;

select n, result, hypothesis, detail from upsert_diag order by n;
