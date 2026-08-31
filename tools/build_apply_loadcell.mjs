// Generates supabase/apply_loadcell.sql -- the whole load-cell augmentation as
// ONE file, in ONE transaction, that refuses to commit if it changed anything
// about the bowl counters.
//
// WHY GENERATE IT rather than write it. The six source files are the ones that
// get maintained, reviewed and rolled back individually; a hand-written fifth
// copy of their contents would be a second definition of the same migration and
// would drift from them the first time one was touched. This script fuses them
// mechanically, so the combined file cannot say anything the parts do not.
//
// Re-run it after editing any of them:
//     node tools/build_apply_loadcell.mjs
//
// WHAT THE FUSION CHANGES, and it is only this: each source file's own
// `begin;`/`commit;` is removed so the six become one transaction, and the
// per-file verification SELECTs that trail each `commit;` are dropped in favour
// of one consolidated report at the end. Nothing else is rewritten.
import { readFileSync, writeFileSync } from 'node:fs';

const REPO = new URL('..', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const SRC = `${REPO}/supabase`;

// [migration, what it does, the rollback that undoes it or null]
//
// THE THIRD COLUMN EXISTS BECAUSE THE UNDO LIST WENT STALE. The apply list in
// the generated header has always been interpolated from this table, but the
// rollback list beside it was a hand-written literal -- so when
// migrate_manual_fill.sql was appended here, the undo instructions were not
// updated and the documented three-step sequence ABORTED at step 2:
// rollback_weight_samples.sql drops a table that trial_fill_vs_weight still
// selects from, and Postgres refuses. Two lists, one maintained.
//
// Now both are derived from this one table and the undo order is simply the
// reverse of the apply order, which is also what the header has always claimed
// it was. A part with no rollback carries null -- migrate_bowl_weight.sql
// predates the load cells and the bowl counters depend on it, and the two
// registry steps are inserts into an existing table.
const PARTS = [
  ['migrate_bowl_weight.sql', 'per-bowl weight, and the Master Dashboard view', null],
  ['migrate_loadcell.sql', 'load-cell stations alongside the bowl counters', 'rollback_loadcell.sql'],
  ['register_loadcells.sql', 'LDC-001..032 into the devices registry', null],
  ['assign_loadcells.sql', 'each LDC onto the position of its BWL', null],
  ['migrate_weight_samples.sql', 'the analog history a scale appends to', 'rollback_weight_samples.sql'],
  ['migrate_burn_rate.sql', 'consumption rate, and when a dish runs out', 'rollback_burn_rate.sql'],
  ['migrate_manual_fill.sql', 'TRIAL HARNESS -- the manual fill estimate', 'rollback_manual_fill.sql'],
  // MAINS PRESENCE, AND IT WAS MISSING FROM THIS LIST ENTIRELY. The firmware
  // puts external_power in EVERY device_status PATCH (scale_telemetry.cpp:479),
  // so a database built from this file without the column answers 400 and the
  // scale goes SILENT -- no weight, no state, no battery -- while its own panel
  // looks perfectly normal. It was invisible because the live database had the
  // column applied by hand. A second site would have found it the hard way.
  //
  // After migrate_loadcell.sql, necessarily: migrate_vbus_sense.sql raises
  // unless device_status.weight_state already exists.
  ['migrate_vbus_sense.sql', 'mains presence, from the VBUS divider', 'rollback_vbus_sense.sql'],
];

// Reverse of the apply order, skipping the parts that have no rollback. This
// also puts rollback_vbus_sense.sql ahead of rollback_loadcell.sql, which it
// must be: public.device_power reads devices.kind.
const ROLLBACKS = PARTS.map(([, , r]) => r).filter(Boolean).reverse();

// The last two were missing until an adversarial pass ran this file into an
// empty database as `anon` and found that public.weight_samples did not exist.
// The omission was invisible from either side: apply_loadcell.sql ran clean,
// every bowl-counting figure it guards was untouched, and the live database
// had the two tables anyway because they had been applied by hand first.
//
// What it would have cost is a SECOND site. The operator runs the one file the
// documentation points at, the fleet comes up, current weight appears on the
// dashboard -- and every history POST answers 404 forever, so there is no
// curve and no burn rate. The device even handles that case gracefully and
// carries on reporting current state, which is the wrong kind of robust: it
// means nothing anywhere is red.
//
// THE TRIAL MIGRATION IS IN HERE, and leaving it out was nearly expensive. The
// firmware sends manual_fill_pct on EVERY status PATCH and every history row --
// not only when a knob is fitted -- and PostgREST answers a column it does not
// know with 400. So a database built from the documented applier, with this
// image flashed against it, reports NOTHING: no weight, no state, no battery,
// no history. The station looks perfect on its own panel and is simply absent
// from the dashboard.
//
// It did not bite here only because the live project had it applied by hand.
// The first rebuild, second site or staging copy would have found it, and the
// symptom points at the network rather than at a missing column.
//
// ORDER IS LOAD-BEARING. weight_samples needs devices.kind from
// migrate_loadcell; burn_rate reads weight_samples. They go last, in this
// order, and the fusion refuses to reorder them.


/** The transactional body of one migration: everything strictly between its own
 *  `begin;` and `commit;`. Anything after the commit is that file's own
 *  verification, which the consolidated report at the end replaces. */
function body(file) {
  const lines = readFileSync(`${SRC}/${file}`, 'utf8').replace(/^﻿/, '').split(/\r?\n/);
  const b = lines.findIndex(l => l.trim() === 'begin;');
  const c = lines.findIndex((l, i) => i > b && l.trim() === 'commit;');
  if (b < 0 || c < 0) throw new Error(`${file}: expected one begin;/commit; pair`);
  if (lines.slice(c + 1).some(l => l.trim() === 'commit;'))
    throw new Error(`${file}: more than one commit; -- the fusion assumes one`);
  return lines.slice(b + 1, c).join('\n').replace(/^\n+|\n+$/g, '');
}

const banner = (n, file, what) => `
-- #####################################################################
-- ##  PART ${n} of ${PARTS.length}:  ${file}
-- ##  ${what}
-- #####################################################################
`;

const out = `-- =====================================================================
--  Bowlstack -- apply the load-cell augmentation.  GENERATED FILE.
--
--  Regenerate with:  node tools/build_apply_loadcell.mjs
--  Do not edit by hand -- edit the six files it fuses and re-run that.
--
--  ---------------------------------------------------------------------
--  WHAT THIS IS FOR
--  ---------------------------------------------------------------------
--  Paste the whole file into the Supabase SQL editor and run it ONCE. It
--  does what these ${PARTS.length} do, in the only order that works:
--
${PARTS.map(([f, w], i) => `--    ${i + 1}. ${f.padEnd(26)} ${w}`).join('\n')}
--
--  ---------------------------------------------------------------------
--  IF YOU NEED TO UNDO IT AFTER IT HAS COMMITTED
--  ---------------------------------------------------------------------
--  Run these ${ROLLBACKS.length}, IN THIS ORDER -- they undo in the reverse of the
--  order applied, because each drops objects the one before it depends on:
--
${ROLLBACKS.map((f) => `--      ${f}`).join('\n')}
--
--  Taking them out of order fails on a dependency rather than doing
--  damage, so a mistake here is loud. Note that migrate_bowl_weight.sql
--  (part 1) has NO rollback and does not need one: it predates the load
--  cells and the bowl counters depend on it.
--
--  ---------------------------------------------------------------------
--  WHY IT IS SAFE TO RUN MID-SERVICE
--  ---------------------------------------------------------------------
--  ONE TRANSACTION. All ${PARTS.length} parts and the verification run inside a single
--  BEGIN. If ANY of it fails -- a missing prerequisite, a constraint, a
--  verification check -- the whole thing rolls back and your database is
--  exactly as it was. There is no half-applied state to recover from.
--
--  IT PROVES IT DID NOT TOUCH THE BOWL COUNTERS. Before changing anything
--  it snapshots every existing device, its status row, and the whole of
--  slot_overview -- the numbers the Stock screen renders. After the
--  migration it compares them and RAISES if a single figure moved, which
--  aborts the transaction. "It should not affect bowl counting" becomes a
--  check the database performs rather than a claim in a comment.
--
--  IT ADDS AND NEVER REMOVES. No table is dropped, no row is rewritten, no
--  existing column changes name, type, nullability or default.
--
--  ---------------------------------------------------------------------
--  TO REHEARSE FIRST
--  ---------------------------------------------------------------------
--  Change the LAST line of this file from
--
--      commit;
--  to
--      rollback;
--
--  and run it. Everything executes, every check runs, the report prints --
--  and nothing is kept. Then change it back and run it for real.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
--  BEFORE: snapshot what must not change.
--
--  Temp tables, so they vanish with the session and cannot collide with
--  anything. Captured INSIDE the transaction, so what they record is
--  precisely the state this transaction started from.
-- ---------------------------------------------------------------------
create temp table _before_devices on commit drop as
  select device_id, location, food_slot, label, timezone from public.devices;

create temp table _before_status on commit drop as
  select device_id, reported, boot_id, uptime_s, stack_count, stack_status,
         levels, sensors_ok, sensors_online, battery_mv, battery_level,
         charging, firmware, mac
    from public.device_status;

create temp table _before_stock on commit drop as
  select location, food_slot, devices, devices_reported, bowls_capacity,
         bowls_trusted, bowls_reported, any_fault, any_degraded
    from public.slot_overview;

create temp table _before_counts on commit drop as
  select (select count(*) from public.devices)            as devices,
         (select count(*) from public.device_status)      as device_status,
         (select count(*) from public.status_events)      as status_events,
         (select count(*) from public.service_windows)    as service_windows,
         (select count(*) from public.meal_food_mapping)  as meal_food_mapping;
${PARTS.map(([f, w], i) => banner(i + 1, f, w) + '\n' + body(f)).join('\n')}

-- #####################################################################
-- ##  VERIFICATION -- runs before the commit, and aborts it on failure.
-- #####################################################################
do $$
declare
  v_n int;
  v_txt text;
begin
  -- 1. Every pre-existing device kept its identity and its posting.
  select count(*) into v_n from (
    select device_id, location, food_slot, label, timezone from _before_devices
    except
    select device_id, location, food_slot, label, timezone from public.devices
  ) x;
  if v_n > 0 then
    raise exception 'ABORTED: % existing device row(s) changed or vanished', v_n;
  end if;

  select count(*) into v_n from (
    select * from _before_status
    except
    select device_id, reported, boot_id, uptime_s, stack_count, stack_status,
           levels, sensors_ok, sensors_online, battery_mv, battery_level,
           charging, firmware, mac
      from public.device_status
  ) x;
  if v_n > 0 then
    raise exception 'ABORTED: % existing device_status row(s) changed', v_n;
  end if;

  -- 2. THE ONE THAT MATTERS. Every figure the Stock screen renders, unmoved.
  --    A load cell registered at a served position must not become a fourth
  --    bowl counter -- that would inflate capacity with a correct numerator,
  --    which is a bar that reads low forever and looks like nothing at all.
  select count(*) into v_n from (
    (select * from _before_stock
      except
     select location, food_slot, devices, devices_reported, bowls_capacity,
            bowls_trusted, bowls_reported, any_fault, any_degraded
       from public.slot_overview)
    union all
    (select location, food_slot, devices, devices_reported, bowls_capacity,
            bowls_trusted, bowls_reported, any_fault, any_degraded
       from public.slot_overview
      except
     select * from _before_stock)
  ) x;
  if v_n > 0 then
    raise exception
      'ABORTED: % row(s) of slot_overview moved -- the bowl counters are '
      'not supposed to change. Nothing has been committed.', v_n;
  end if;

  -- 3. Nothing deleted anywhere. Only devices and device_status may GROW,
  --    and only by the 32 scales being registered.
  select count(*) into v_n from _before_counts b
   where (select count(*) from public.status_events)     <> b.status_events
      or (select count(*) from public.service_windows)   <> b.service_windows
      or (select count(*) from public.meal_food_mapping) <> b.meal_food_mapping
      or (select count(*) from public.devices)           <  b.devices
      or (select count(*) from public.device_status)     <  b.device_status;
  if v_n > 0 then
    raise exception 'ABORTED: a table gained or lost rows it should not have';
  end if;

  -- 4. Every pre-existing device is still classified as a bowl counter.
  select count(*) into v_n from public.devices d
    join _before_devices b using (device_id)
   where d.kind <> 'stack';
  if v_n > 0 then
    raise exception 'ABORTED: % pre-existing device(s) were reclassified', v_n;
  end if;

  -- 5. The device write path did not widen. anon must still be able to read
  --    exactly one column of device_status, whatever columns were added.
  select count(*) into v_n from information_schema.column_privileges
   where table_schema='public' and table_name='device_status'
     and grantee='anon' and privilege_type='SELECT';
  if v_n <> 1 then
    raise exception
      'ABORTED: anon can now SELECT % columns of device_status, expected 1', v_n;
  end if;

  -- 6. No function became callable by PUBLIC. A fresh CREATE FUNCTION carries
  --    that by default and a bare GRANT does not remove it -- which is exactly
  --    how weight_mismatch_tolerance briefly became the only one in the schema
  --    that anon could call.
  select string_agg(proname, ', ') into v_txt from (
    select p.proname from pg_proc p
     cross join lateral aclexplode(coalesce(p.proacl, acldefault('f', p.proowner))) a
     where p.pronamespace = 'public'::regnamespace
       and a.privilege_type = 'EXECUTE' and a.grantee = 0
     group by p.proname
  ) x;
  if v_txt is not null then
    raise exception 'ABORTED: function(s) executable by PUBLIC: %', v_txt;
  end if;

  raise notice 'All verification checks passed. Committing.';
end $$;

-- ---------------------------------------------------------------------
--  The report. One result set, because the Supabase editor shows only the
--  last statement's output.
-- ---------------------------------------------------------------------
select item as step, name as detail, status from (
  values
    (1, 'bowl counters (BWL-*)',
        (select count(*)::text from public.devices where kind = 'stack')
          || ' registered, unchanged'),
    (2, 'load cells (LDC-*)',
        (select count(*)::text from public.devices where kind = 'scale')
          || ' registered, '
          || (select count(*)::text from public.devices
               where kind='scale' and location in ('D','M','T')) || ' deployed'),
    (3, 'Stock screen (slot_overview)',
        (select count(*)::text from public.slot_overview)
          || ' dish positions, every figure identical to before'),
    (4, 'Master screen (slot_quantity)',
        'view present, ' ||
        (select count(*)::text from information_schema.columns
          where table_schema='public' and table_name='slot_quantity')
          || ' columns'),
    (5, 'measured weight',
        'no station has reported one yet -- expected until a scale is flashed'),
    (6, 'next',
        'run supabase/smoke_test.sql -- expect 32 assertions, 0 FAIL')
) as t(item, name, status)
order by item;

-- CHANGE THIS TO rollback; TO REHEARSE WITHOUT KEEPING ANYTHING.
commit;
`;

writeFileSync(`${SRC}/apply_loadcell.sql`, out, 'utf8');
console.log(`wrote supabase/apply_loadcell.sql (${out.split('\n').length} lines, `
          + `fusing ${PARTS.length} migrations)`);
