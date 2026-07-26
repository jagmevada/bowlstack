-- =====================================================================
--  Bowlstack schema smoke test
--
--  Run AFTER schema.sql and migration_002_service_windows.sql, and BEFORE
--  flashing any device. Paste the whole file into the Supabase SQL editor and
--  run once. It returns a table of PASS/FAIL rows plus a verdict.
--
--  Several assertions are SUPPOSED to fail -- a device must NOT be able to
--  read your data. They are wrapped in exception handlers so the run
--  continues. If you ever see "permission denied for table devices" as a
--  top-level error instead of a PASS row, that is assertion 1 succeeding but
--  aborting the script.
--
--  The assertion that matters most is #3: a device can INSERT a row carrying a
--  foreign key into `devices` while having NO permission to READ `devices`.
--  The entire privilege design rests on that.
--
--  Uses a throwaway device id (BWL-SMOKETEST) and deletes everything it
--  creates, so real installations are untouched. Safe to re-run.
-- =====================================================================

drop table if exists smoke_results;
create temp table smoke_results (
  n          int,
  result     text,
  check_name text,
  detail     text
);

do $$
declare
  -- jsonb, not text[]: `text[] || 'some literal'` is ambiguous -- Postgres
  -- resolves the untyped literal to text[] and then fails trying to parse it
  -- as an array literal (22P02). jsonb concatenation has no such ambiguity.
  res   jsonb := '[]'::jsonb;
  e     jsonb;
  v_age interval;
  v_gap numeric;
  v_rls boolean;
  DEV   constant text := 'BWL-SMOKETEST';
begin
  ------------------------------------------------------------------
  -- Clean slate, in case a previous run died before its cleanup.
  ------------------------------------------------------------------
  execute 'reset role';
  delete from public.status_events where device_id = DEV;
  delete from public.device_status where device_id = DEV;
  delete from public.devices       where device_id = DEV;

  insert into public.devices (device_id, label, location)
  values (DEV, 'smoke test', 'transient');

  ------------------------------------------------------------------
  -- 0. RLS must be ON for all three tables. The GRANTs alone would still
  --    deny reads, but that is one layer instead of two, and Supabase's
  --    linter flags public tables exposed via PostgREST without RLS.
  ------------------------------------------------------------------
  select bool_and(c.relrowsecurity) into v_rls
    from pg_class c
    join pg_namespace nsp on nsp.oid = c.relnamespace
   where nsp.nspname = 'public'
     and c.relname in ('devices','device_status','status_events');

  res := res || jsonb_build_object('n',0,'result',
           case when v_rls then 'PASS' else 'FAIL' end,
           'name','RLS enabled on all tables','detail',
           case when v_rls then 'row level security is on'
                else 'RLS is OFF - re-run the "alter table ... enable row level security" lines from schema.sql'
           end);

  ------------------------------------------------------------------
  -- Device-side assertions.
  --
  -- Each block sets its own role. Catching an exception rolls back to the
  -- block's savepoint, which UNDOES a SET LOCAL ROLE made earlier -- so a
  -- single `set role` at the top would silently revert after the first caught
  -- assertion and every later one would run as the owner, reporting false
  -- passes.
  ------------------------------------------------------------------

  -- 1. The registry must be invisible to devices.
  begin
    execute 'set local role anon';
    perform 1 from public.devices limit 1;
    res := res || jsonb_build_object('n',1,'result','FAIL',
             'name','devices unreadable by anon',
             'detail','anon CAN read devices; expected denial');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',1,'result','PASS',
             'name','devices unreadable by anon',
             'detail','permission denied, as intended');
  end;

  -- 2. A device must not read back any state.
  begin
    execute 'set local role anon';
    perform 1 from public.device_status limit 1;
    res := res || jsonb_build_object('n',2,'result','FAIL',
             'name','device_status unreadable by anon',
             'detail','anon CAN read device_status; expected denial');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',2,'result','PASS',
             'name','device_status unreadable by anon',
             'detail','permission denied, as intended');
  end;

  -- 3. THE KEY ASSERTION: the upsert succeeds through a foreign key into a
  --    table anon cannot read. RI checks bypass row security and run as the
  --    table owner. If this were wrong, every device write would fail.
  begin
    execute 'set local role anon';
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
    res := res || jsonb_build_object('n',3,'result','PASS',
             'name','upsert through unreadable FK','detail','accepted');
  exception when others then
    res := res || jsonb_build_object('n',3,'result','FAIL',
             'name','upsert through unreadable FK',
             'detail',sqlstate||' '||sqlerrm);
  end;

  -- 4. Event insert. age_ms = 300000 means "this happened 5 minutes ago".
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      (DEV, 12345, 1, 300000, 'change', 2, 'ok',
       array['present','present','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0');
    res := res || jsonb_build_object('n',4,'result','PASS',
             'name','event insert','detail','accepted');
  exception when others then
    res := res || jsonb_build_object('n',4,'result','FAIL',
             'name','event insert','detail',sqlstate||' '||sqlerrm);
  end;

  -- 5. Idempotency: replaying (device_id, boot_id, seq) must be a no-op. This
  --    is what makes a retry after a lost response safe.
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      (DEV, 12345, 1, 300000, 'change', 2, 'ok',
       array['present','present','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0')
    on conflict (device_id, boot_id, seq) do nothing;
    res := res || jsonb_build_object('n',5,'result','PASS',
             'name','duplicate event ignored','detail','idempotent retry');
  exception when others then
    res := res || jsonb_build_object('n',5,'result','FAIL',
             'name','duplicate event ignored','detail',sqlstate||' '||sqlerrm);
  end;

  -- 6. Provisioning gate: BWL-000 is the version.h default and is deliberately
  --    never registered, so a unit flashed without -DBOWLSTACK_DEVICE_ID fails
  --    loudly rather than writing into a real installation's row.
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      ('BWL-000', 1, 1, 0, 'boot', 0, 'degraded',
       array['unknown','unknown','unknown','unknown'],
       array[false,false,false,false], 0, null, false, '0.2.0');
    res := res || jsonb_build_object('n',6,'result','FAIL',
             'name','unregistered id rejected',
             'detail','BWL-000 was ACCEPTED; it must stay unregistered');
  exception when foreign_key_violation then
    res := res || jsonb_build_object('n',6,'result','PASS',
             'name','unregistered id rejected',
             'detail','23503, provisioning gate works');
  end;

  -- 7. Wire vocabulary is pinned. "OK" is the plotter display string; only
  --    lowercase "ok" is valid on the wire.
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_pct, charging, firmware)
    values
      (DEV, 12345, 2, 0, 'change', 1, 'OK',
       array['present','absent','absent','absent'],
       array[true,true,true,true], 4, 76, false, '0.2.0');
    res := res || jsonb_build_object('n',7,'result','FAIL',
             'name','bad vocabulary rejected',
             'detail','uppercase stack_status was ACCEPTED');
  exception when check_violation then
    res := res || jsonb_build_object('n',7,'result','PASS',
             'name','bad vocabulary rejected','detail','CHECK constraint held');
  end;

  ------------------------------------------------------------------
  -- Owner-side inspection of what the device wrote.
  --
  -- Note the ordering: the delete test (8) runs LAST, after these read-backs.
  -- It is the only destructive assertion, and if anon turned out to be able to
  -- delete, running it here would wipe the row assertion 9 needs -- reporting
  -- one real problem as two.
  ------------------------------------------------------------------

  -- 9. Clock-free timestamps: recorded_at must sit ~300 s in the past even
  --    though the row was inserted a moment ago. This is what lets a device
  --    with no RTC replay buffered events with correct times.
  begin
    execute 'reset role';
    select received_at - recorded_at into v_age
      from public.status_events
     where device_id = DEV and boot_id = 12345 and seq = 1;

    if v_age is null then
      res := res || jsonb_build_object('n',9,'result','FAIL',
               'name','recorded_at backdated by age_ms',
               'detail','event row not found');
    else
      v_gap := abs(extract(epoch from v_age) - 300);
      res := res || jsonb_build_object('n',9,'result',
               case when v_gap < 2 then 'PASS' else 'FAIL' end,
               'name','recorded_at backdated by age_ms','detail',
               case when v_gap < 2
                    then round(extract(epoch from v_age))::text||'s back, expected 300s'
                    else 'off by '||round(v_gap)::text||'s; trigger not applying age_ms'
               end);
    end if;
  exception when others then
    res := res || jsonb_build_object('n',9,'result','FAIL',
             'name','recorded_at backdated by age_ms',
             'detail',sqlstate||' '||sqlerrm);
  end;

  -- 10. updated_at must advance on the UPDATE half of an upsert. A column
  --     DEFAULT does not fire there, so without the trigger it would freeze at
  --     first contact -- and that column is what marks a device offline.
  begin
    execute 'reset role';
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

    res := res || jsonb_build_object('n',10,'result',
             case when extract(epoch from v_age) < 5 then 'PASS' else 'FAIL' end,
             'name','updated_at refreshed on upsert','detail',
             case when extract(epoch from v_age) < 5 then 'trigger fired'
                  else 'stale by '||round(extract(epoch from v_age))::text||'s; trigger not firing'
             end);
  exception when others then
    res := res || jsonb_build_object('n',10,'result','FAIL',
             'name','updated_at refreshed on upsert',
             'detail',sqlstate||' '||sqlerrm);
  end;

  -- 11. Service windows (migration_002). SKIPs cleanly if not yet applied.
  begin
    execute 'reset role';
    res := res || jsonb_build_object('n',11,'result','PASS',
             'name','service windows installed','detail',
             case when public.in_service_window(now(),'Asia/Kolkata',DEV)
                  then 'in service now' else 'outside service hours now' end);
  exception when undefined_function then
    res := res || jsonb_build_object('n',11,'result','SKIP',
             'name','service windows installed',
             'detail','run migration_002_service_windows.sql');
  when others then
    res := res || jsonb_build_object('n',11,'result','FAIL',
             'name','service windows installed',
             'detail',sqlstate||' '||sqlerrm);
  end;

  -- 8. Devices must not be able to delete history. Runs last because it is
  --    destructive; see the note above.
  begin
    execute 'set local role anon';
    delete from public.status_events where device_id = DEV;
    res := res || jsonb_build_object('n',8,'result','FAIL',
             'name','anon cannot delete events','detail','anon CAN delete');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',8,'result','PASS',
             'name','anon cannot delete events',
             'detail','permission denied, as intended');
  end;

  ------------------------------------------------------------------
  -- Cleanup. Deliberately no enclosing ROLLBACK: that would discard the
  -- results along with the test data, which is what made an earlier version
  -- report nothing at all.
  ------------------------------------------------------------------
  execute 'reset role';
  delete from public.status_events where device_id = DEV;
  delete from public.device_status where device_id = DEV;
  delete from public.devices       where device_id = DEV;

  for e in select * from jsonb_array_elements(res) loop
    insert into smoke_results
    values ((e->>'n')::int, e->>'result', e->>'name', e->>'detail');
  end loop;
end $$;

-- Results.
select n, result, check_name, detail
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
