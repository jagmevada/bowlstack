-- =====================================================================
--  Bowlstack -- schema smoke test.  43 assertions.
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
--    20-24  per-bowl weight: the units-mix-up rails, the preload, and
--           slot_quantity's cross-area, sum-then-multiply arithmetic
--    25-31  LOAD CELLS -- the second product sharing this schema. The kind
--           discriminator; the constraint that makes "a gram figure exists
--           exactly when the state says so" structural rather than a
--           convention; zero surviving as a real weight; the device write
--           path; and the two things that would be silently wrong if `kind`
--           were added and nothing else -- a scale inflating bowl capacity,
--           and the two pools being ADDED rather than one chosen over
--           the other -- a scale reading empty must not erase a full buffer
--    32-39  BUFFER PLATFORMS -- BWL ids weighing in kg (migrate_buffer.sql).
--           The kind and its write path; the guard keeping each kind to its
--           own half of device_status; the counter's 100 kg rail surviving
--           the 250 kg CHECK; a slot with nothing measured reading NULL, not
--           0; buffer and counter ADDED; a slot missing a platform marked
--           partial; history into weight_samples and never status_events;
--           and the stock series dropping a platform the moment it stops
--           being ok instead of carrying its last weight forward
--    40-42  AREA HUBS -- battery belongs to the hub (migrate_hubs.sql). A hub
--           registers and may write its power; a hub refuses a weight and a
--           platform refuses a battery, while an explicit NULL still passes;
--           and the platform node health writes, reads back through
--           device_overview, keeps 0 as 0 and leaves NULL as NULL
--
--  Several assertions are SUPPOSED to fail: a device must NOT be able to read
--  your data. Each is wrapped in an exception handler so the run continues, and
--  records the real SQLSTATE so a failure names its own cause instead of leaving
--  you to guess.
--
--  Fixtures: BWL-SMOKETEST at location 'R' with no slot, BWL-SMOKE2 and
--  BWL-SMOKE3 both at R/8 so the aggregation has something to aggregate, and menu
--  rows on an absurd date. 'R' is reserved and slot 8 is outside the deployed
--  1-5, so nothing can merge with live data. The buffer fixtures sit at slot 6
--  -- also undeployed, and apart from slot 8 so the figures 29-31 assert are
--  untouched. The hub fixture HUB-SMOKE sits at 'R' with no slot, as the real
--  hubs have none. Everything is deleted afterwards, so this is safe to re-run
--  and safe against a populated database.
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
  -- LOAD CELLS. SDEV1 stays a stack deliberately -- it is the fixture for "the
  -- kind column defaults correctly", which needs a row inserted the way every
  -- pre-load-cell script inserts one. SDEV2 is a scale parked at 'R' with no
  -- slot, so it touches no dish position. SDEV3 is a scale AT D/8, beside DEV4,
  -- which is what makes the capacity and precedence assertions possible.
  SDEV1 constant text := 'BWL-SMOKEKIND';
  SDEV2 constant text := 'LDC-SMOKE1';
  SDEV3 constant text := 'LDC-SMOKE2';
  SDEV4 constant text := 'LDC-SMOKE3';
  -- Whether migrate_loadcell.sql has been applied. Assertions 25-31 SKIP
  -- rather than FAIL without it, for the reason docs/PHASE1_BOWL_WEIGHT.md
  -- gives about the Master tab: "an un-migrated database says which of these
  -- you are looking at". Six red rows on a database that is simply older sends
  -- somebody hunting for a fault instead of to the SQL editor.
  v_lc  boolean;
  -- BUFFER PLATFORMS. BUF1 and BUF3 at D/6 beside a scale (BSC), BUF2 at M/6:
  -- slot 6, so the slot-8 figures above stay exactly what they assert.
  BUF1  constant text := 'BWL-SMOKEBUF1';
  BUF2  constant text := 'BWL-SMOKEBUF2';
  BUF3  constant text := 'BWL-SMOKEBUF3';
  BSC   constant text := 'LDC-SMOKEBUF';
  -- Whether migrate_buffer.sql has been applied; 32-39 SKIP without it, as
  -- 25-31 do without migrate_loadcell.sql.
  v_buf boolean;
  v_w   bigint;
  v_bg  bigint;
  v_cg  bigint;
  v_bw  bigint;
  v_part boolean;
  -- AREA HUBS. Whether migrate_hubs.sql has been applied; 40-42 SKIP without
  -- it. The fixture has no slot, so no stock view can see it.
  HUB   constant text := 'HUB-SMOKE';
  v_hub boolean;
  -- Menu fixtures live at location 'R' on an absurd date, so they cannot collide
  -- with a real menu even if this runs against a populated database.
  MDAY  constant date := date '1999-01-01';
begin
  execute 'reset role';

  -- Clean slate, in case a previous run died before its cleanup. The scale
  -- fixtures are listed here too: a run that dies between assertion 25 and the
  -- cleanup would otherwise leave SDEV3 sitting at D/8, where the next run's
  -- capacity assertion would count it and fail for a reason that has nothing
  -- to do with the code.
  --
  -- weight_samples FIRST: its foreign key to devices is ON DELETE RESTRICT,
  -- so a leftover buffer sample would block the devices delete below and
  -- abort the whole run. Dynamic, because the table does not exist before
  -- migrate_weight_samples.sql.
  if to_regclass('public.weight_samples') is not null then
    execute 'delete from public.weight_samples where device_id = any($1)'
      using array[BUF1, BUF2, BUF3, BSC, SDEV2, SDEV3, SDEV4];
  end if;
  -- The buffer ids too: if assertion 38 ever FAILS, a bowl-count event for
  -- BUF2 got in, and its restrict key would abort this run's successor.
  delete from public.status_events
   where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6, BUF1, BUF2, BUF3, BSC);
  delete from public.device_status
   where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6, SDEV1, SDEV2, SDEV3, SDEV4,
                       BUF1, BUF2, BUF3, BSC, HUB);
  delete from public.devices
   where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6, SDEV1, SDEV2, SDEV3, SDEV4,
                       BUF1, BUF2, BUF3, BSC, HUB, 'BWL-SMOKEBAD');
  delete from public.meal_food_mapping
   where location = 'D' and food_slot = 6 and food_name = 'Smoke-Buffer';

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
  -- 25-30. LOAD CELLS. Everything below is about the second product
  --        sharing this schema -- see supabase/migrate_loadcell.sql.
  ------------------------------------------------------------------
  select exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'devices'
                    and column_name = 'kind')
    into v_lc;

  if not v_lc then
    -- Not migrated. Say so once per assertion so the numbering stays stable
    -- and the verdict is not diluted by rows that were never applicable.
    for v_n in 25..31 loop
      res := res || jsonb_build_object('n', v_n, 'r','SKIP',
               'c','load cells: ' || case v_n
                     when 25 then 'devices.kind defaults to stack; a bad value is refused'
                     when 26 then 'weight_g exists exactly when weight_state is ok'
                     when 27 then 'a measured empty platform stores 0, not null'
                     when 28 then 'a scale may write its weight and may not read it'
                     when 29 then 'a scale does not inflate bowl capacity'
                     when 30 then 'slot_quantity ADDS buffered stock and the counter'
                     else         'two bad scales in one hall do not double its bowls'
                   end,
               'd','migrate_loadcell.sql has not been run on this database');
    end loop;
  else

  -- 25. The discriminator, and its closed vocabulary.
  execute 'reset role';
  st := 'OK';
  begin
    insert into public.devices (device_id, location, food_slot)
    values (SDEV1, 'R', null);
    select kind into v_txt from public.devices where device_id = SDEV1;
    if v_txt is distinct from 'stack' then
      st := 'default was ' || coalesce(v_txt, 'null') || ', want stack';
    end if;
    begin
      insert into public.devices (device_id, kind) values ('BWL-SMOKEBAD', 'weighbridge');
      st := 'a kind outside the vocabulary was ACCEPTED';
    exception when check_violation then null;
    end;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',25,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','devices.kind defaults to stack; a bad value is refused',
           'd', case when st = 'OK' then 'default stack, 23514 on nonsense' else st end);

  -- 26. THE HONESTY RULE, MADE STRUCTURAL. A gram figure exists exactly when
  --     weight_state says it does -- both directions. This is the constraint
  --     that stops a firmware bug putting an invented number on the dashboard,
  --     and the one scale_telemetry.cpp::weightState() is written to mirror.
  st := 'OK';
  begin
    insert into public.devices (device_id, location, food_slot, kind)
    values (SDEV2, 'R', null, 'scale');
    begin
      update public.device_status set weight_state = 'uncalibrated', weight_g = 1234
       where device_id = SDEV2;
      st := 'grams alongside a non-ok state were ACCEPTED';
    exception when check_violation then null;
    end;
    if st = 'OK' then
      begin
        update public.device_status set weight_state = 'ok', weight_g = null
         where device_id = SDEV2;
        st := '''ok'' with no grams was ACCEPTED';
      exception when check_violation then null;
      end;
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',26,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','weight_g exists exactly when weight_state is ok',
           'd', case when st = 'OK' then 'both directions refused with 23514' else st end);

  -- 27. ZERO IS A REAL WEIGHT. A measured, tared, empty platform reads 0 g and
  --     means "refill me"; NULL means nobody knows. The whole schema turns on
  --     keeping those apart, and a scale is where they are easiest to collapse.
  st := 'OK';
  begin
    update public.device_status
       set weight_state = 'ok', weight_g = 0, cells_online = 3
     where device_id = SDEV2;
    select weight_g into v_n from public.device_status where device_id = SDEV2;
    if v_n is null then st := '0 g came back as NULL';
    elsif v_n <> 0 then st := '0 g came back as ' || v_n; end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',27,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a measured empty platform stores 0, not null',
           'd', case when st = 'OK' then '0 g survives as 0' else st end);

  -- 28. The device write path, twin of assertions 3 and 4. A scale must be
  --     able to WRITE its weight and must not be able to READ it back.
  st := 'OK';
  begin
    execute 'set local role anon';
    update public.device_status
       set weight_state = 'ok', weight_g = 4321, cells_online = 3,
           counts_per_gram = 106.857, net_counts = 182771
     where device_id = SDEV2;
    get diagnostics v_n = row_count;
    if v_n <> 1 then st := 'PATCH matched ' || v_n || ' rows, want 1'; end if;
    if st = 'OK' then
      begin
        select weight_g into v_n from public.device_status where device_id = SDEV2;
        st := 'anon READ the weight back';
      exception when insufficient_privilege then null;
      end;
    end if;
    execute 'reset role';
  exception when others then
    st := sqlstate || ' ' || sqlerrm;
    execute 'reset role';
  end;
  res := res || jsonb_build_object('n',28,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a scale may write its weight and may not read it',
           'd', case when st = 'OK' then '1 row updated; SELECT denied 42501' else st end);

  -- 29. A SCALE IS NOT A STACK. The filter that would have been missed by
  --     adding the column and nothing else: a load cell sharing a served
  --     position must not add four bowls of capacity that no counter watches.
  --     The numerator stays right either way, so the only symptom of getting
  --     this wrong is a progress bar reading low forever.
  st := 'OK';
  begin
    insert into public.devices (device_id, location, food_slot, kind)
    values (SDEV3, 'D', 8, 'scale');
    update public.device_status
       set weight_state='ok', weight_g=12000, cells_online=3, reported=true
     where device_id = SDEV3;
    select devices, bowls_capacity, scales, measured_weight_g
      into v_devs, v_cap, v_n, v_trusted
      from public.slot_overview where location = 'D' and food_slot = 8;
    -- DEV4 is the only STACK at D/8, so one device and four bowls of capacity.
    if v_devs <> 1 or v_cap <> 4 then
      st := 'devices=' || v_devs || ' capacity=' || v_cap || ', want 1 and 4';
    elsif v_n <> 1 or v_trusted <> 12000 then
      st := 'scales=' || v_n || ' measured=' || coalesce(v_trusted::text,'null');
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',29,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a scale does not inflate bowl capacity',
           'd', case when st = 'OK'
                     then 'D/8: 1 stack, capacity 4, 1 scale at 12000 g' else st end);

  -- 30. BUFFER PLUS COUNTER, PER AREA -- added, never chosen between.
  --     D/8 holds 4 trusted bowls at 5000 g (20000 g buffered) AND a scale
  --     reading 12000 g on the counter: 32000 g at that hall. M/8 adds 3 bowls
  --     x 4000 g = 12000 g. Slot total 44000 g.
  --
  --     THE REGRESSION THIS GUARDS. An earlier cut applied a precedence rule
  --     -- measured beats estimated -- and D/8 would have reported 12000 g,
  --     silently discarding 20 kg of buffered food because a scale happened to
  --     be present. Found live, where an empty counter beside two 18 kg bowls
  --     made Master read 18 kg for a position holding 54 kg.
  st := 'OK';
  begin
    select weight_g, buffer_g, counter_g, est_weight_g
      into v_trusted, v_devs, v_cap, v_reported
      from public.slot_quantity where food_slot = 8;
    -- GATED ON THE ESTIMATE, not on weight_g. The measurement arrives whatever
    -- the clock says -- a scale needs no menu -- so weight_g is non-null even
    -- when the fixture menu could not be created, and this assertion would then
    -- fail with "12000, measured" for a reason that is about the hour of the
    -- day rather than about the precedence rule. The half that depends on the
    -- service window is the ESTIMATE, so that is what decides the skip.
    if v_reported is null then
      st := 'SKIP';   -- no current meal, so no menu fixture; see assertion 24
    elsif v_trusted <> 44000 then
      st := 'weight_g=' || coalesce(v_trusted::text,'null') || ', want 44000'
         || ' (buffer ' || coalesce(v_devs::text,'null')
         || ' + counter ' || coalesce(v_cap::text,'null') || ')';
    elsif v_devs <> 32000 or v_cap <> 12000 then
      st := 'buffer=' || coalesce(v_devs::text,'null')
         || ' counter=' || coalesce(v_cap::text,'null') || ', want 32000/12000';
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',30,
           'r', case when st = 'OK' then 'PASS'
                     when st = 'SKIP' then 'SKIP' else 'FAIL' end,
           'c','slot_quantity ADDS buffered stock and the counter',
           'd', case when st = 'OK'
                     then 'D 20000 buffered + 12000 counter, M 12000 = 44000'
                     when st = 'SKIP'
                     then 'outside service hours - no current meal to resolve'
                     else st end);

  -- 31. TWO UNHAPPY SCALES IN ONE HALL MUST NOT DOUBLE ITS BOWL COUNT.
  --
  --     The regression test for the worst bug found in review. slot_quantity
  --     collected the distinct non-ok scale states with a lateral unnest inside
  --     the grouped query, which is a row multiplier: a hall with TWO distinct
  --     bad states produced two copies of that hall's row, and every sum in the
  --     group counted it twice -- devices, bowls_capacity and bowls_trusted
  --     alike.
  --
  --     The bowl count is what makes it serious. It stayed internally
  --     consistent and nothing looked wrong; the position simply appeared to
  --     hold twice the food it had, whenever two of its scales were unhappy in
  --     two different ways.
  st := 'OK';
  begin
    insert into public.devices (device_id, location, food_slot, kind)
    values (SDEV4, 'D', 8, 'scale');
    -- Two DISTINCT non-ok states at the same hall. One alone cannot show this.
    update public.device_status
       set weight_state = 'uncalibrated', weight_g = null, reported = true
     where device_id = SDEV3;
    update public.device_status
       set weight_state = 'no_cells', weight_g = null, cells_online = 0, reported = true
     where device_id = SDEV4;

    select devices, bowls_capacity, bowls_trusted
      into v_devs, v_cap, v_trusted
      from public.slot_quantity where food_slot = 8;
    if v_devs is null then
      st := 'SKIP';                    -- no current meal; see assertion 24
    elsif v_devs <> 2 or v_cap <> 8 then
      -- DEV4 at D/8 and DEV5 at M/8 are the only STACKS: 2 devices, 8 bowls.
      st := 'devices=' || v_devs || ' capacity=' || v_cap || ', want 2 and 8'
         || ' (the per-area row was counted once per bad scale state)';
    elsif v_trusted <> 7 then
      st := 'bowls_trusted=' || v_trusted || ', want 7 (4 at D + 3 at M)';
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',31,
           'r', case when st = 'OK' then 'PASS'
                     when st = 'SKIP' then 'SKIP' else 'FAIL' end,
           'c','two bad scales in one hall do not double its bowls',
           'd', case when st = 'OK'
                     then '2 stacks, capacity 8, 7 bowls -- unchanged by 2 scale faults'
                     when st = 'SKIP'
                     then 'outside service hours - no current meal to resolve'
                     else st end);

  end if;   -- v_lc: the load-cell schema is present

  ------------------------------------------------------------------
  -- 32-39. BUFFER PLATFORMS -- see supabase/migrate_buffer.sql.
  ------------------------------------------------------------------
  execute 'reset role';
  select exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'device_status'
                    and column_name = 'bowls')
    into v_buf;

  if not v_buf then
    for v_n in 32..39 loop
      res := res || jsonb_build_object('n', v_n, 'r','SKIP',
               'c','buffers: ' || case v_n
                     when 32 then 'a buffer registers and may write a 200 kg reading'
                     when 33 then 'each kind may write only its own half of the row'
                     when 34 then 'the counter keeps its 100 kg rail and has no bowls'
                     when 35 then 'a slot with nothing measured reads NULL, not 0'
                     when 36 then 'buffer and counter are ADDED'
                     when 37 then 'a reported platform with no weight marks the slot partial'
                     when 38 then 'a buffer appends to weight_samples, never status_events'
                     else         'the stock series drops a platform that stops being ok'
                   end,
               'd','migrate_buffer.sql has not been run on this database');
    end loop;
  else

  -- 32. The kind, and the device write path at the top of the range. 190 kg
  --     of food in four bowls is a full buffer; the old 100 kg CHECK would
  --     have answered 400 and the platform would have gone silent.
  st := 'OK';
  begin
    insert into public.devices (device_id, location, food_slot, kind)
    values (BUF1, 'D', 6, 'buffer');
    begin
      execute 'set local role anon';
      -- No battery: a platform's power is its hub's (assertion 41), and on a
      -- database without migrate_hubs.sql leaving it out changes nothing.
      update public.device_status
         set boot_id = 7, uptime_s = 30, weight_state = 'ok', weight_g = 190000,
             gross_g = 200000, bowls = 4, bowls_confirmed = true,
             cells_online = 1, counts_per_gram = 20.7, net_counts = 4140000,
             firmware = 'smoke'
       where device_id = BUF1;
      get diagnostics v_n = row_count;
      if v_n <> 1 then st := 'buffer PATCH matched ' || v_n || ' rows, want 1'; end if;
    exception when others then
      st := 'buffer PATCH refused: ' || sqlstate || ' ' || sqlerrm;
    end;
    execute 'reset role';
    -- bowls and bowls_confirmed travel together, or not at all.
    if st = 'OK' then
      begin
        update public.device_status set bowls = 3, bowls_confirmed = null
         where device_id = BUF1;
        st := 'bowls without bowls_confirmed was ACCEPTED';
      exception when check_violation then null;
      end;
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',32,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a buffer registers and may write a 200 kg reading',
           'd', case when st = 'OK'
                     then 'kind buffer, 190 kg food / 4 bowls written by anon; '
                          'bowls without bowls_confirmed refused'
                     else st end);

  -- 33. THE GUARD, both directions. Neither write breaks a CHECK -- 2 is a
  --     valid stack_count and 1000 g beside 'ok' is a valid weight -- so only
  --     device_status_kind can refuse them. That is the point: after the
  --     cut-over a stack simulator still aimed at a BWL id writes perfectly
  --     well-formed bowl counts into a row the views read as kilograms.
  st := 'OK';
  begin
    execute 'set local role anon';
    update public.device_status set stack_count = 2 where device_id = BUF1;
    st := 'stack_count written to a buffer was ACCEPTED';
  exception when check_violation then null;
  when others then st := sqlstate || ' ' || sqlerrm;
  end;
  if st = 'OK' then
    begin
      execute 'set local role anon';
      update public.device_status set weight_state = 'ok', weight_g = 1000
       where device_id = DEV;
      st := 'a weight written to a stack was ACCEPTED';
    exception when check_violation then null;
    when others then st := sqlstate || ' ' || sqlerrm;
    end;
  end if;
  execute 'reset role';
  res := res || jsonb_build_object('n',33,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','each kind may write only its own half of the row',
           'd', case when st = 'OK'
                     then 'stack_* on a buffer and weight on a stack both 23514'
                     else st end);

  -- 34. The CHECK went to 250 kg for the buffers; the COUNTER's 100 kg rail
  --     moved into the guard and must still hold there. And a counter has no
  --     bowls -- a scale row carrying them would be a buffer filed wrong.
  st := 'OK';
  begin
    execute 'set local role anon';
    update public.device_status set weight_state = 'ok', weight_g = 150000
     where device_id = SDEV2;
    st := 'a 150 kg counter reading was ACCEPTED';
  exception when check_violation then null;
  when others then st := sqlstate || ' ' || sqlerrm;
  end;
  if st = 'OK' then
    begin
      execute 'set local role anon';
      update public.device_status
         set weight_state = 'ok', weight_g = 1000, bowls = 1,
             bowls_confirmed = true, gross_g = 3500
       where device_id = SDEV2;
      st := 'bowls written to a scale were ACCEPTED';
    exception when check_violation then null;
    when others then st := sqlstate || ' ' || sqlerrm;
    end;
  end if;
  execute 'reset role';
  res := res || jsonb_build_object('n',34,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','the counter keeps its 100 kg rail and has no bowls',
           'd', case when st = 'OK'
                     then '150 kg on a scale and bowls on a scale both 23514'
                     else st end);

  -- 35. NOTHING MEASURED IS NULL, NOT 0. D/6 gets a per-bowl weight on the
  --     menu and its only platform is still settling. Before this migration
  --     the per-bowl weight alone made the hall "known" -- an estimate of 0
  --     bowls x 5 kg -- so the slot read 0.0 kg: empty, go and refill it,
  --     about a position nothing had weighed. With no stack there is no
  --     estimate, and with no ok platform there is no figure.
  --
  --     The menu row goes in through last_served_meal(), which slot_quantity
  --     reads and which always answers, so this needs no service window.
  st := 'OK';
  begin
    execute 'reset role';
    insert into public.meal_food_mapping
           (location, meal_type, meal_date, food_slot, food_name, bowl_weight_g)
    select 'D', lm.meal_type, lm.meal_date, 6, 'Smoke-Buffer', 5000
      from public.last_served_meal('Asia/Kolkata') lm
    on conflict (location, meal_date, meal_type, food_slot) do nothing;
    update public.device_status
       set weight_state = 'settling', weight_g = null, gross_g = null,
           bowls = null, bowls_confirmed = null
     where device_id = BUF1;
    select weight_g, buffer_g into v_w, v_bg
      from public.slot_quantity where food_slot = 6;
    if not found then
      st := 'slot 6 is missing from slot_quantity';
    elsif v_w is not null or v_bg is not null then
      st := 'slot_quantity weight_g=' || coalesce(v_w::text,'null')
         || ' buffer_g=' || coalesce(v_bg::text,'null') || ', want NULL/NULL';
    else
      select weight_g into v_w from public.slot_overview
       where location = 'D' and food_slot = 6;
      if v_w is not null then
        st := 'slot_overview weight_g=' || v_w || ', want NULL';
      end if;
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',35,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a slot with nothing measured reads NULL, not 0',
           'd', case when st = 'OK'
                     then 'settling buffer + menu kg/bowl, no stack: NULL in both views'
                     else st end);

  -- 36. BUFFER PLUS COUNTER, ADDED. D/6: a buffer with 30 kg of food in two
  --     bowls and a counter scale at 12 kg; M/6: a buffer with 45 kg in three.
  --     Slot 87 kg = 75 kg buffered + 12 kg on the counter, 5 bowls, exact --
  --     and D's own row 42 kg on the Stock screen. The menu's 5 kg per bowl
  --     must NOT be applied to buffer bowls: they are weighed, not estimated.
  st := 'OK';
  begin
    execute 'reset role';
    insert into public.devices (device_id, location, food_slot, kind)
    values (BUF2, 'M', 6, 'buffer'),
           (BSC,  'D', 6, 'scale');
    update public.device_status
       set weight_state = 'ok', weight_g = 30000, gross_g = 35000,
           bowls = 2, bowls_confirmed = true, cells_online = 1
     where device_id = BUF1;
    update public.device_status
       set weight_state = 'ok', weight_g = 45000, gross_g = 52500,
           bowls = 3, bowls_confirmed = true, cells_online = 1
     where device_id = BUF2;
    update public.device_status
       set weight_state = 'ok', weight_g = 12000, cells_online = 3
     where device_id = BSC;
    select weight_g, buffer_g, counter_g, buffer_bowls, weight_is_partial
      into v_w, v_bg, v_cg, v_bw, v_part
      from public.slot_quantity where food_slot = 6;
    if v_w is distinct from 87000 or v_bg is distinct from 75000
       or v_cg is distinct from 12000 or v_bw is distinct from 5 then
      st := 'weight=' || coalesce(v_w::text,'null')
         || ' buffer=' || coalesce(v_bg::text,'null')
         || ' counter=' || coalesce(v_cg::text,'null')
         || ' bowls=' || coalesce(v_bw::text,'null') || ', want 87000/75000/12000/5';
    elsif v_part then
      st := 'every platform is ok, yet weight_is_partial is true';
    else
      select weight_g, buffer_g, measured_weight_g into v_w, v_bg, v_cg
        from public.slot_overview where location = 'D' and food_slot = 6;
      if v_w is distinct from 42000 or v_bg is distinct from 30000
         or v_cg is distinct from 12000 then
        st := 'slot_overview D/6 weight=' || coalesce(v_w::text,'null')
           || ' buffer=' || coalesce(v_bg::text,'null')
           || ' counter=' || coalesce(v_cg::text,'null') || ', want 42000/30000/12000';
      end if;
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',36,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','buffer and counter are ADDED',
           'd', case when st = 'OK'
                     then 'slot 6: 30000 + 45000 buffered + 12000 counter = 87000; '
                          'D/6 row 42000'
                     else st end);

  -- 37. PARTIAL. A third platform at D/6 reports no_cells: the 87 kg is now a
  --     lower bound, because food is standing on a platform nothing can weigh.
  --     The figure must stay (it is all real food) and say ">=". And a bowl
  --     count only remembered across a power cycle is flagged, not hidden.
  st := 'OK';
  begin
    execute 'reset role';
    insert into public.devices (device_id, location, food_slot, kind)
    values (BUF3, 'D', 6, 'buffer');
    update public.device_status
       set weight_state = 'no_cells', cells_online = 0
     where device_id = BUF3;
    update public.device_status set bowls_confirmed = false where device_id = BUF1;
    select weight_g, weight_is_partial, buffer_unconfirmed into v_w, v_part, v_saved
      from public.slot_quantity where food_slot = 6;
    if v_w is distinct from 87000 then
      st := 'weight_g=' || coalesce(v_w::text,'null') || ', want 87000 (kept, not dropped)';
    elsif v_part is not true then
      st := 'a reported no_cells buffer did not make the slot partial';
    elsif v_saved is not true then
      st := 'an unconfirmed bowl count was not flagged';
    else
      select buffer_issues into v_n from public.slot_overview
       where location = 'D' and food_slot = 6;
      if v_n is distinct from 1 then
        st := 'slot_overview D/6 buffer_issues=' || coalesce(v_n::text,'null') || ', want 1';
      end if;
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',37,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a reported platform with no weight marks the slot partial',
           'd', case when st = 'OK'
                     then '87000 kept, weight_is_partial, buffer_unconfirmed, '
                          'buffer_issues 1'
                     else st end);

  -- 38. HISTORY: a buffer appends to weight_samples, as anon, with its bowls
  --     and gross -- and status_events, the bowl-count history, refuses it.
  st := 'OK';
  begin
    execute 'set local role anon';
    insert into public.weight_samples
      (device_id, boot_id, seq, age_ms, reason, weight_state, weight_g,
       cells_online, net_counts, counts_per_gram, battery_mv, battery_level,
       firmware, bowls, bowls_confirmed, gross_g)
    values (BUF2, 7, 1, 0, 'boot', 'ok', 45000, 1, 931500, 20.7, 4000, 'good',
            'smoke', 3, true, 52500);
  exception when others then
    st := 'buffer sample refused: ' || sqlstate || ' ' || sqlerrm;
  end;
  if st = 'OK' then
    begin
      execute 'set local role anon';
      insert into public.status_events
        (device_id, boot_id, seq, age_ms, reason, stack_count, stack_status,
         levels, sensors_ok, sensors_online, battery_level, charging, firmware)
      values (BUF2, 7, 1, 0, 'boot', 2, 'ok',
              array['present','present','absent','absent'],
              array[true,true,true,true], 4, 'good', false, 'smoke');
      st := 'a bowl-count event for a buffer was ACCEPTED';
    exception when check_violation then null;
    when others then st := sqlstate || ' ' || sqlerrm;
    end;
  end if;
  execute 'reset role';
  res := res || jsonb_build_object('n',38,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a buffer appends to weight_samples, never status_events',
           'd', case when st = 'OK'
                     then 'weight_samples insert accepted; status_events 23514'
                     else st end);

  -- 39. NO FROZEN CARRY-FORWARD. BUF1 reported 30 kg twenty minutes ago and
  --     no_cells one minute ago. The series must carry 30 kg up to the
  --     no_cells sample and then STOP, marking the point partial -- not keep
  --     contributing the last good figure, which is food nothing is
  --     measuring. Timestamps come from age_ms, so this needs no clock.
  st := 'OK';
  begin
    execute 'reset role';
    insert into public.weight_samples
      (device_id, boot_id, seq, age_ms, reason, weight_state, weight_g,
       cells_online, firmware, bowls, bowls_confirmed, gross_g)
    values (BUF1, 9, 1, 1200000, 'boot',   'ok',       30000, 1, 'smoke', 2, true, 35000),
           (BUF1, 9, 2,   60000, 'change', 'no_cells', null,  0, 'smoke', null, null, null);
    select buffer_g, buffer_is_partial into v_bg, v_part
      from public.slot_stock_series
     where location = 'D' and food_slot = 6
     order by at_ts desc limit 1;
    select buffer_g into v_w
      from public.slot_stock_series
     where location = 'D' and food_slot = 6
     order by at_ts desc offset 1 limit 1;
    if v_bg is not null or v_part is not true then
      st := 'latest point buffer_g=' || coalesce(v_bg::text,'null')
         || ' partial=' || coalesce(v_part::text,'null') || ', want NULL/true';
    elsif v_w is distinct from 30000 then
      st := 'point before the no_cells sample buffer_g='
         || coalesce(v_w::text,'null') || ', want 30000';
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',39,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','the stock series drops a platform that stops being ok',
           'd', case when st = 'OK'
                     then '30000 until the no_cells sample, then NULL and partial'
                     else st end);

  end if;   -- v_buf: the buffer schema is present

  ------------------------------------------------------------------
  -- 40-42. AREA HUBS -- see supabase/migrate_hubs.sql. They reuse the
  --        buffer fixtures, which exist whenever this schema does: the
  --        migration refuses to run without migrate_buffer.sql.
  ------------------------------------------------------------------
  execute 'reset role';
  select exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'device_status'
                    and column_name = 'supply_mv')
    into v_hub;

  if not (v_hub and v_buf) then
    for v_n in 40..42 loop
      res := res || jsonb_build_object('n', v_n, 'r','SKIP',
               'c','hubs: ' || case v_n
                     when 40 then 'a hub registers and may write its power'
                     when 41 then 'a hub refuses a weight, a platform refuses a battery'
                     else         'node health reads back; 0 stays 0, NULL stays NULL'
                   end,
               'd','migrate_hubs.sql has not been run on this database');
    end loop;
  else

  -- 40. A hub registers, gets its status row from the trigger, and the
  --     device write path takes the whole power-and-identity PATCH --
  --     asserted on rows affected, as 4 is, because a PATCH that matches
  --     nothing is a 204 too.
  st := 'OK';
  begin
    insert into public.devices (device_id, location, food_slot, kind)
    values (HUB, 'R', null, 'hub');
    begin
      execute 'set local role anon';
      update public.device_status
         set boot_id = 3, uptime_s = 60, firmware = 'smoke',
             mac = '8C:94:DF:00:00:01', battery_mv = 4100,
             battery_level = 'good', charging = true, external_power = true
       where device_id = HUB;
      get diagnostics v_n = row_count;
      if v_n <> 1 then st := 'hub PATCH matched ' || v_n || ' rows, want 1'; end if;
    exception when others then
      st := 'hub PATCH refused: ' || sqlstate || ' ' || sqlerrm;
    end;
    execute 'reset role';
    if st = 'OK' then
      select battery_mv, charging into v_w, v_part
        from public.device_overview where device_id = HUB and kind = 'hub';
      if v_w is distinct from 4100 or v_part is not true then
        st := 'device_overview battery_mv=' || coalesce(v_w::text,'null')
           || ' charging=' || coalesce(v_part::text,'null') || ', want 4100/true';
      end if;
    end if;
  exception when others then st := sqlstate || ' ' || sqlerrm;
  end;
  res := res || jsonb_build_object('n',40,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a hub registers and may write its power',
           'd', case when st = 'OK'
                     then 'kind hub, battery/charging/mains written by anon, '
                          'read back through device_overview'
                     else st end);

  -- 41. THE GUARD, both directions, plus the NULL that must still pass.
  --     Every refused write is valid by its CHECK, so only device_status_kind
  --     can refuse it. A weight on a hub would be food at no dish; a battery
  --     on a platform is the copy of the hub's cell this migration removed,
  --     and the counter (BSC) is held to it as well as the buffers. Current
  --     firmware sends `"battery_mv": null` to a platform -- that must clear,
  --     not 400.
  st := 'OK';
  begin
    execute 'set local role anon';
    update public.device_status set weight_state = 'ok', weight_g = 1000
     where device_id = HUB;
    st := 'a weight written to a hub was ACCEPTED';
  exception when check_violation then null;
  when others then st := sqlstate || ' ' || sqlerrm;
  end;
  if st = 'OK' then
    begin
      execute 'set local role anon';
      update public.device_status set supply_mv = 5000 where device_id = HUB;
      st := 'node health written to a hub was ACCEPTED';
    exception when check_violation then null;
    when others then st := sqlstate || ' ' || sqlerrm;
    end;
  end if;
  if st = 'OK' then
    begin
      execute 'set local role anon';
      update public.device_status set battery_mv = 4000 where device_id = BUF1;
      st := 'battery_mv written to a buffer was ACCEPTED';
    exception when check_violation then null;
    when others then st := sqlstate || ' ' || sqlerrm;
    end;
  end if;
  if st = 'OK' then
    begin
      execute 'set local role anon';
      update public.device_status set external_power = true where device_id = BSC;
      st := 'external_power written to a scale was ACCEPTED';
    exception when check_violation then null;
    when others then st := sqlstate || ' ' || sqlerrm;
    end;
  end if;
  if st = 'OK' then
    begin
      execute 'set local role anon';
      update public.device_status
         set battery_mv = null, battery_level = null, charging = null,
             external_power = null
       where device_id = BUF1;
      get diagnostics v_n = row_count;
      if v_n <> 1 then st := 'null power PATCH matched ' || v_n || ' rows, want 1'; end if;
    exception when others then
      st := 'explicit null power on a buffer refused: ' || sqlstate || ' ' || sqlerrm;
    end;
  end if;
  execute 'reset role';
  res := res || jsonb_build_object('n',41,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','a hub refuses a weight, a platform refuses a battery',
           'd', case when st = 'OK'
                     then 'weight and supply_mv on a hub, battery on a buffer, '
                          'mains on a scale all 23514; explicit NULLs accepted'
                     else st end);

  -- 42. NODE HEALTH round trip. BUF1's node reports a healthy supply, no
  --     checksum failures and a dead-on zero: the 0s are MEASUREMENTS and
  --     must read back as 0. BUF2's node has reported nothing, and must read
  --     back NULL -- not 0, which would claim a check nobody ran. And the
  --     ranges hold.
  st := 'OK';
  begin
    execute 'set local role anon';
    update public.device_status
       set supply_mv = 5020, crc_errors = 0, responding = true, no_load_g = 0
     where device_id = BUF1;
    get diagnostics v_n = row_count;
    if v_n <> 1 then st := 'node-health PATCH matched ' || v_n || ' rows, want 1'; end if;
  exception when others then
    st := 'node-health PATCH refused: ' || sqlstate || ' ' || sqlerrm;
  end;
  execute 'reset role';
  if st = 'OK' then
    select supply_mv, crc_errors, no_load_g, responding into v_w, v_bg, v_cg, v_part
      from public.device_overview where device_id = BUF1;
    if v_w is distinct from 5020 or v_bg is distinct from 0
       or v_cg is distinct from 0 or v_part is not true then
      st := 'BUF1 supply_mv=' || coalesce(v_w::text,'null')
         || ' crc_errors=' || coalesce(v_bg::text,'null')
         || ' no_load_g=' || coalesce(v_cg::text,'null')
         || ' responding=' || coalesce(v_part::text,'null') || ', want 5020/0/0/true';
    end if;
  end if;
  if st = 'OK' then
    select count(*) into v_n from public.device_overview
     where device_id = BUF2
       and supply_mv is null and crc_errors is null
       and responding is null and no_load_g is null;
    if v_n <> 1 then st := 'BUF2 never reported node health, yet it is not NULL'; end if;
  end if;
  if st = 'OK' then
    begin
      update public.device_status set supply_mv = 25000 where device_id = BUF1;
      st := 'supply_mv 25000 was ACCEPTED';
    exception when check_violation then null;
    end;
  end if;
  if st = 'OK' then
    begin
      update public.device_status set crc_errors = -1 where device_id = BUF1;
      st := 'crc_errors -1 was ACCEPTED';
    exception when check_violation then null;
    end;
  end if;
  res := res || jsonb_build_object('n',42,
           'r', case when st = 'OK' then 'PASS' else 'FAIL' end,
           'c','node health reads back; 0 stays 0, NULL stays NULL',
           'd', case when st = 'OK'
                     then '5020 mV / 0 / 0 g / true read back; unreported reads NULL; '
                          'ranges refuse 25000 mV and -1'
                     else st end);

  end if;   -- v_hub: the hub schema is present

  ------------------------------------------------------------------
  -- Cleanup. Deliberately no enclosing ROLLBACK: that would discard the
  -- results along with the test data.
  ------------------------------------------------------------------
  execute 'reset role';
  -- weight_samples before devices -- the foreign key is ON DELETE RESTRICT.
  if to_regclass('public.weight_samples') is not null then
    execute 'delete from public.weight_samples where device_id = any($1)'
      using array[BUF1, BUF2, BUF3, BSC, SDEV2, SDEV3, SDEV4];
  end if;
  delete from public.status_events
   where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6, BUF1, BUF2, BUF3, BSC);
  delete from public.device_status
   where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6, SDEV1, SDEV2, SDEV3, SDEV4,
                       BUF1, BUF2, BUF3, BSC, HUB);
  delete from public.devices
   where device_id in (DEV, DEV2, DEV3, DEV4, DEV5, DEV6, SDEV1, SDEV2, SDEV3, SDEV4,
                       BUF1, BUF2, BUF3, BSC, HUB, 'BWL-SMOKEBAD');
  delete from public.meal_food_mapping
   where location = 'R' and meal_date in (MDAY, MDAY + 1);
  -- Assertion 35's menu row, by its name, so a real dish at D/6 (which the
  -- insert left alone) is never touched.
  delete from public.meal_food_mapping
   where location = 'D' and food_slot = 6 and food_name = 'Smoke-Buffer';
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
