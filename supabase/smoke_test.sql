-- =====================================================================
--  Bowlstack -- schema smoke test.  20 assertions.
--
--  Run after schema.sql, and BEFORE flashing any device. Paste the whole file
--  into the Supabase SQL editor; it returns one table of PASS/FAIL rows plus a
--  verdict.
--
--  WHAT IT PROVES
--    0-1    the objects exist and registering a device provisions its status row
--    2-3    a device cannot READ anything -- not the registry, not telemetry
--    4      the PATCH hot path actually writes (asserted on ROWS AFFECTED)
--    5      a device cannot change which row it is
--    6-7    events insert, and a replay is idempotent via 23505
--    8      an unregistered device_id is refused -- the provisioning gate
--    9      the wire vocabulary is pinned by CHECK constraints
--    10-12  clock-free timestamps, the reported flag, service windows
--    13     a device cannot delete history
--    14-16  the menu is invisible and unwritable to devices, and the preload
--           inherits the previous meal while correctly flagging is_saved
--    17-19  the VIEWS, which are the front-end's whole interface: slot_overview
--           sums the stacks sharing a dish position, keeps untrustworthy counts
--           out of that total, and device_overview resolves the assignment
--
--  Several assertions are SUPPOSED to fail: a device must NOT be able to read
--  your data. Each is wrapped in an exception handler so the run continues, and
--  records the real SQLSTATE so a failure names its own cause instead of leaving
--  you to guess.
--
--  Fixtures: BWL-SMOKETEST at location 'R' with no slot, BWL-SMOKE2 and
--  BWL-SMOKE3 both at R/8 so the aggregation has something to aggregate, and menu
--  rows on an absurd date. 'R' is reserved and slot 8 is outside the deployed
--  1-5, so nothing can merge with live data. Everything is deleted afterwards, so
--  this is safe to re-run and safe against a populated database.
-- =====================================================================

drop table if exists smoke_results;
create temp table smoke_results (n int, result text, check_name text, detail text);

do $$
declare
  -- jsonb, not text[]: `text[] || 'literal'` is ambiguous -- Postgres resolves
  -- the untyped literal to text[] and then fails parsing it as an array (22P02).
  res   jsonb := '[]'::jsonb;
  e     jsonb;
  st    text;
  msg   text;
  v_age interval;
  v_gap numeric;
  v_rls boolean;
  v_n   int;
  v_txt text;
  v_saved boolean;
  v_trusted  bigint;
  v_reported bigint;
  v_cap      bigint;
  v_devs     bigint;
  v_degraded boolean;
  DEV   constant text := 'BWL-SMOKETEST';
  -- A second pair, both at the SAME dish position, to exercise the aggregation
  -- in slot_overview. Placed at R/8: 'R' is reserved so it is not a real serving
  -- area, and slot 8 is outside the 1-5 currently deployed, so the group cannot
  -- merge with live data even on a populated database.
  DEV2  constant text := 'BWL-SMOKE2';
  DEV3  constant text := 'BWL-SMOKE3';
  -- Menu fixtures live at location 'R' on an absurd date, so they cannot collide
  -- with a real menu even if this runs against a populated database.
  MDAY  constant date := date '1999-01-01';
begin
  execute 'reset role';

  -- Clean slate, in case a previous run died before its cleanup.
  delete from public.status_events where device_id in (DEV, DEV2, DEV3);
  delete from public.device_status where device_id in (DEV, DEV2, DEV3);
  delete from public.devices       where device_id in (DEV, DEV2, DEV3);

  -- 'R' (reserved) with no food_slot. A transient fixture must not claim a real
  -- serving position, and location is a D/M/T/R enum, so a descriptive string
  -- here would fail the CHECK.
  insert into public.devices (device_id, label, location, food_slot)
  values (DEV, 'smoke test', 'R', null);

  -- Two stacks sharing one dish position, for the slot_overview assertions.
  insert into public.devices (device_id, label, location, food_slot)
  values (DEV2, 'smoke test slot pair', 'R', 8),
         (DEV3, 'smoke test slot pair', 'R', 8);

  ------------------------------------------------------------------
  -- 0. RLS must be on. The GRANTs alone would still deny reads, but that is
  --    one layer instead of two, and Supabase's linter flags public tables
  --    exposed via PostgREST without RLS.
  ------------------------------------------------------------------
  select bool_and(c.relrowsecurity) into v_rls
    from pg_class c join pg_namespace nsp on nsp.oid = c.relnamespace
   where nsp.nspname = 'public'
     and c.relname in ('devices','device_status','status_events','service_windows',
                       'meal_food_mapping');

  res := res || jsonb_build_object('n',0,
           'r', case when v_rls then 'PASS' else 'FAIL' end,
           'c','RLS enabled on all tables',
           'd', case when v_rls then 'row level security is on'
                     else 'RLS is OFF - re-run schema.sql section 6' end);

  ------------------------------------------------------------------
  -- 1. Registering a device must auto-create its status row. This is what
  --    removes the need for an upsert on the device's hot path.
  ------------------------------------------------------------------
  select count(*) into v_n from public.device_status where device_id = DEV;
  res := res || jsonb_build_object('n',1,
           'r', case when v_n = 1 then 'PASS' else 'FAIL' end,
           'c','registering a device creates its status row',
           'd', v_n::text || ' row(s); trigger devices_create_status');

  ------------------------------------------------------------------
  -- Device-side assertions.
  --
  -- Each block sets its own role: catching an exception rolls back to the
  -- block's savepoint, which UNDOES an earlier SET LOCAL ROLE. A single
  -- set-role at the top would silently revert after the first caught
  -- assertion, and every later check would run as the owner -- reporting false
  -- passes on exactly the checks that prove a device cannot read your data.
  ------------------------------------------------------------------

  -- 2. The registry must be invisible to devices.
  begin
    execute 'set local role anon';
    perform 1 from public.devices limit 1;
    res := res || jsonb_build_object('n',2,'r','FAIL',
             'c','devices unreadable by anon','d','anon CAN read devices');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',2,'r','PASS',
             'c','devices unreadable by anon','d','permission denied, as intended');
  end;

  -- 3. A device must not read telemetry -- not its own, not anyone's.
  begin
    execute 'set local role anon';
    perform stack_count from public.device_status limit 1;
    res := res || jsonb_build_object('n',3,'r','FAIL',
             'c','telemetry unreadable by anon',
             'd','anon CAN read stack_count');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',3,'r','PASS',
             'c','telemetry unreadable by anon','d','permission denied, as intended');
  end;

  -- 4. THE HOT PATH: a plain UPDATE, which is what PostgREST PATCH issues.
  --    This replaced INSERT ... ON CONFLICT, which required full-table SELECT
  --    plus an RLS SELECT policy for anon -- i.e. letting every device read
  --    every installation's telemetry.
  --
  --    Asserts on ROWS AFFECTED, not merely the absence of an error. A
  --    zero-row UPDATE is a complete success as far as Postgres is concerned,
  --    so checking only for an exception would pass while the device wrote
  --    nothing at all -- which is exactly what happened when the SELECT policy
  --    was missing and the WHERE clause could not see the row.
  st := 'NO ERROR';
  v_n := -1;
  begin
    execute 'set local role anon';
    update public.device_status
       set boot_id = 12345, uptime_s = 100, stack_count = 3,
           stack_status = 'ok',
           levels = array['present','present','present','absent'],
           sensors_ok = array[true,true,true,true], sensors_online = 4,
           battery_mv = 3980, battery_level = 'good', charging = false,
           firmware = '0.2.0', mac = '8C:94:DF:4C:7A:04'
     where device_id = DEV;
    get diagnostics v_n = row_count;
  exception when others then
    st := sqlstate; msg := sqlerrm;
  end;
  res := res || jsonb_build_object('n',4,
           'r', case when st = 'NO ERROR' and v_n = 1 then 'PASS' else 'FAIL' end,
           'c','device UPDATE of its status row (PATCH hot path)',
           'd', case
                  when st <> 'NO ERROR' then st || coalesce(' | ' || msg, '')
                  when v_n = 0 then '0 rows matched - the WHERE cannot see the '
                                    'row; anon needs a SELECT policy on device_status'
                  else v_n::text || ' row updated'
                end);

  -- 5. A device must not be able to change which row it is.
  begin
    execute 'set local role anon';
    update public.device_status set device_id = 'BWL-HIJACK' where device_id = DEV;
    res := res || jsonb_build_object('n',5,'r','FAIL',
             'c','device cannot rewrite device_id','d','anon CAN change device_id');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',5,'r','PASS',
             'c','device cannot rewrite device_id','d','no UPDATE grant on that column');
  end;

  -- 6. Event insert. age_ms = 300000 means "this happened 5 minutes ago".
  st := 'NO ERROR';
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_level, charging, firmware)
    values (DEV, 12345, 1, 300000, 'change', 2, 'ok',
            array['present','present','absent','absent'],
            array[true,true,true,true], 4, 'good', false, '0.2.0');
  exception when others then
    st := sqlstate; msg := sqlerrm;
  end;
  res := res || jsonb_build_object('n',6,
           'r', case when st = 'NO ERROR' then 'PASS' else 'FAIL' end,
           'c','event insert','d', st || coalesce(' | ' || msg, ''));

  -- 7. Idempotency WITHOUT ON CONFLICT: a replayed event must raise 23505,
  --    which the firmware treats as "already recorded" and clears its buffer.
  st := 'NO ERROR';
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_level, charging, firmware)
    values (DEV, 12345, 1, 300000, 'change', 2, 'ok',
            array['present','present','absent','absent'],
            array[true,true,true,true], 4, 'good', false, '0.2.0');
  exception when unique_violation then
    st := '23505';
  when others then
    st := sqlstate; msg := sqlerrm;
  end;
  res := res || jsonb_build_object('n',7,
           'r', case when st = '23505' then 'PASS' else 'FAIL' end,
           'c','replayed event rejected with 23505',
           'd', case when st = '23505' then 'unique constraint held; retry is idempotent'
                     else 'expected 23505, got ' || st || coalesce(' | ' || msg, '') end);

  -- 8. Provisioning gate: BWL-000 is the version.h default and is deliberately
  --    never registered, so a unit flashed without -DBOWLSTACK_DEVICE_ID fails
  --    loudly instead of writing into a real installation's row.
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_level, charging, firmware)
    values ('BWL-000', 1, 1, 0, 'boot', 0, 'degraded',
            array['unknown','unknown','unknown','unknown'],
            array[false,false,false,false], 0, null, false, '0.2.0');
    res := res || jsonb_build_object('n',8,'r','FAIL',
             'c','unregistered id rejected','d','BWL-000 was ACCEPTED');
  exception when foreign_key_violation then
    res := res || jsonb_build_object('n',8,'r','PASS',
             'c','unregistered id rejected','d','23503, provisioning gate works');
  end;

  -- 9. Wire vocabulary is pinned. "OK" is the plotter display string; only
  --    lowercase "ok" is valid on the wire.
  begin
    execute 'set local role anon';
    insert into public.status_events
      (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
       levels, sensors_ok, sensors_online, battery_level, charging, firmware)
    values (DEV, 12345, 2, 0, 'change', 1, 'OK',
            array['present','absent','absent','absent'],
            array[true,true,true,true], 4, 'good', false, '0.2.0');
    res := res || jsonb_build_object('n',9,'r','FAIL',
             'c','bad vocabulary rejected','d','uppercase stack_status ACCEPTED');
  exception when check_violation then
    res := res || jsonb_build_object('n',9,'r','PASS',
             'c','bad vocabulary rejected','d','CHECK constraint held');
  end;

  ------------------------------------------------------------------
  -- Owner-side inspection.
  ------------------------------------------------------------------

  -- 10. Clock-free timestamps: recorded_at must sit ~300 s in the past even
  --     though the row was inserted a moment ago. This is what lets a device
  --     with no RTC replay buffered events with correct times.
  begin
    execute 'reset role';
    select received_at - recorded_at into v_age
      from public.status_events
     where device_id = DEV and boot_id = 12345 and seq = 1;

    if v_age is null then
      res := res || jsonb_build_object('n',10,'r','FAIL',
               'c','recorded_at backdated by age_ms','d','event row not found');
    else
      v_gap := abs(extract(epoch from v_age) - 300);
      res := res || jsonb_build_object('n',10,
               'r', case when v_gap < 2 then 'PASS' else 'FAIL' end,
               'c','recorded_at backdated by age_ms',
               'd', case when v_gap < 2
                    then round(extract(epoch from v_age))::text||'s back, expected 300s'
                    else 'off by '||round(v_gap)::text||'s' end);
    end if;
  exception when others then
    res := res || jsonb_build_object('n',10,'r','FAIL',
             'c','recorded_at backdated by age_ms','d',sqlstate||' '||sqlerrm);
  end;

  -- 11. updated_at and `reported` must be set by the trigger on UPDATE. A
  --     column DEFAULT cannot do this: defaults only apply on INSERT, and the
  --     device never inserts. updated_at is what marks a device offline.
  begin
    execute 'reset role';
    select now() - updated_at into v_age
      from public.device_status where device_id = DEV;
    select count(*) into v_n
      from public.device_status where device_id = DEV and reported;

    res := res || jsonb_build_object('n',11,
             'r', case when v_age is not null
                        and extract(epoch from v_age) < 30
                        and v_n = 1 then 'PASS' else 'FAIL' end,
             'c','updated_at and reported set on UPDATE',
             'd', coalesce(round(extract(epoch from v_age))::text||'s ago', 'null') ||
                  ', reported=' || v_n::text);
  exception when others then
    res := res || jsonb_build_object('n',11,'r','FAIL',
             'c','updated_at and reported set on UPDATE','d',sqlstate||' '||sqlerrm);
  end;

  -- 12. Service windows: absence of data outside meal hours must not alarm.
  begin
    execute 'reset role';
    res := res || jsonb_build_object('n',12,'r','PASS',
             'c','service windows installed',
             'd', case when public.in_service_window(now(),'Asia/Kolkata',DEV)
                       then 'in service now' else 'outside service hours now' end);
  exception when others then
    res := res || jsonb_build_object('n',12,'r','FAIL',
             'c','service windows installed','d',sqlstate||' '||sqlerrm);
  end;

  -- 13. Devices must not be able to delete history. Placed AFTER assertion 10,
  --     which reads that row back: if the grant were ever wrong and this delete
  --     succeeded, running it earlier would fail 10 as well and report one
  --     problem as two.
  begin
    execute 'set local role anon';
    delete from public.status_events where device_id = DEV;
    res := res || jsonb_build_object('n',13,'r','FAIL',
             'c','anon cannot delete events','d','anon CAN delete');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',13,'r','PASS',
             'c','anon cannot delete events','d','permission denied, as intended');
  end;

  -- 14. The menu must be invisible to devices. A device stores a slot NUMBER and
  --     never learns or needs the dish, so anon having no access here is a
  --     deliberate property rather than an omission -- otherwise the anon key in
  --     32 flash images would also read the whole site's configuration.
  begin
    execute 'set local role anon';
    perform 1 from public.meal_food_mapping limit 1;
    res := res || jsonb_build_object('n',14,'r','FAIL',
             'c','meal_food_mapping unreadable by anon',
             'd','anon CAN read the menu');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',14,'r','PASS',
             'c','meal_food_mapping unreadable by anon',
             'd','permission denied, as intended');
  end;

  -- 15. ...and unwritable, so a compromised device cannot rewrite the menu.
  begin
    execute 'set local role anon';
    insert into public.meal_food_mapping
      (location, meal_type, meal_date, food_slot, food_name)
    values ('R','Lunch',MDAY,1,'anon should not manage this');
    res := res || jsonb_build_object('n',15,'r','FAIL',
             'c','meal_food_mapping unwritable by anon',
             'd','anon CAN write the menu');
  exception when insufficient_privilege then
    res := res || jsonb_build_object('n',15,'r','PASS',
             'c','meal_food_mapping unwritable by anon',
             'd','permission denied, as intended');
  end;

  -- 16. Preload inherits the previous same-meal menu, and says it is inherited.
  --     The is_saved flag is the part worth testing: a preloaded form is
  --     pixel-identical to a saved one, so if it ever reported true for an
  --     inherited row an admin would believe a menu was recorded when no row
  --     exists. Verifies both directions -- inherited, then saved.
  st := 'NO ERROR';
  begin
    execute 'reset role';
    delete from public.meal_food_mapping where location = 'R' and meal_date in (MDAY, MDAY + 1);

    insert into public.meal_food_mapping
      (location, meal_type, meal_date, food_slot, food_name)
    values ('R','Lunch',MDAY,1,'SmokeDishA');

    -- Asking for the NEXT day must inherit MDAY and flag it as not saved.
    select food_name, is_saved into v_txt, v_saved
      from public.meal_mapping_preload('R','Lunch',MDAY + 1);

    if v_txt = 'SmokeDishA' and v_saved is false then
      -- Now save that day for real; the same call must flip to is_saved.
      insert into public.meal_food_mapping
        (location, meal_type, meal_date, food_slot, food_name)
      values ('R','Lunch',MDAY + 1,1,'SmokeDishB');

      select food_name, is_saved into v_txt, v_saved
        from public.meal_mapping_preload('R','Lunch',MDAY + 1);

      if v_txt = 'SmokeDishB' and v_saved is true then
        st := 'OK';
      else
        st := 'saved lookup returned ' || coalesce(v_txt,'null') ||
              '/is_saved=' || coalesce(v_saved::text,'null');
      end if;
    else
      st := 'inherited lookup returned ' || coalesce(v_txt,'null') ||
            '/is_saved=' || coalesce(v_saved::text,'null');
    end if;

    delete from public.meal_food_mapping where location = 'R' and meal_date in (MDAY, MDAY + 1);
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',16,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','meal_mapping_preload inherits, and flags is_saved',
           'd', case when st = 'OK'
                     then 'inherited previous day as draft, then saw the save'
                     else st end);

  ------------------------------------------------------------------
  -- The VIEWS. These are the front-end's entire interface, and slot_overview
  -- carries the newest and most consequential logic in the schema.
  ------------------------------------------------------------------

  -- 17. slot_overview must SUM stack_count across the stacks sharing a slot.
  --     (location, food_slot) is not unique precisely so it can: Darshanarthi
  --     runs three counters per dish position. A UI reading one device and
  --     calling it "Rice remaining" under-reports 3x on the busiest positions,
  --     so this is the assertion that stops that being written.
  st := 'NO ERROR';
  begin
    execute 'reset role';
    update public.device_status
       set boot_id = 1, uptime_s = 10, stack_count = 4, stack_status = 'ok',
           levels = array['present','present','present','present'],
           sensors_ok = array[true,true,true,true], sensors_online = 4,
           battery_level = 'good', charging = false, firmware = '0.2.0'
     where device_id = DEV2;
    update public.device_status
       set boot_id = 1, uptime_s = 10, stack_count = 3, stack_status = 'ok',
           levels = array['present','present','present','absent'],
           sensors_ok = array[true,true,true,true], sensors_online = 4,
           battery_level = 'good', charging = false, firmware = '0.2.0'
     where device_id = DEV3;

    select devices, bowls_capacity, bowls_trusted, bowls_reported
      into v_devs, v_cap, v_trusted, v_reported
      from public.slot_overview where location = 'R' and food_slot = 8;

    if v_devs = 2 and v_cap = 8 and v_trusted = 7 and v_reported = 7 then
      st := 'OK';
    else
      st := 'devices=' || coalesce(v_devs::text,'null') ||
            ' capacity=' || coalesce(v_cap::text,'null') ||
            ' trusted='  || coalesce(v_trusted::text,'null') ||
            ' reported=' || coalesce(v_reported::text,'null') ||
            ' (want 2/8/7/7)';
    end if;
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',17,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','slot_overview sums stacks sharing a dish position',
           'd', case when st = 'OK' then '4 + 3 = 7 bowls over 2 stacks, capacity 8'
                     else st end);

  -- 18. ...and must keep QUANTITY separate from TRUST. A degraded device's count
  --     is a lower bound, so it must not be folded into bowls_trusted -- that
  --     would silently overstate confidence in the number the kitchen acts on.
  --     It still counts in bowls_reported, and must raise any_degraded.
  st := 'NO ERROR';
  begin
    execute 'reset role';
    update public.device_status
       set boot_id = 1, uptime_s = 20, stack_status = 'degraded',
           levels = array['present','present','present','unknown'],
           sensors_ok = array[true,true,true,false], sensors_online = 3
     where device_id = DEV3;

    select bowls_trusted, bowls_reported, any_degraded
      into v_trusted, v_reported, v_degraded
      from public.slot_overview where location = 'R' and food_slot = 8;

    if v_trusted = 4 and v_reported = 7 and v_degraded then
      st := 'OK';
    else
      st := 'trusted=' || coalesce(v_trusted::text,'null') ||
            ' reported=' || coalesce(v_reported::text,'null') ||
            ' any_degraded=' || coalesce(v_degraded::text,'null') ||
            ' (want 4/7/true)';
    end if;
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',18,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','slot_overview excludes untrustworthy counts from the total',
           'd', case when st = 'OK'
                     then 'degraded stack dropped from trusted, kept in reported'
                     else st end);

  -- 19. device_overview must resolve the assignment and the meal clock. The
  --     dish name itself cannot be asserted here without making the test
  --     depend on the time of day -- current_meal_type is NULL outside service
  --     hours, which is correct -- so this checks the parts that hold either way.
  st := 'NO ERROR';
  begin
    execute 'reset role';
    select count(*) into v_n
      from public.device_overview
     where device_id = DEV2 and location = 'R' and food_slot = 8
       and awaiting_deployment = false;

    if v_n <> 1 then
      st := 'device_overview returned ' || v_n::text || ' matching row(s), want 1';
    elsif public.current_meal_date('Asia/Kolkata')
            <> (now() at time zone 'Asia/Kolkata')::date then
      st := 'current_meal_date disagrees with the local date';
    elsif coalesce(public.current_meal_type('Asia/Kolkata'), 'Lunch')
            not in ('Breakfast','Lunch','Dinner') then
      st := 'current_meal_type returned an out-of-vocabulary value';
    else
      st := 'OK';
    end if;
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',19,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','device_overview resolves assignment; meal clock is sane',
           'd', case when st = 'OK'
                     then 'row found, local date agrees, meal=' ||
                          coalesce(public.current_meal_type('Asia/Kolkata'),'none now')
                     else st end);

  ------------------------------------------------------------------
  -- Cleanup. Deliberately no enclosing ROLLBACK: that would discard the
  -- results along with the test data.
  ------------------------------------------------------------------
  execute 'reset role';
  delete from public.status_events where device_id in (DEV, DEV2, DEV3);
  delete from public.device_status where device_id in (DEV, DEV2, DEV3);
  delete from public.devices       where device_id in (DEV, DEV2, DEV3);
  delete from public.meal_food_mapping
   where location = 'R' and meal_date in (MDAY, MDAY + 1);

  for e in select * from jsonb_array_elements(res) loop
    insert into smoke_results
    values ((e->>'n')::int, e->>'r', e->>'c', e->>'d');
  end loop;
end $$;

-- Results AND verdict in ONE result set: the Supabase SQL editor displays only
-- the last statement's output, so two SELECTs would show the verdict and
-- silently discard the rows saying what failed.
select n, result, check_name, detail from smoke_results
union all
select 99,
       case when count(*) filter (where result = 'FAIL') = 0 then 'PASS' else 'FAIL' end,
       '== VERDICT ==',
       case when count(*) filter (where result = 'FAIL') = 0
            then 'all checks passed - safe to flash devices'
            else count(*) filter (where result = 'FAIL')::text || ' failed'
       end
  from smoke_results
 order by 1;
