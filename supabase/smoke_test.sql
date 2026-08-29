-- =====================================================================
--  Bowlstack -- schema smoke test.  25 assertions.
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
  -- Cross-area pair for slot_quantity, which groups by slot NUMBER across
  -- every serving area. These must sit in real serving areas ('R' is excluded
  -- from that view -- a reserved unit serves no dish and its bowls must never
  -- join a dish total), so collision safety comes from slot 8 instead: the
  -- deployment uses 1-5, so nothing live can merge with this group.
  DEV4  constant text := 'BWL-SMOKE4';
  DEV5  constant text := 'BWL-SMOKE5';
  -- Registered, assigned, and never heard from -- the undeployed-backup case.
  DEV6  constant text := 'BWL-SMOKE6';
  -- Menu fixtures live at location 'R' on an absurd date, so they cannot collide
  -- with a real menu even if this runs against a populated database.
  MDAY  constant date := date '1999-01-01';
begin
  execute 'reset role';

  -- Clean slate, in case a previous run died before its cleanup.
  delete from public.status_events where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6);
  delete from public.device_status where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6);
  delete from public.devices       where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6);

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
  -- 20. bowl_weight_g must reject a units mix-up at the edge.
  --     The whole point of the CHECK: someone typing kilograms into a grams
  --     field turns 6.5 kg into 6.5 g, and someone pasting a raw gram figure
  --     into a kilogram field turns it into 6500 kg. Both are silent on
  --     screen and both wreck the Master total, so both must be a 400 here
  --     rather than a dashboard reading 14 000 kg.
  --
  --     NULL must still be accepted: it is "no weight configured", which is
  --     a legitimate and common state, and is NOT zero.
  ------------------------------------------------------------------
  st := 'NO ERROR';
  begin
    execute 'reset role';
    -- NULL is fine.
    insert into public.meal_food_mapping
           (location, meal_type, meal_date, food_slot, food_name, bowl_weight_g)
    values ('R', 'Lunch', MDAY, 7, 'Weight-null', null);

    -- A real weight is fine.
    insert into public.meal_food_mapping
           (location, meal_type, meal_date, food_slot, food_name, bowl_weight_g)
    values ('R', 'Lunch', MDAY, 8, 'Weight-ok', 6500);

    v_n := 0;
    begin
      insert into public.meal_food_mapping
             (location, meal_type, meal_date, food_slot, food_name, bowl_weight_g)
      values ('R', 'Lunch', MDAY, 6, 'Weight-tiny', 6);        -- 6 g: kg typed as g
      v_n := v_n + 1;
    exception when check_violation then null;
    end;
    begin
      insert into public.meal_food_mapping
             (location, meal_type, meal_date, food_slot, food_name, bowl_weight_g)
      values ('R', 'Lunch', MDAY, 5, 'Weight-huge', 6500000);  -- 6500 kg
      v_n := v_n + 1;
    exception when check_violation then null;
    end;

    select bowl_weight_g into v_cap
      from public.meal_food_mapping
     where location = 'R' and meal_date = MDAY and meal_type = 'Lunch' and food_slot = 8;

    if v_n = 0 and v_cap = 6500 then
      st := 'OK';
    else
      st := v_n::text || ' out-of-range value(s) accepted; stored=' ||
            coalesce(v_cap::text, 'null');
    end if;
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',20,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','bowl_weight_g accepts NULL but rejects a units mix-up',
           'd', case when st = 'OK'
                     then 'null and 6500 g stored; 6 g and 6 500 000 g refused'
                     else st end);

  ------------------------------------------------------------------
  -- 21. meal_mapping_preload must carry the weight, not just the name.
  --     Inheriting a dish but not its weight would mean re-typing a figure
  --     that does not change from one day to the next -- and, worse, every
  --     carried-forward menu would land on Master as "set a weight".
  ------------------------------------------------------------------
  st := 'NO ERROR';
  begin
    execute 'reset role';
    select bowl_weight_g, is_saved into v_cap, v_saved
      from public.meal_mapping_preload('R', 'Lunch', MDAY)
     where food_slot = 8;

    if v_cap = 6500 and v_saved then
      st := 'OK';
    else
      st := 'weight=' || coalesce(v_cap::text, 'null') ||
            ' is_saved=' || coalesce(v_saved::text, 'null') || ' (want 6500/true)';
    end if;
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',21,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','meal_mapping_preload returns the per-bowl weight',
           'd', case when st = 'OK' then 'saved row preloads with 6500 g'
                     else st end);

  ------------------------------------------------------------------
  -- 22. slot_quantity must group ACROSS AREAS by slot number.
  --     This is the property that distinguishes it from slot_overview and
  --     the whole reason it exists: DEV4 sits at Darshanarthi slot 8 with 4
  --     bowls and DEV5 at Mahatma slot 8 with 3, and the Master Dashboard
  --     must show ONE row of 7 -- not two rows, and not one hall's share.
  --
  --     Slot 8 is used because nothing is deployed there, so this cannot
  --     merge with live data even on a populated database.
  ------------------------------------------------------------------
  st := 'NO ERROR';
  begin
    execute 'reset role';
    insert into public.devices (device_id, label, location, food_slot)
    values (DEV4, 'smoke test cross-area', 'D', 8),
           (DEV5, 'smoke test cross-area', 'M', 8);

    update public.device_status
       set boot_id = 1, uptime_s = 20, stack_count = 4, stack_status = 'ok',
           levels = array['present','present','present','present'],
           sensors_ok = array[true,true,true,true], sensors_online = 4
     where device_id = DEV4;
    update public.device_status
       set boot_id = 1, uptime_s = 20, stack_count = 3, stack_status = 'ok',
           levels = array['present','present','present','absent'],
           sensors_ok = array[true,true,true,true], sensors_online = 4
     where device_id = DEV5;

    select bowls_trusted, bowls_capacity, devices, jsonb_array_length(areas)
      into v_trusted, v_cap, v_devs, v_n
      from public.slot_quantity where food_slot = 8;

    if v_trusted = 7 and v_cap = 8 and v_devs = 2 and v_n = 2 then
      st := 'OK';
    else
      st := 'trusted=' || coalesce(v_trusted::text,'null') ||
            ' capacity=' || coalesce(v_cap::text,'null') ||
            ' devices=' || coalesce(v_devs::text,'null') ||
            ' areas=' || coalesce(v_n::text,'null') || ' (want 7/8/2/2)';
    end if;
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',22,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','slot_quantity sums one slot across every serving area',
           'd', case when st = 'OK'
                     then 'D8 4 bowls + M8 3 bowls = one row of 7, two areas'
                     else st end);

  ------------------------------------------------------------------
  -- 23. ...and must HIDE a slot no device has ever reported.
  --     This is what keeps the undeployed backup units off the Master
  --     Dashboard. Parking a permanent "no data" row there would train
  --     people to ignore a row that means something real when a deployed
  --     slot goes quiet -- so the gate is `has ever reported`, deliberately
  --     NOT `is reporting now`: a slot that dies mid-service must KEEP its
  --     row and its flags.
  ------------------------------------------------------------------
  st := 'NO ERROR';
  begin
    execute 'reset role';
    insert into public.devices (device_id, label, location, food_slot)
    values (DEV6, 'smoke test never reported', 'T', 7);

    select count(*) into v_n from public.slot_quantity where food_slot = 7;
    if v_n = 0 then
      -- Now let it report once, and the row must appear.
      update public.device_status
         set boot_id = 1, uptime_s = 20, stack_count = 2, stack_status = 'ok',
             levels = array['present','present','absent','absent'],
             sensors_ok = array[true,true,true,true], sensors_online = 4
       where device_id = DEV6;
      select count(*) into v_n from public.slot_quantity where food_slot = 7;
      if v_n = 1 then st := 'OK';
      else st := 'slot stayed hidden after its first report'; end if;
    else
      st := 'a never-reported slot produced ' || v_n::text || ' row(s), want 0';
    end if;
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',23,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','slot_quantity hides slots that have never reported',
           'd', case when st = 'OK'
                     then 'hidden while silent, appears on the first report'
                     else st end);

  ------------------------------------------------------------------
  -- 24. The weight arithmetic: SUM(bowls x that area''s own weight).
  --     Darshanarthi slot 8 gets 4 bowls at 5000 g and Mahatma slot 8 gets 3
  --     at 4000 g, so the honest total is 20 000 + 12 000 = 32 000 g. The
  --     naive formula -- 7 bowls x one weight -- gives 35 000 or 28 000, both
  --     wrong, and neither would look wrong on screen.
  --
  --     slot_quantity resolves the dish through current_meal_type(), which
  --     is NULL outside service hours by design. So this can only run DURING
  --     a meal window; outside one it reports SKIP rather than a false
  --     failure. (Same constraint assertion 19 documents.)
  ------------------------------------------------------------------
  st := 'NO ERROR';
  begin
    execute 'reset role';
    if public.current_meal_type('Asia/Kolkata') is null then
      st := 'SKIP';
    else
      insert into public.meal_food_mapping
             (location, meal_type, meal_date, food_slot, food_name, bowl_weight_g)
      values ('D', public.current_meal_type('Asia/Kolkata'),
              public.current_meal_date('Asia/Kolkata'), 8, 'Smoke-D', 5000),
             ('M', public.current_meal_type('Asia/Kolkata'),
              public.current_meal_date('Asia/Kolkata'), 8, 'Smoke-M', 4000)
      on conflict (location, meal_date, meal_type, food_slot) do update
        set food_name = excluded.food_name, bowl_weight_g = excluded.bowl_weight_g;

      select est_weight_g, bowl_weight_g into v_trusted, v_cap
        from public.slot_quantity where food_slot = 8;

      -- bowl_weight_g must be NULL: the two areas disagree, so there is no
      -- single "kg per bowl" that describes the slot -- even though the
      -- total is exact.
      if v_trusted = 32000 and v_cap is null then
        st := 'OK';
      else
        st := 'est=' || coalesce(v_trusted::text,'null') ||
              ' per_bowl=' || coalesce(v_cap::text,'null') ||
              ' (want 32000/null)';
      end if;
    end if;
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',24,
           'r', case when st = 'OK' then 'PASS'
                     when st = 'SKIP' then 'SKIP' else 'FAIL' end,
           'c','slot_quantity weighs each area against its own dish',
           'd', case when st = 'OK'
                     then '4x5000 + 3x4000 = 32000 g; no single per-bowl weight'
                     when st = 'SKIP'
                     then 'outside service hours - no current meal to resolve'
                     else st end);

  ------------------------------------------------------------------
  -- Cleanup. Deliberately no enclosing ROLLBACK: that would discard the
  -- results along with the test data.
  ------------------------------------------------------------------
  execute 'reset role';
  delete from public.status_events where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6);
  delete from public.device_status where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6);
  delete from public.devices       where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6);
  delete from public.meal_food_mapping
   where location = 'R' and meal_date in (MDAY, MDAY + 1);
  -- Assertion 24 writes into TODAY's real menu at slot 8 to exercise the
  -- weight arithmetic. Slot 8 is not deployed so nothing reads it, but it is
  -- a real location and date -- leaving it behind would put two invented
  -- dishes on a live Master Dashboard.
  delete from public.meal_food_mapping
   where food_slot = 8
     and location in ('D','M')
     and meal_date = public.current_meal_date('Asia/Kolkata')
     and food_name in ('Smoke-D','Smoke-M');

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
