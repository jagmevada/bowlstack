-- =====================================================================
--  Register the fleet -- BWL-001 .. BWL-032
--
--  Run once as owner. Idempotent: re-running adds only what is missing and
--  never disturbs a device already in service.
--
--  You register INSTALLATIONS, not boards. A replacement ESP32 flashed with the
--  same BOWLSTACK_DEVICE_ID needs no new row -- it writes to the existing one
--  and the `mac` column updates itself. So this runs once, ever, however many
--  boards get swapped.
--
--  BWL-000 is deliberately ABSENT and must stay that way. It is the firmware
--  default in include/version.h, so leaving it unregistered means a unit
--  flashed without -DBOWLSTACK_DEVICE_ID is rejected with 23503 instead of
--  quietly writing into a real installation's row.
-- =====================================================================

begin;

-- Registering a device fires devices_create_status, so each row here also
-- provisions its device_status row -- which is what lets the firmware use a
-- plain UPDATE instead of an upsert.
-- area, item_slot and label are left NULL on purpose. A unit has no serving
-- position until it is physically installed, and the front-end configuration
-- page is what assigns it -- guessing here would put 32 devices at positions
-- nobody has built yet.
insert into public.devices (device_id, area, item_slot, label, location, timezone)
select 'BWL-' || lpad(n::text, 3, '0'),
       null,              -- 'D' Darshanarthi | 'T' Tiffin | 'M' Mahtma
       null,              -- 1-5, the physical slot label on the station
       null,
       null,
       'Asia/Kolkata'     -- change per site if the fleet ever spans zones
  from generate_series(1, 32) as n
 on conflict (device_id) do nothing;

commit;

-- ---------------------------------------------------------------------
--  Result: one row per device, plus a summary. Single result set, because
--  the Supabase SQL editor only displays the last statement's output.
-- ---------------------------------------------------------------------
select o.device_id,
       coalesce(o.area, '-') || coalesce(o.item_slot::text, '-') as position,
       coalesce(o.label, '(unnamed)')                            as label,
       case when o.awaiting_deployment then 'awaiting deployment'
            when o.offline              then 'OFFLINE'
            else 'reporting' end                                 as state,
       o.stack_count
  from public.device_overview o
union all
select 'TOTAL',
       count(*) filter (where area is not null)::text || ' placed',
       count(*)::text || ' registered',
       count(*) filter (where awaiting_deployment)::text || ' awaiting',
       null
  from public.device_overview
 order by 1;

-- ---------------------------------------------------------------------
--  Naming a unit as you install it
-- ---------------------------------------------------------------------
-- update public.devices
--    set label    = 'Kitchen A counter 1',
--        location = 'Kitchen A'
--  where device_id = 'BWL-001';
--
--  Several at once:
--
-- update public.devices d
--    set label = v.label, location = v.location
--   from (values ('BWL-001','Kitchen A counter 1','Kitchen A'),
--                ('BWL-002','Kitchen A counter 2','Kitchen A'),
--                ('BWL-003','Kitchen B counter 1','Kitchen B'))
--        as v(device_id, label, location)
--  where d.device_id = v.device_id;
--
-- ---------------------------------------------------------------------
--  Per-site meal times, if one kitchen differs from the fleet defaults.
--  Adding ANY row for a device replaces the defaults for that device
--  entirely, so list all three meals.
-- ---------------------------------------------------------------------
-- insert into public.service_windows (device_id, label, starts_at, ends_at)
-- values ('BWL-007','breakfast',time '06:00',time '09:00'),
--        ('BWL-007','lunch',    time '11:30',time '14:00'),
--        ('BWL-007','dinner',   time '19:00',time '22:00');
--
-- ---------------------------------------------------------------------
--  Extending the fleet later: change 32 to the new total and re-run this
--  file. Existing rows are untouched by the ON CONFLICT DO NOTHING.
-- ---------------------------------------------------------------------
