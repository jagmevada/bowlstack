-- =====================================================================
--  Load-cell deployment -- location + food_slot for LDC-001..032.
--
--  Run as owner, after register_loadcells.sql. IDEMPOTENT: it states the
--  intended assignment absolutely rather than mutating what is there, so
--  re-running converges and repairs any drift.
--
--  THE RULE IS ONE LINE: LDC-nnn sits at the same physical position as
--  BWL-nnn.
--
--  A load-cell station is a platform UNDER the counter that the bowl stack
--  stands on, so the two are the same installation seen two ways -- one counts
--  what is buffered, the other weighs what is actually there. Pairing them by
--  number is what lets the dashboard put a measurement next to the estimate
--  for the same dish, which is the whole point of the mismatch check in
--  slot_quantity.
--
--  DERIVED, NOT COPIED, and that is a deliberate departure from
--  assign_devices.sql. That file argues for a literal table because the runs
--  are irregular -- Darshanarthi takes BWL-021/022 at slot 5, after the
--  Mahatma and Tiffin blocks -- so arithmetic over device numbers would be
--  wrong in a way that is hard to see. That argument is about deriving a
--  POSITION from a NUMBER, and it still holds.
--
--  This file derives a position from ANOTHER DEVICE'S position, which is a
--  different thing: it is the correspondence itself, stated once. A second
--  literal table here would have to be kept in step with the first by hand,
--  and the day somebody moves BWL-014 to Mahatma slot 5 without touching this
--  file, LDC-014 keeps weighing under a counter that is no longer there --
--  and the mismatch chip would blame the calibration.
--
--  (location, food_slot) is deliberately NOT unique, so a scale sharing a
--  position with three bowl counters is normal and expected.
-- =====================================================================

begin;

update public.devices d
   set location  = b.location,
       food_slot = b.food_slot,
       -- The bowl counter's label with the measurement named, so the two are
       -- recognisably the same position in a device list sorted by label.
       -- Still names the PHYSICAL position and never the dish -- what sits in
       -- slot 3 changes with the meal, and meal_food_mapping owns that.
       label     = case
                     when b.label is null then null
                     when b.location = 'R' then 'Reserved (scale)'
                     else b.label || ' scale'
                   end
  from public.devices b
 where d.device_id like 'LDC-%'
   and d.kind = 'scale'
   -- The pairing. substring from position 5 is the three-digit tail, so
   -- 'LDC-014' finds 'BWL-014'. An LDC with no BWL of the same number simply
   -- does not match and is left unassigned rather than being given a guess.
   and b.device_id = 'BWL-' || substring(d.device_id from 5)
   and (d.location  is distinct from b.location
     or d.food_slot is distinct from b.food_slot);

commit;

-- ---------------------------------------------------------------------
--  Verify: every scale paired, and sitting where its bowl counter sits.
-- ---------------------------------------------------------------------

-- Any scale that found no partner. Expect zero rows -- a row here means
-- register_devices.sql and register_loadcells.sql disagree about the fleet
-- size, which would leave a real station invisible to the dashboard.
select d.device_id as unpaired_scale
  from public.devices d
 where d.kind = 'scale'
   and not exists (select 1 from public.devices b
                    where b.device_id = 'BWL-' || substring(d.device_id from 5))
 order by d.device_id;

-- Any pair that disagrees about where it is. Expect zero rows; a row here
-- means this file has not been re-run since the bowl counter moved.
select d.device_id     as scale,
       d.location      as scale_location,
       d.food_slot     as scale_slot,
       b.device_id     as stack,
       b.location      as stack_location,
       b.food_slot     as stack_slot
  from public.devices d
  join public.devices b on b.device_id = 'BWL-' || substring(d.device_id from 5)
 where d.kind = 'scale'
   and (d.location is distinct from b.location
     or d.food_slot is distinct from b.food_slot)
 order by d.device_id;

-- The installation, both kinds side by side.
select coalesce(location, '(none)')        as location,
       coalesce(food_slot::text, '(none)') as slot,
       count(*) filter (where kind = 'stack')                    as stacks,
       count(*) filter (where kind = 'scale')                    as scales,
       string_agg(device_id, ', ' order by device_id)            as devices
  from public.devices
 group by location, food_slot
 order by location nulls last, food_slot nulls last;

select count(*) filter (where kind = 'scale' and location in ('D','M','T')) as scales_deployed,
       count(*) filter (where kind = 'scale' and location = 'R')            as scales_reserved,
       count(*) filter (where kind = 'scale' and location is null)          as scales_unassigned,
       count(*) filter (where kind = 'stack' and location in ('D','M','T')) as stacks_deployed
  from public.devices;
-- Expect: scales_deployed 24, scales_reserved 8, scales_unassigned 0,
--         stacks_deployed 24 -- an exact mirror of assign_devices.sql.
