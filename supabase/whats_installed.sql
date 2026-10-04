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
    -- The view too: schema.sql now carries the column (the kind guard reads
    -- it) but not device_power, which the dashboard reads mains from -- so
    -- the column alone no longer proves the migration ran.
    to_regclass('public.device_power')            is not null as v_power,
    -- migrate_buffer.sql: the columns and the guard. Both, because a firmware
    -- buffer PATCH needs the columns and the cut-over needs the guard.
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='device_status'
        and column_name='bowls')                                    > 0 as c_bowls,
    to_regprocedure('public.tg_device_status_kind()') is not null       as f_kindguard,
    -- migrate_hubs.sql: the four node-health columns and the three hubs.
    -- Both, because the columns are what a platform PATCH needs and the hub
    -- rows are what the hub's own PATCH needs -- a missing one is a 400, a
    -- missing other is a PATCH that matches nothing.
    (select count(*) from information_schema.columns
      where table_schema='public' and table_name='device_status'
        and column_name in ('supply_mv','checksum_errors','responding','no_load_g'))
                                                                    as n_health,
    -- kind through to_jsonb(), as below, so this parses before kind exists.
    (select count(*) from public.devices d
      where d.device_id in ('HUB-D','HUB-M','HUB-T')
        and to_jsonb(d) ->> 'kind' = 'hub')                         as n_hubs,
    -- migrate_weight_samples_brin.sql: the time index is BRIN, not btree.
    to_regclass('public.weight_samples_recorded_at_brin') is not null   as i_brin,
    to_regclass('public.weight_samples_recorded_at_idx') is not null    as i_btree,
    -- migrate_statistics.sql: the Statistics page's two functions.
    to_regprocedure('public.slot_meal_series(date, text, integer)') is not null as f_series,
    to_regprocedure('public.slot_meal_stats(date, date, text, integer)') is not null as f_stats,
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
),
-- What the BWL ids are now. Read through to_jsonb() so the query parses on a
-- database with no kind column at all -- there every BWL is a stack, which is
-- what the coalesce says.
bwl as (
  select count(*) filter (where coalesce(to_jsonb(d) ->> 'kind', 'stack') = 'buffer') as n_buffer,
         count(*) filter (where coalesce(to_jsonb(d) ->> 'kind', 'stack') = 'stack')  as n_stack
    from public.devices d
   where d.device_id like 'BWL-%'
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
        case when (select c_extpwr and v_power from present) then 'installed'
             when (select c_extpwr or v_power from present) then 'PARTIAL'
             else 'MISSING' end,
        'device_status.external_power + the device_power view -- FIRMWARE '
        'PATCHES THIS COLUMN (a hub with its reading, a platform with null); '
        'absent means 400 on every post and the device goes silent'),
    (11, 'migrate_buffer.sql',
        case when (select c_bowls and f_kindguard from present) then 'installed'
             when (select c_bowls or f_kindguard from present) then 'PARTIAL'
             else 'MISSING' end,
        'kind buffer, bowls/gross_g, the kind guards, measured buffer kg in the '
        'views -- a buffer PATCH answers 400 without it'),
    (12, 'cutover_buffers.sql',
        case when not (select c_bowls from present) then 'blocked by 11'
             when (select n_buffer from bwl) = 0 then 'not run'
             when (select n_stack from bwl) = 0 then 'done'
             else 'PARTIAL' end,
        (select n_buffer from bwl)::text || ' buffer, '
          || (select n_stack from bwl)::text || ' stack among BWL-* -- run it '
          || 'only once the web that reads buffers is live'),
    (13, 'migrate_hubs.sql',
        case when (select n_health = 4 and n_hubs = 3 from present) then 'installed'
             when (select n_health > 0 or n_hubs > 0 from present) then 'PARTIAL'
             else 'MISSING' end,
        (select n_hubs from present)::text || ' of HUB-D/M/T registered, '
          || (select n_health from present)::text || ' of 4 node-health '
          || 'columns -- battery is the hub''s; stop every writer of platform '
          || 'battery BEFORE running it, or that platform answers 400'),
    (14, 'migrate_weight_samples_brin.sql',
        case when (select i_brin and not i_btree from present) then 'installed'
             when (select i_brin from present) then 'PARTIAL'
             else 'MISSING' end,
        'weight_samples time index as BRIN -- without it the dashboard''s '
          || 'stock series and burn rate take seconds per refresh'),
    (15, 'migrate_statistics.sql',
        case when (select f_series and f_stats from present) then 'installed'
             when (select f_series or f_stats from present) then 'PARTIAL'
             else 'MISSING' end,
        'slot_meal_series + slot_meal_stats -- the Statistics page reads nothing '
          || 'without them')
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
--    9. migrate_buffer.sql          <- BWL buffer platforms; refuses unless 6
--                                      has run. Additive: no bowl figure
--                                      moves, every BWL stays a stack
--   10. migrate_hubs.sql            <- HUB-D/M/T carry the battery; platforms
--                                      report node health. Refuses unless 9
--                                      has run. Clears platform battery
--   11. smoke_test.sql              <- 43 assertions; expect ALL PASS
--
--  Steps 5-10 are what apply_loadcell.sql fuses together with 1-4, so running
--  that one file instead is the shorter route and the one the docs point at.
--
--  cutover_buffers.sql is NOT in that sequence. It re-kinds every BWL to
--  'buffer' -- the one step that changes the dashboard -- and goes only after
--  the web that reads buffers has been pushed. Then the kg fleet simulator.
--
--  weekly_menu_and_offline.sql, if it has never been run, goes AFTER 10 --
--  it carries its own copies of two views whose bodies now read devices.kind,
--  the buffer columns and the node health, and it checks for those before
--  writing anything.
--
--  Nothing here drops a table or rewrites an existing row, so all of it is
--  safe mid-service. The one thing that CHANGES on screen the moment step 2
--  lands is bowl capacity at any position where a load cell is registered --
--  which is correct, and is the whole point of the kind filter.
-- ---------------------------------------------------------------------
