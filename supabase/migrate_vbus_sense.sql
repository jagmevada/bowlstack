-- =====================================================================
--  Bowlstack -- publish VBUS presence, so the dashboard can say "on mains".
--
--  Rollback: supabase/rollback_vbus_sense.sql. One nullable column and one
--  small view; removing it cannot disturb a row that existed before.
--
--  ---------------------------------------------------------------------
--  WHY THIS IS NOT `charging`
--  ---------------------------------------------------------------------
--  Two different facts, known to different degrees, and the schema already
--  had only the one the hardware cannot read:
--
--    charging        the ETA6098 pushing current into the cell, from its STAT
--                    pin -- which reaches no GPIO on an unmodified board, so
--                    it is genuinely unknown and stays NULL.
--    external_power  5 V present, measured through a 5k/10k divider on IO10.
--                    Known on every board that has the divider fitted.
--
--  They diverge exactly when the cell fills: the charger terminates, STAT
--  releases, and 5 V is still there. So a dashboard that reported VBUS as
--  `charging` would claim a charge that finished hours ago -- and one that
--  reported only `charging` told a technician looking at a plugged-in
--  station that nothing was knowable, while the device could see the mains
--  perfectly well. That second failure is what this fixes.
--
--  NULL still means unreadable, on the thirty-two bowl counters and on any
--  scale without the divider. It does not mean "on battery".
--
--  ---------------------------------------------------------------------
--  WHY A SEPARATE VIEW RATHER THAN A WIDER device_overview
--  ---------------------------------------------------------------------
--  device_overview is defined in THREE files -- schema.sql,
--  migrate_loadcell.sql and weekly_menu_and_offline.sql -- because a live
--  database cannot be rebuilt and every path has to arrive at the same
--  view. Appending a column means editing all three and keeping them in
--  step, and the last time two of those copies disagreed the result was a
--  file that could not run at all.
--
--  device_power costs one join on the dashboard. Folding it into
--  device_overview is the right long-term home and is a deliberate
--  follow-up, not an oversight.
-- =====================================================================

begin;

do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'device_status'
                    and column_name = 'weight_state') then
    raise exception
      'run migrate_loadcell.sql first -- device_status has no weight columns';
  end if;
end $$;

alter table public.device_status
  add column if not exists external_power boolean;

comment on column public.device_status.external_power is
  'Mains present, from the VBUS divider. NOT `charging`: the charger stops when '
  'the cell is full and 5 V remains, so this stays true where charging goes '
  'false. NULL means unreadable -- no divider fitted -- and never "on battery".';

grant update (external_power) on public.device_status to anon;

-- What the dashboard reads. Every device, not only scales: a bowl counter with
-- the divider fitted would report it too, and one without simply carries NULL.
-- security_invoker, as schema.sql requires of every view here. It was omitted
-- when this went in; nothing leaks without it, because the view is granted to
-- `authenticated` only, but a view running with the owner's rights evaluates
-- RLS as the owner and the next grant added would quietly bypass a policy.
create or replace view public.device_power
  with (security_invoker = true) as
select d.device_id,
       d.kind,
       s.external_power,
       s.charging,
       s.battery_mv,
       s.battery_level,
       s.updated_at
  from public.devices d
  join public.device_status s using (device_id);

comment on view public.device_power is
  'Power state per device -- mains presence beside charge state, which are '
  'different facts. Separate from device_overview only because that view is '
  'defined in three files that must agree; folding this in is a follow-up.';

revoke all on public.device_power from anon, authenticated, public;
grant select on public.device_power to authenticated;

commit;

select 'device_status.external_power' as what,
       count(*) filter (where column_name = 'external_power') as present
  from information_schema.columns
 where table_schema = 'public' and table_name = 'device_status'
union all
select 'device_power view',
       (to_regclass('public.device_power') is not null)::int;
-- Expect 1 on both.
