-- =====================================================================
--  DEBUG ONLY -- move the service windows so the dashboard can be
--  exercised outside real service hours.
--
--  ##  THIS FILE CHANGES LIVE BEHAVIOUR AND MUST BE UNDONE.  ##
--
--  Replaces the earlier debug_extend_dinner.sql, which widened dinner to
--  23:59 and then stopped being enough once the clock passed midnight.
--
--  The service windows are not decoration. They gate:
--
--    in_service_window()      whether a silent device is a FAULT or is
--                             simply asleep -- devices are dark ~16 h a day
--    device_overview.offline  which is gated on the above
--    missed_last_service      "slept through the last completed window"
--    current_meal_type()      which meal's menu the dashboard resolves
--    last_served_meal()       what food is on the counters right now
--    slot_burn_rate           the poll drops to a 10-minute idle cadence
--                             outside service, so the curve stops moving
--
--  So a moved window does not merely let you see the screen: it tells the
--  whole fleet it is supposed to be awake. Every deployed unit that is
--  powered down for the night reads `offline` on the Health tab, in red,
--  for as long as this is in place.
--
--  THE REAL VALUES, recorded here because this has bitten before -- a debug
--  window (dinner preponed to 16:30) was once left in place and had to be
--  tracked down later:
--
--      breakfast   06:30:00 - 09:30:00      <-- this file moves the START
--      lunch       11:30:00 - 14:30:00
--      dinner      18:30:00 - 21:30:00      <-- and restores this END
--
--  Read from the live database on 2026-08-29 at 22:52 IST, before any of
--  this was touched.
-- =====================================================================

begin;

-- --- 1. RESTORE dinner ---------------------------------------------------
-- Put back the 21:30 close that debug_extend_dinner.sql pushed to 23:59.
-- Doing it here rather than in a separate step means the database is never
-- left with two widened windows at once, which would make "is this device
-- legitimately asleep" unanswerable across most of the day.
update public.service_windows
   set ends_at = time '21:30:00'
 where device_id is null
   and label = 'dinner';

-- --- 2. MOVE breakfast's start to 00:10 ----------------------------------
-- The END stays at 09:30. Only the start moves, for the same reason the
-- dinner edit only moved the end: last_served_meal() resolves the most
-- recently STARTED window, so moving a start changes which meal the whole
-- dashboard thinks is on the counters. Moving both ends of both windows at
-- once would make that ambiguous.
--
-- 00:10 is chosen against the clock rather than for any operational reason:
-- it is a few minutes from now, so the window opens while somebody is
-- watching it rather than at some point overnight.
update public.service_windows
   set starts_at = time '00:10:00'
 where device_id is null
   and label = 'breakfast';

commit;

select label, starts_at, ends_at,
       (now() at time zone 'Asia/Kolkata')::time(0)        as ist_now,
       public.current_meal_type('Asia/Kolkata')            as current_meal,
       public.in_service_window(now(),'Asia/Kolkata',null) as in_service
  from public.service_windows
 where device_id is null
 order by starts_at;
-- Expect: breakfast 00:10-09:30, lunch 11:30-14:30, dinner 18:30-21:30.
-- current_meal reads Breakfast and in_service reads t from 00:10 onward.


-- ---------------------------------------------------------------------
--  RESTORE -- run this when you are done. Uncomment and execute.
--
--  Do it before the fleet's real breakfast: between 00:10 and 06:30 every
--  powered-down station reads offline, which is the alarm the service
--  windows exist to suppress.
-- ---------------------------------------------------------------------

-- update public.service_windows
--    set starts_at = time '06:30:00'
--  where device_id is null
--    and label = 'breakfast';
--
-- select label, starts_at, ends_at from public.service_windows
--  where device_id is null order by starts_at;
-- -- Expect: breakfast 06:30-09:30, lunch 11:30-14:30, dinner 18:30-21:30
