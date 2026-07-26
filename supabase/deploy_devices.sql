-- =====================================================================
--  Deploy 15 devices to serving positions -- for FRONT-END DEVELOPMENT
--
--  Run once as owner, AFTER register_devices.sql. Idempotent.
--
--  WHY THIS IS SEPARATE FROM register_devices.sql
--  ----------------------------------------------
--  Registering an installation and deploying it are different events, weeks
--  apart in practice. register_devices.sql leaves area and item_slot NULL
--  because a unit has no serving position until someone physically bolts it to
--  a station, and guessing would put 32 devices at positions nobody has built.
--
--  But the front-end cannot be built against 32 undeployed rows: the stock view
--  groups by area and orders by item_slot, so with both NULL there is nothing to
--  group and nothing to order. This assigns a realistic deployment so that
--  screen has something to draw.
--
--  THE FLEET IS DELIBERATELY LARGER THAN THE DEPLOYMENT
--  ----------------------------------------------------
--  3 areas x 5 slots = 15 positions, against 32 registered units. The other 17
--  are spares and rotation stock, and they stay NULL -- which is not a mistake
--  to be tidied up. `awaiting_deployment` in device_overview exists precisely
--  for them, and the front-end must show them differently from a deployed unit
--  that has gone quiet. If every row were deployed, that distinction would be
--  untestable and the UI would get it wrong in the field.
--
--  This is development scaffolding. Before real deployment, assign positions
--  from the front-end configuration page instead -- that path is what the
--  kitchen staff will actually use, and it deserves to be exercised.
-- =====================================================================

begin;

-- BWL-001..005 -> D, BWL-006..010 -> T, BWL-011..015 -> M.
-- (area, item_slot) is UNIQUE, so this mapping must stay a bijection; the
-- arithmetic below guarantees it rather than trusting a hand-written list.
update public.devices d
   set area      = v.area,
       item_slot = v.item_slot,
       label     = v.label
  from (
    select 'BWL-' || lpad(n::text, 3, '0')            as device_id,
           (array['D','T','M'])[((n - 1) / 5) + 1]    as area,
           (((n - 1) % 5) + 1)::smallint              as item_slot,
           -- Free text, and only a placeholder. What food sits in a slot
           -- changes with the meal, so a label naming a dish would be wrong by
           -- lunchtime -- see docs/frontend.md on the slot -> food mapping.
           (array['Darshanarthi','Tiffin','Mahtma'])[((n - 1) / 5) + 1]
             || ' slot ' || (((n - 1) % 5) + 1)       as label
      from generate_series(1, 15) as n
  ) v
 where d.device_id = v.device_id;

commit;

-- ---------------------------------------------------------------------
--  Result: the deployment grid, plus the spares left undeployed.
-- ---------------------------------------------------------------------
select coalesce(area, '--')                        as area,
       coalesce(item_slot::text, '--')             as slot,
       device_id,
       coalesce(label, '(spare)')                  as label
  from public.devices
 order by area nulls last, item_slot, device_id;

select count(*) filter (where area is not null)    as deployed,
       count(*) filter (where area is null)        as spare,
       count(*)                                    as total
  from public.devices;
