-- =====================================================================
--  Bowlstack -- TRIAL HARNESS: the manual fill estimate.
--
--  ##  TEMPORARY. ONE DEVICE. NOT PRODUCTION.  ##
--
--  Rollback: supabase/rollback_manual_fill.sql. Everything this file adds is
--  either a NULLABLE column or a NEW table, so removing it cannot disturb a
--  row that existed before it ran.
--
--  ---------------------------------------------------------------------
--  WHAT IT IS FOR
--  ---------------------------------------------------------------------
--  LDC-001 has a rotary knob fitted beside its load cell. The counter
--  attendant looks at the vessel, judges how full it is, and dials the
--  figure in. The load cell underneath keeps weighing the same food,
--  independently and continuously.
--
--  The experiment is the GAP between those two numbers. A knob and a person
--  cost almost nothing next to three bridge converters and a mux; the
--  question a reviewer is asking is whether the cheap version is accurate
--  enough to deploy, and that can only be answered by recording both against
--  the same vessel at the same instants and subtracting.
--
--  ---------------------------------------------------------------------
--  WHY IT TOUCHES NOTHING THAT MEASURES
--  ---------------------------------------------------------------------
--  The manual estimate is stored BESIDE the measured weight and is read by
--  nothing except the comparison view at the end of this file. It does not
--  appear in slot_quantity, it does not appear in slot_burn_rate, and it
--  does not appear in any figure the kitchen acts on.
--
--  That is not tidiness, it is the design of the experiment. The subject
--  under test must not be able to move its own control -- if the manual
--  figure fed the slot totals, then a good agreement between "what the
--  dashboard says" and "what the attendant dialled in" would prove only that
--  the attendant's number had been copied into both columns.
-- =====================================================================

begin;

-- --- 1. prerequisites -------------------------------------------------
-- This sits on top of the load-cell migration; it has nothing to add to a
-- database that has no scales in it.
do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'device_status'
                    and column_name = 'weight_state') then
    raise exception
      'run migrate_loadcell.sql first -- device_status has no weight columns';
  end if;
  if to_regclass('public.weight_samples') is null then
    raise exception
      'run migrate_weight_samples.sql first -- there is no history to append to';
  end if;
end $$;

-- --- 2. the estimate, on current state and on history -----------------
-- NULLABLE AND NULL BY DEFAULT, on every device. Thirty-one load cells and
-- thirty-two bowl counters have no knob, and `0` would say their vessels are
-- empty. The absence of an estimate is not an estimate of zero -- the same
-- rule that keeps weight_g NULL until a scale is calibrated.
alter table public.device_status
  add column if not exists manual_fill_pct smallint,
  add column if not exists manual_fill_age_s integer;

alter table public.weight_samples
  add column if not exists manual_fill_pct smallint;

do $$
begin
  if not exists (select 1 from pg_constraint
                  where conname = 'device_status_manual_fill_ck') then
    alter table public.device_status add constraint device_status_manual_fill_ck
      check (manual_fill_pct is null or manual_fill_pct between 0 and 100);
  end if;
  if not exists (select 1 from pg_constraint
                  where conname = 'weight_samples_manual_fill_ck') then
    alter table public.weight_samples add constraint weight_samples_manual_fill_ck
      check (manual_fill_pct is null or manual_fill_pct between 0 and 100);
  end if;
end $$;

comment on column public.device_status.manual_fill_pct is
  'TRIAL HARNESS. Vessel fullness as judged by eye and dialled in on the knob, '
  '0-100. NULL on every device without one. Never feeds slot_quantity or '
  'slot_burn_rate -- it is the subject of a comparison against weight_g, not an '
  'input to anything the kitchen acts on.';

comment on column public.device_status.manual_fill_age_s is
  'TRIAL HARNESS. Seconds since the attendant last touched the knob. The '
  'staleness IS a result: an estimate nobody refreshes is the failure mode a '
  'knob-based system actually has, so it is recorded rather than hidden.';

-- --- 3. the device may write them -------------------------------------
grant update (manual_fill_pct, manual_fill_age_s)
  on public.device_status to anon;
grant insert (manual_fill_pct)
  on public.weight_samples to anon;

-- --- 4. the vessel capacity -------------------------------------------
-- ITS OWN TABLE, deliberately, rather than a column on meal_food_mapping.
--
-- The obvious place is beside bowl_weight_g, and that is exactly why it is not
-- there: meal_food_mapping is production, it is edited by kitchen staff every
-- day, and it is keyed per location and slot. This is one number for one
-- prototype vessel that will be gone in a few weeks. Widening a live table for
-- a temporary experiment leaves a column nobody dares drop.
--
-- SERVER-SIDE rather than in the browser, because the reviewer, the kitchen and
-- whoever is collating results will each open the dashboard somewhere else and
-- must see the same figure. A number in local storage is a different experiment
-- per browser.
create table if not exists public.trial_vessel_capacity (
  meal_type   text        primary key
              check (meal_type in ('Breakfast','Lunch','Dinner')),
  capacity_g  integer     not null check (capacity_g between 100 and 200000),
  note        text,
  updated_at  timestamptz not null default now()
);

comment on table public.trial_vessel_capacity is
  'TRIAL HARNESS. Full-vessel mass per meal, so a manual percentage can be '
  'rendered in kilograms: 50% of an 18 kg vessel is 9 kg. Separate from '
  'meal_food_mapping on purpose -- that table is production and this is a '
  'prototype that will be deleted.';

alter table public.trial_vessel_capacity enable row level security;

revoke all on public.trial_vessel_capacity from anon, authenticated, public;

-- Staff read and write it; devices never see it. The conversion to kilograms
-- happens on the dashboard, not on the board -- a station that knew its vessel
-- capacity could report kilograms it had not measured, which is the one thing
-- the honesty rule exists to prevent.
drop policy if exists trial_capacity_staff on public.trial_vessel_capacity;
create policy trial_capacity_staff on public.trial_vessel_capacity
  for all to authenticated using (true) with check (true);

grant select, insert, update, delete on public.trial_vessel_capacity to authenticated;

-- --- 4b. what the dashboard reads -------------------------------------
-- ITS OWN VIEW RATHER THAN A WIDER device_overview, and the reason is drift.
-- device_overview is defined in THREE files -- schema.sql, migrate_loadcell.sql
-- and weekly_menu_and_offline.sql -- because a live database cannot be rebuilt
-- and each path has to arrive at the same view. Appending a trial column means
-- editing all three and keeping them in step, and the last time two of those
-- copies disagreed the result was a file that could not run at all.
--
-- A separate view costs nothing, joins in one line on the dashboard, and is
-- deleted by the rollback along with everything else.
create or replace view public.trial_manual_fill as
select d.device_id,
       d.location,
       d.food_slot,
       s.manual_fill_pct,
       s.manual_fill_age_s,
       -- The measured figure travels WITH it, so a screen showing both cannot
       -- pair an estimate against a weight from a different poll.
       s.weight_state,
       s.weight_g,
       s.updated_at
  from public.devices d
  join public.device_status s using (device_id)
 where d.kind = 'scale';

comment on view public.trial_manual_fill is
  'TRIAL HARNESS. Current manual estimate beside the current measured weight, '
  'per scale. Separate from device_overview so the trial can be dropped without '
  'touching a view that three migration paths all have to agree about.';

revoke all on public.trial_manual_fill from anon, authenticated, public;
grant select on public.trial_manual_fill to authenticated;

-- --- 5. the comparison ------------------------------------------------
-- ONE ROW PER SAMPLE THAT HAS BOTH NUMBERS, which is the only shape an error
-- analysis can use. Rows with one or the other are excluded rather than
-- coalesced: a missing manual estimate is not 0%, and pairing a measurement
-- against an assumption would manufacture agreement.
create or replace view public.trial_fill_vs_weight as
select s.device_id,
       s.recorded_at,
       d.location,
       d.food_slot,
       s.manual_fill_pct,
       s.weight_g                                        as measured_g,
       c.capacity_g,
       -- What the attendant's estimate implies, in the same unit as the scale.
       (s.manual_fill_pct::numeric / 100) * c.capacity_g  as manual_g,
       -- THE NUMBER THE WHOLE EXERCISE EXISTS TO PRODUCE. Signed, so a
       -- systematic lean -- people rounding up because a full-looking vessel is
       -- the safe answer -- is visible as a bias rather than averaged away by
       -- an absolute value.
       (s.manual_fill_pct::numeric / 100) * c.capacity_g - s.weight_g
                                                          as error_g,
       case when s.weight_g > 0
            then round((((s.manual_fill_pct::numeric / 100) * c.capacity_g
                         - s.weight_g) / s.weight_g) * 100, 1)
       end                                                as error_pct
  from public.weight_samples s
  join public.devices d on d.device_id = s.device_id
  left join public.trial_vessel_capacity c
         on c.meal_type = public.current_meal_type(d.timezone)
 where s.manual_fill_pct is not null
   and s.weight_state = 'ok'
   and s.weight_g is not null
 order by s.recorded_at desc;

comment on view public.trial_fill_vs_weight is
  'TRIAL HARNESS. Manual estimate against measured weight, per sample, with the '
  'signed error. Only rows carrying BOTH figures appear -- a missing estimate is '
  'not 0%, and pairing a measurement with an assumption would manufacture '
  'agreement.';

revoke all on public.trial_fill_vs_weight from anon, authenticated, public;
grant select on public.trial_fill_vs_weight to authenticated;

commit;

-- --- verification ------------------------------------------------------
select 'device_status.manual_fill_pct' as what,
       count(*) filter (where column_name = 'manual_fill_pct') as present
  from information_schema.columns
 where table_schema = 'public' and table_name = 'device_status'
union all
select 'weight_samples.manual_fill_pct',
       count(*) filter (where column_name = 'manual_fill_pct')
  from information_schema.columns
 where table_schema = 'public' and table_name = 'weight_samples'
union all
select 'trial_vessel_capacity',
       (to_regclass('public.trial_vessel_capacity') is not null)::int
union all
select 'trial_fill_vs_weight',
       (to_regclass('public.trial_fill_vs_weight') is not null)::int;
-- Expect 1 on every row.
