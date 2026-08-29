-- =====================================================================
--  Register the load-cell stations.  LDC-001 .. LDC-032.
--
--  Run as owner in the Supabase SQL editor, AFTER migrate_loadcell.sql (which
--  adds devices.kind). Safe to re-run: every statement is ON CONFLICT DO
--  NOTHING or an absolute UPDATE, so a second run converges rather than
--  duplicating or resetting anything.
--
--  Mirrors register_devices.sql exactly -- same generate_series shape, same
--  timezone, same conflict handling -- because the load cells reuse the whole
--  registry, assignment, service-window and offline machinery the bowl
--  counters already have. The ONLY difference is kind = 'scale'.
--
--  REGISTERING IS WHAT PROVISIONS THE STATUS ROW. The devices_create_status
--  trigger inserts one row into device_status per registration, which is what
--  lets the firmware use a plain UPDATE on the hot path instead of an upsert.
--  A device_id absent from `devices` therefore has nowhere to write, and the
--  server answers 23503 -- which telemetry.cpp already latches as
--  "unprovisioned" and backs off hard from, because retrying cannot fix it.
--
--  LDC-000 MUST NEVER BE CREATED, for the same reason BWL-000 must not be.
--  include/version.h defaults BOWLSTACK_DEVICE_ID to BWL-000 and
--  platformio.ini overrides it to LDC-001 for the ws-s3-loadcell image -- so a
--  load-cell board flashed without that flag reports BWL-000 and is caught by
--  the provisioning gate rather than quietly writing into a real slot. Leaving
--  both zero-ids permanently unregistered is what keeps that gate armed.
-- =====================================================================

begin;

-- THIRTY-TWO, MIRRORING BWL-001..032 ONE FOR ONE. Not twenty, which is what
-- the Phase 2 sketch in docs/PHASE1_BOWL_WEIGHT.md guessed at before the fleet
-- was laid out: LDC-nnn is the load cell at the same physical position as
-- BWL-nnn, so the series has to match or the correspondence has holes in it.
-- assign_loadcells.sql relies on exactly this pairing.
--
-- Only device_id, timezone and kind are named here. Position comes from
-- assign_loadcells.sql, which reads it off the bowl counter of the same number
-- rather than repeating it.
insert into public.devices (device_id, timezone, kind)
select 'LDC-' || lpad(n::text, 3, '0'), 'Asia/Kolkata', 'scale'
  from generate_series(1, 32) as n
 on conflict (device_id) do nothing;

-- Absolute, not conditional, so a row created before migrate_loadcell.sql ran
-- -- and therefore defaulted to 'stack' -- is corrected rather than left
-- misclassified. A scale filed as a stack is worse than an unregistered one:
-- it would add four phantom bowls of capacity to whatever position it sits at,
-- and the numerator would stay right, so the only symptom is a progress bar
-- that reads low forever.
update public.devices
   set kind = 'scale'
 where device_id like 'LDC-%'
   and kind is distinct from 'scale';

commit;

-- ---------------------------------------------------------------------
--  DEPLOYMENT IS NOT HERE. Run supabase/assign_loadcells.sql next.
--
--  A station only reaches the Master Dashboard once it has a location and a
--  food_slot -- slot_quantity's WHERE requires both, and its area grouping is
--  what turns a weight into "12.4 kg of Rice at Darshanarthi".
--
--  That file does not repeat the assignment; it DERIVES it, by reading the
--  position off the bowl counter of the same number. LDC-nnn sits under
--  BWL-nnn, so one statement expresses the whole deployment and the two cannot
--  drift apart the day a stack is moved. See the note at the top of it for why
--  that is a different decision from assign_devices.sql's literal table.
-- ---------------------------------------------------------------------

-- ---------------------------------------------------------------------
--  Verification.
-- ---------------------------------------------------------------------
select count(*) filter (where kind = 'scale')                        as scales,
       count(*) filter (where kind = 'scale' and location is null)   as unassigned,
       count(*) filter (where kind = 'scale' and location is not null) as deployed,
       count(*) filter (where kind = 'stack')                        as stacks
  from public.devices;
-- Expect, before assign_loadcells.sql: scales 32, unassigned 32, deployed 0,
-- stacks 32. After it: scales 32, unassigned 0, deployed 24 (8 at 'R').
