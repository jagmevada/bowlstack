-- =====================================================================
--  What is installed on THIS database, and what to run next.
--
--  READ-ONLY. Writes nothing, locks nothing, safe to run mid-service and
--  safe to run on a database you know nothing about. Paste the whole file
--  into the Supabase SQL editor.
--
--  It exists because the migrations have an ORDER, and the failure mode of
--  getting it wrong is a message that names a symptom rather than a cause --
--  `function public.last_served_meal(text) does not exist` is what you get
--  for running the load-cell migration on a database that never had the
--  weight one. Both files now refuse up front instead, but the useful thing
--  is to know before you start.
--
--  The dashboard's own "Master needs one migration" banner answers a narrower
--  question: it checks for the slot_quantity VIEW alone, because that is all
--  the Master tab needs to render. It is right about that and says nothing
--  about the rest.
-- =====================================================================

with present as (
  select
    -- Base schema.
    (select count(*) from pg_tables
      where schemaname='public' and tablename='devices')            > 0 as t_devices,
    (select count(*) from pg_tables
      where schemaname='public' and tablename='device_status')      > 0 as t_status,
    -- weekly_menu_and_offline.sql
    (select count(*) from pg_tables
      where schemaname='public' and tablename='meal_menu_template') > 0 as t_template,
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='device_overview'
        and column_name='missed_last_service')                      > 0 as c_missed,
    -- migrate_bowl_weight.sql
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='meal_food_mapping'
        and column_name='bowl_weight_g')                            > 0 as c_bowlwt,
    (select count(*) from pg_views
      where schemaname='public' and viewname='slot_quantity')       > 0 as v_quantity,
    -- migrate_loadcell.sql
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='devices'
        and column_name='kind')                                     > 0 as c_kind,
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='device_status'
        and column_name='weight_g')                                 > 0 as c_weight,
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='slot_quantity'
        and column_name='measured_weight_g')                        > 0 as c_measured,
    -- THE FOUR THIS FILE USED TO STOP SHORT OF. It is named by CLAUDE.md,
    -- docs/supabase.md and schema.sql as the authority on what a database has,
    -- and it checked six steps out of ten -- so it reported a COMPLETE database
    -- that answers 400 to every scale PATCH. The last two matter most: the
    -- firmware puts manual_fill_pct and external_power in bodies it sends
    -- unconditionally, and a missing column there is not a degraded feature,
    -- it is the whole device going silent.
    --
    -- No gating needed, unlike the `scales` CTE below: information_schema
    -- returns zero rows for an absent table rather than raising, and
    -- to_regclass returns NULL rather than erroring on an unknown name.
    to_regclass('public.weight_samples')          is not null as t_samples,
    to_regclass('public.slot_burn_rate')          is not null as t_burn,
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='device_status'
        and column_name='manual_fill_pct')                          > 0 as c_fill,
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='device_status'
        and column_name='external_power')                           > 0 as c_extpwr,
    -- Registration.
    (select count(*) from public.devices where device_id like 'BWL-%') as n_stacks
),
-- Counted separately: `devices.kind` may not exist yet, and referencing a
-- column that is absent is a parse error rather than a NULL -- so the query
-- that reads it has to be gated on the query that finds it.
scales as (
  select case when (select c_kind from present)
              then (select count(*) from public.devices
                     where device_id like 'LDC-%')
         end as n_scales
)
select item as step, name as run_this, status, note from (
  values
    (1, 'schema.sql (base tables)',
        case when (select t_devices and t_status from present) then 'installed'
             else 'MISSING' end,
        'devices, device_status, status_events, service_windows'),
    (2, 'weekly_menu_and_offline.sql',
        case when (select t_template and c_missed from present) then 'installed'
             when (select t_template or c_missed from present) then 'PARTIAL'
             else 'not run' end,
        'weekly menu template + missed_last_service'),
    (3, 'migrate_bowl_weight.sql',
        case when (select c_bowlwt and v_quantity from present) then 'installed'
             when (select c_bowlwt or v_quantity from present) then 'PARTIAL'
             else 'not run -- Master says "needs one migration"' end,
        'bowl_weight_g + the slot_quantity view'),
    (4, 'migrate_loadcell.sql',
        case when (select c_kind and c_weight and c_measured from present) then 'installed'
             when (select c_kind or c_weight from present) then 'PARTIAL'
             else 'not run -- no measured weight anywhere' end,
        'devices.kind + weight columns + the precedence rule'),
    (5, 'register_devices.sql',
        case when (select n_stacks from present) >= 32 then 'installed'
             when (select n_stacks from present) > 0 then 'PARTIAL'
             else 'not run' end,
        'BWL-001..032: ' || (select n_stacks from present)::text || ' registered'),
    (6, 'register_loadcells.sql',
        case when (select n_scales from scales) is null then 'blocked by 4'
             when (select n_scales from scales) >= 32 then 'installed'
             when (select n_scales from scales) > 0 then 'PARTIAL'
             else 'not run' end,
        coalesce('LDC-001..032: ' || (select n_scales from scales)::text
                 || ' registered', 'needs devices.kind first')),
    (7, 'migrate_weight_samples.sql',
        case when (select t_samples from present) then 'installed'
             else 'MISSING' end,
        'weight_samples -- the analog history a scale appends to'),
    (8, 'migrate_burn_rate.sql',
        case when (select t_burn from present) then 'installed'
             else 'MISSING' end,
        'slot_stock_series + slot_burn_rate -- when a dish runs out'),
    (9, 'migrate_manual_fill.sql',
        case when (select c_fill from present) then 'installed'
             else 'MISSING' end,
        'device_status.manual_fill_pct -- FIRMWARE PATCHES THIS COLUMN; '
        'absent means 400 on every post and the scale goes silent'),
    (10, 'migrate_vbus_sense.sql',
        case when (select c_extpwr from present) then 'installed'
             else 'MISSING' end,
        'device_status.external_power -- FIRMWARE PATCHES THIS COLUMN; '
        'absent means 400 on every post and the scale goes silent')
) as t(item, name, status, note)
order by item;

-- ---------------------------------------------------------------------
--  THE ORDER, if you are starting from a database that has only the base
--  schema. Each is idempotent, so re-running a step that is already
--  installed converges rather than erroring.
--
--    1. migrate_bowl_weight.sql     <- the Master banner is asking for this
--    2. migrate_loadcell.sql        <- refuses unless 1 has run
--    3. register_loadcells.sql      <- refuses unless 2 has run (needs kind)
--    4. assign_loadcells.sql        <- mirrors each LDC onto its BWL
--    5. migrate_weight_samples.sql  <- the history a scale appends to
--    6. migrate_burn_rate.sql       <- reads weight_samples, so after 5
--    7. migrate_manual_fill.sql     <- the trial's manual estimate
--    8. migrate_vbus_sense.sql      <- mains presence; refuses unless 2 has run
--    9. smoke_test.sql              <- 32 assertions; expect ALL PASS
--
--  Steps 5-8 are what apply_loadcell.sql fuses together with 1-4, so running
--  that one file instead is the shorter route and the one the docs point at.
--
--  weekly_menu_and_offline.sql, if it has never been run, goes AFTER 2 --
--  it carries its own copies of two views whose bodies now read devices.kind,
--  and it checks for that before writing anything.
--
--  Nothing here drops a table or rewrites an existing row, so all of it is
--  safe mid-service. The one thing that CHANGES on screen the moment step 2
--  lands is bowl capacity at any position where a load cell is registered --
--  which is correct, and is the whole point of the kind filter.
-- ---------------------------------------------------------------------
