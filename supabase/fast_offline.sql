-- =====================================================================
--  ONE-TIME: make offline detection fast, without losing any data
--
--  Run ONCE, as owner, in the Supabase SQL editor. Then delete this file.
--
--  NON-DESTRUCTIVE. It replaces one function and two VIEWS. Views hold no rows,
--  so no table is touched, no telemetry is lost and no device has to be
--  re-registered. This is the whole point of it existing: schema.sql would
--  achieve the same end state by dropping and rebuilding every table.
--
--  WHAT IT CHANGES
--    offline detection   5 minutes  ->  ~40-60 s
--
--  The threshold moves out of the view bodies -- where it was written twice,
--  and could drift between device_overview and slot_overview -- and into
--  public.offline_after(). After this, retuning is one CREATE OR REPLACE
--  FUNCTION and nothing else needs rebuilding.
--
--  WHY 40 SECONDS
--      detection (worst case) = threshold + front-end poll interval
--      threshold              > firmware heartbeat + one retry backoff
--
--    firmware heartbeat   20 s   (STATUS_PERIOD_MS, src/telemetry.cpp)
--    retry backoff        15 s   (RETRY_PERIOD_MS)
--    dashboard poll       20 s   (POLL_MS, web/js/app.js)
--
--    -> a HEALTHY unit that loses one post is 35 s stale, so 40 s leaves slack
--    -> detection is 40 s best case, 60 s worst
--
--  The lower bound is not negotiable: a threshold below the gap a healthy device
--  leaves between posts makes every device alarm between its own heartbeats.
--  Below ~40 s an ordinary WiFi reconnection starts reading as an outage.
--
--  REQUIRES the 20 s firmware heartbeat. On the older 60 s build a 40 s
--  threshold WILL flap -- flash the firmware first, or set 90 s below.
--
--  The view bodies here are copied verbatim from schema.sql, so a fresh install
--  from that file lands in exactly this state and the two cannot disagree.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
--  1. The threshold, as a function so it is stated once.
-- ---------------------------------------------------------------------
create or replace function public.offline_after()
returns interval language sql immutable parallel safe as $$
  select interval '40 seconds'
$$;

revoke all on function public.offline_after() from public, anon;
grant execute on function public.offline_after() to authenticated;

-- ---------------------------------------------------------------------
--  2. The two views that read it.
--
--  CREATE OR REPLACE, not DROP: replacing preserves the existing grants and
--  cannot cascade into anything. The column list, order and types are unchanged
--  -- only the offline expression differs -- which is what makes REPLACE legal
--  here at all.
--
--  security_invoker is restated deliberately. Omitting it would leave the view
--  running with owner rights and silently bypassing every RLS policy, which is
--  a security regression that produces no error.
-- ---------------------------------------------------------------------
create or replace view public.device_overview
with (security_invoker = true) as
select d.device_id,
       d.location,
       d.food_slot,
       d.label,
       d.timezone,

       -- What this device's slot is serving RIGHT NOW. NULL outside service
       -- hours, or when nobody has entered a mapping -- both mean "no current
       -- dish", which is not the same as a dish with no name.
       m.food_name                          as current_food,
       public.current_meal_type(d.timezone) as current_meal,

       s.reported,
       s.updated_at,
       now() - s.updated_at as stale_for,
       public.in_service_window(now(), d.timezone, d.device_id) as in_service,

       -- Alarm only when the device SHOULD be reporting and is not. Devices are
       -- dark ~16h/day by design, so plain staleness is not a fault.
       --
       -- s.reported gates this so a device that has NEVER reported does not
       -- alarm: that is a registered-but-not-yet-deployed unit, not a failure.
       -- Without it, pre-registering the fleet would light up every unbuilt
       -- station as offline and bury the ones that genuinely went down.
       (coalesce(s.reported, false)
        and public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at < now() - public.offline_after()) as offline,

       -- Registered but never heard from -- i.e. awaiting installation.
       (not coalesce(s.reported, false)) as awaiting_deployment,

       -- Outside service hours these numbers are the last known state from the
       -- previous service, not live data. Say so rather than letting a
       -- coordinator read a stale count as current.
       (not public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at is not null) as data_is_stale,

       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_level, s.charging,
       s.uptime_s, s.firmware, s.mac
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone);

create or replace view public.slot_overview
with (security_invoker = true) as
select d.location,
       d.food_slot,
       max(m.food_name)                                        as current_food,
       public.current_meal_type(max(d.timezone))               as current_meal,
       count(*)                                                as devices,
       count(*) filter (where coalesce(s.reported, false))     as devices_reported,

       -- Ceiling for this slot, so a progress bar need not hardcode one. If a
       -- fourth stack joins Darshanarthi slot 1 the achievable total rises, and a
       -- UI with "max 12" baked in would misreport the busiest position in the
       -- hall.
       (count(*) * 4)::bigint                                  as bowls_capacity,

       -- The trustworthy total. NULL, not 0, when no device reported: no data is
       -- not an empty counter, and the two send staff to do opposite things.
       sum(s.stack_count) filter (where s.stack_status = 'ok') as bowls_trusted,
       sum(s.stack_count)                                      as bowls_reported,

       bool_or(s.stack_status = 'discontiguous')               as any_fault,
       bool_or(s.stack_status = 'degraded')                    as any_degraded,
       bool_or(s.battery_level in ('low','critical'))          as any_battery_warn,
       bool_or(coalesce(s.reported, false)
               and public.in_service_window(now(), d.timezone, d.device_id)
               and s.updated_at < now() - public.offline_after()) as any_offline,
       min(s.updated_at)                                       as oldest_update
  from public.devices d
  left join public.device_status s using (device_id)
  left join public.meal_food_mapping m
         on m.location  = d.location
        and m.food_slot = d.food_slot
        and m.meal_date = public.current_meal_date(d.timezone)
        and m.meal_type = public.current_meal_type(d.timezone)
 where d.location is not null
   and d.food_slot is not null
 group by d.location, d.food_slot;

commit;

-- ---------------------------------------------------------------------
--  Verify
-- ---------------------------------------------------------------------

-- Both must say "wired".
select v.viewname,
       case when pg_get_viewdef(('public.' || v.viewname)::regclass)
                 like '%offline_after%'
            then 'wired; retuning offline_after() now takes effect'
            else '<<< STILL HARDCODED - the replace did not apply'
       end as wiring,
       case when c.reloptions @> array['security_invoker=true']
            then 'security_invoker'
            else '<<< OWNER RIGHTS - bypasses RLS, re-run schema.sql'
       end as security
  from (values ('device_overview'), ('slot_overview')) as v(viewname)
  join pg_class c on c.relname = v.viewname
  join pg_namespace n on n.oid = c.relnamespace and n.nspname = 'public';

select public.offline_after() as threshold_now;

-- Nothing should have moved except how quickly silence is noticed.
select count(*) filter (where reported)             as reporting,
       count(*) filter (where offline)              as offline_now,
       count(*) filter (where awaiting_deployment)  as awaiting,
       count(*) filter (where data_is_stale)        as stale
  from public.device_overview;

-- `offline` is only ever true INSIDE a service window. Outside one it is false
-- for every device by design, so run this during breakfast, lunch or dinner if
-- you want to watch the new threshold actually fire.
select public.current_meal_type('Asia/Kolkata') as meal_now,
       public.in_service_window(now(), 'Asia/Kolkata') as in_service_now;
