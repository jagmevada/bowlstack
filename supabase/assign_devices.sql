-- =====================================================================
--  Permanent device assignment -- location + food_slot for BWL-001..032
--
--  Run as owner, AFTER migrations/001_location_food_slot.sql. IDEMPOTENT: it
--  states the intended assignment absolutely rather than mutating what is there,
--  so re-running converges on the same result and repairs any drift.
--
--  Replaces the earlier deploy_devices.sql, which assigned 15 units across
--  3 areas x 5 slots on the assumption of ONE stack per position. That is not the
--  real deployment: Darshanarthi slot 1 has three stacks. Anything still holding
--  the old mapping should re-run this.
--
--  ASSIGNMENT IS PERMANENT, MENU IS NOT
--  ------------------------------------
--  A device's location and food_slot change only when hardware is physically
--  moved or replaced. What that slot SERVES changes three times a day and lives
--  in meal_food_mapping. Devices never store food names -- see
--  docs/meal_mapping.md.
--
--  SEVERAL STACKS PER SLOT IS NORMAL
--  ---------------------------------
--  (location, food_slot) is deliberately NOT unique. A busy counter needs more
--  than one stack of rice, so remaining stock for a dish is the SUM of
--  stack_count over the devices sharing the slot. Read public.slot_overview
--  rather than summing in the client.
-- =====================================================================

begin;

-- The assignment, stated explicitly rather than derived. The runs are not
-- regular -- Darshanarthi takes BWL-021/022 at slot 5, after the Mahatma and
-- Tiffin blocks -- so arithmetic over device numbers would be wrong in a way
-- that is hard to see. A literal table can be read against the physical
-- installation.
with intended(device_id, location, food_slot) as (
  values
    -- Darshanarthi: 14 stacks over 5 dish positions
    ('BWL-001', 'D', 1), ('BWL-002', 'D', 1), ('BWL-003', 'D', 1),
    ('BWL-004', 'D', 2), ('BWL-005', 'D', 2), ('BWL-006', 'D', 2),
    ('BWL-007', 'D', 3), ('BWL-008', 'D', 3), ('BWL-009', 'D', 3),
    ('BWL-010', 'D', 4), ('BWL-011', 'D', 4), ('BWL-012', 'D', 4),
    ('BWL-021', 'D', 5), ('BWL-022', 'D', 5),

    -- Mahatma: one stack per position
    ('BWL-013', 'M', 1),
    ('BWL-014', 'M', 2),
    ('BWL-015', 'M', 3),
    ('BWL-016', 'M', 4),
    ('BWL-023', 'M', 5),

    -- Tiffin: one stack per position
    ('BWL-017', 'T', 1),
    ('BWL-018', 'T', 2),
    ('BWL-019', 'T', 3),
    ('BWL-020', 'T', 4),
    ('BWL-024', 'T', 5),

    -- Reserved / future. food_slot NULL: these are not installed anywhere, and a
    -- slot number would claim a serving position they do not occupy.
    ('BWL-025', 'R', null), ('BWL-026', 'R', null),
    ('BWL-027', 'R', null), ('BWL-028', 'R', null),
    ('BWL-029', 'R', null), ('BWL-030', 'R', null),
    ('BWL-031', 'R', null), ('BWL-032', 'R', null)
)
update public.devices d
   set location  = i.location,
       food_slot = i.food_slot::smallint,
       -- Label names the PHYSICAL position, never the dish. What is in slot 3
       -- changes with the meal, so a label saying "Rice" would be wrong by
       -- lunchtime and would compete with meal_food_mapping as a source of truth.
       label     = case i.location
                     when 'D' then 'Darshanarthi slot ' || i.food_slot
                     when 'M' then 'Mahatma slot '      || i.food_slot
                     when 'T' then 'Tiffin slot '       || i.food_slot
                     else          'Reserved'
                   end
  from intended i
 where d.device_id = i.device_id
   and (d.location  is distinct from i.location
     or d.food_slot is distinct from i.food_slot::smallint);

commit;

-- ---------------------------------------------------------------------
--  Verify: every device assigned, and the totals match the installation.
-- ---------------------------------------------------------------------
select coalesce(location, '--')                as location,
       coalesce(food_slot::text, '--')         as slot,
       count(*)                                as stacks,
       string_agg(device_id, ', ' order by device_id) as devices
  from public.devices
 group by location, food_slot
 order by location nulls last, food_slot nulls last;

select count(*) filter (where location in ('D','M','T'))              as deployed,
       count(*) filter (where location = 'R')                          as reserved,
       count(*) filter (where location is null)                        as unassigned,
       count(*)                                                        as total,
       count(distinct (location, food_slot))
         filter (where location in ('D','M','T'))                      as dish_positions
  from public.devices;
-- Expect: deployed 24, reserved 8, unassigned 0, total 32, dish_positions 15.
