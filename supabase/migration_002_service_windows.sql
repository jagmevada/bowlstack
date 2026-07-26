-- =====================================================================
--  Migration 002 -- service windows
--
--  Run AFTER schema.sql. Safe to re-run.
--
--  Devices are powered only during meal service and are dark the rest of the
--  day. Without this, device_overview.offline fires ~16 hours out of every 24
--  for every healthy unit -- and a genuinely dead device is then invisible
--  among 30 false alarms.
--
--  The device has no RTC and cannot know the time of day. The server does, so
--  the whole fix lives here: no firmware change, and the windows can be edited
--  per site without reflashing anything.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 1. Each installation keeps local time. Service windows are wall-clock
--    ("breakfast is 6am"), so they must be evaluated in the site's zone, not
--    the server's UTC.
-- ---------------------------------------------------------------------
alter table public.devices
  add column if not exists timezone text not null default 'Asia/Kolkata';

comment on column public.devices.timezone is
  'IANA zone for this installation, used to evaluate service windows in local '
  'wall-clock time. Change per site if the fleet spans zones.';

-- ---------------------------------------------------------------------
-- 2. Service windows. A NULL device_id is a fleet-wide default; a row naming a
--    device overrides the defaults for that device only, so one site can shift
--    its lunch without touching the rest.
-- ---------------------------------------------------------------------
create table if not exists public.service_windows (
  id         bigint generated always as identity primary key,
  device_id  text references public.devices(device_id) on delete cascade,
  label      text not null,
  starts_at  time not null,
  ends_at    time not null,
  constraint service_window_order check (ends_at > starts_at)
);

comment on table public.service_windows is
  'Hours a device is expected to be powered. Outside these, absence of reports '
  'is normal and must not raise an alarm. device_id NULL = applies to every '
  'device that has no rows of its own.';

create index if not exists service_windows_device_idx
  on public.service_windows (device_id);

-- Fleet defaults: breakfast, lunch, dinner.
insert into public.service_windows (device_id, label, starts_at, ends_at)
select v.device_id, v.label, v.starts_at, v.ends_at
from (values
  (null::text, 'breakfast', time '06:00', time '09:00'),
  (null::text, 'lunch',     time '11:30', time '14:00'),
  (null::text, 'dinner',    time '18:30', time '21:00')
) as v(device_id, label, starts_at, ends_at)
where not exists (
  select 1 from public.service_windows where device_id is null
);

-- ---------------------------------------------------------------------
-- 3. Is a device expected to be powered right now?
--
--    Device-specific rows win outright: if a device has any of its own, the
--    fleet defaults are ignored for it. A margin widens the window slightly at
--    both ends so a unit switched on a few minutes late is not flagged.
-- ---------------------------------------------------------------------
create or replace function public.in_service_window(
  at_ts     timestamptz,
  tz        text,
  dev_id    text          default null,
  margin    interval      default interval '10 minutes'
) returns boolean
language sql
stable
set search_path = ''
as $$
  with local_now as (
    select (at_ts at time zone coalesce(tz, 'UTC'))::time as t
  ),
  applicable as (
    select w.starts_at, w.ends_at
      from public.service_windows w
     where w.device_id = dev_id
    union all
    select w.starts_at, w.ends_at
      from public.service_windows w
     where w.device_id is null
       and not exists (select 1 from public.service_windows x
                        where x.device_id = dev_id)
  )
  select coalesce(bool_or(
           (select t from local_now) >= (a.starts_at - margin)
       and (select t from local_now) <= (a.ends_at   + margin)
         ), false)
    from applicable a;
$$;

-- ---------------------------------------------------------------------
-- 4. Rebuild the overview so "offline" means UNEXPECTEDLY offline.
--
--    Distinguishing the two is the point: `in_service` says whether the device
--    ought to be awake, and `offline` alarms only when it ought to be and is
--    not. `stale_for` is kept raw so you can still see exactly how long it has
--    been dark.
-- ---------------------------------------------------------------------
-- DROP first, not CREATE OR REPLACE: replacing a view can only APPEND columns
-- at the end, never insert or reorder them. Adding `timezone` after `location`
-- shifts every later column, which fails with
--   42P16: cannot change name of view column "updated_at" to "timezone"
drop view if exists public.device_overview;

create view public.device_overview
with (security_invoker = true) as
select d.device_id,
       d.label,
       d.location,
       d.timezone,
       s.updated_at,
       now() - s.updated_at as stale_for,

       public.in_service_window(now(), d.timezone, d.device_id) as in_service,

       -- Alarm only when the device should be reporting and is not.
       (public.in_service_window(now(), d.timezone, d.device_id)
        and (s.updated_at is null
             or s.updated_at < now() - interval '5 minutes')) as offline,

       -- Outside service hours the numbers below are the last known state from
       -- the previous service, not live data. Say so explicitly rather than
       -- letting a coordinator read a stale count as current.
       (not public.in_service_window(now(), d.timezone, d.device_id)
        and s.updated_at is not null) as data_is_stale,

       s.stack_count, s.stack_status, s.levels,
       s.sensors_online, s.battery_mv, s.battery_pct, s.charging,
       s.uptime_s, s.firmware, s.mac
from public.devices d
left join public.device_status s using (device_id);

revoke all on public.device_overview from anon, authenticated, public;
grant select on public.device_overview to authenticated;

-- ---------------------------------------------------------------------
-- 5. Privileges. Devices never read any of this.
-- ---------------------------------------------------------------------
alter table public.service_windows enable row level security;

drop policy if exists service_windows_select_staff on public.service_windows;
create policy service_windows_select_staff on public.service_windows
  for select to authenticated using (true);

revoke all on public.service_windows from anon, authenticated, public;
grant select on public.service_windows to authenticated;

revoke all on function public.in_service_window(timestamptz, text, text, interval)
  from public, anon;
grant execute on function public.in_service_window(timestamptz, text, text, interval)
  to authenticated;

commit;

-- ---------------------------------------------------------------------
--  Checks
--
--    -- who is genuinely down right now (should normally be empty)
--    select device_id, label, stale_for from public.device_overview
--     where offline;
--
--    -- current expectation per device
--    select device_id, timezone, in_service, data_is_stale, stack_count
--      from public.device_overview order by device_id;
--
--    -- sanity: walk a day and see the windows open and close
--    select h::time,
--           public.in_service_window(
--             (current_date + h) at time zone 'Asia/Kolkata', 'Asia/Kolkata')
--      from generate_series(interval '0h', interval '23h', interval '1h') h;
--
--  Per-site override, e.g. one kitchen serving dinner later:
--    insert into public.service_windows (device_id, label, starts_at, ends_at)
--    values ('BWL-007','breakfast',time '06:00',time '09:00'),
--           ('BWL-007','lunch',    time '11:30',time '14:00'),
--           ('BWL-007','dinner',   time '19:00',time '22:00');
--  (Adding ANY row for a device replaces the fleet defaults for it entirely.)
-- ---------------------------------------------------------------------
