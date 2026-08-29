-- =====================================================================
--  Migration -- weight_samples, the load cell's history.
--
--  Run as owner in the Supabase SQL editor, AFTER migrate_loadcell.sql.
--  Idempotent and NON-destructive: it creates one table, one trigger, two
--  indexes and one policy. It touches nothing that already exists.
--
--  Reversible: supabase/rollback_weight_samples.sql.
--
--  ---------------------------------------------------------------------
--  WHY A SECOND TABLE RATHER THAN A COLUMN ON status_events
--  ---------------------------------------------------------------------
--  Because a scale cannot physically be inserted into status_events. Five of
--  its NOT NULL columns are bowl-shaped:
--
--      stack_count  stack_status  levels  sensors_ok  sensors_online
--
--  A load cell has none of those, so it could only append there by
--  FABRICATING A BOWL COUNT -- which is the one failure this codebase
--  refuses everywhere. Widening them to nullable is worse than a second
--  table: it would drop the guarantee that every historical bowl reading is
--  complete, to accommodate rows that are not bowl readings at all.
--
--  docs/PHASE1_BOWL_WEIGHT.md called this at Phase 4 and named the table:
--  "a weight_samples table for the analog history -- NOT status_events,
--  which is stack-shaped and NOT NULL on stack columns".
--
--  ---------------------------------------------------------------------
--  WHAT IT IS FOR
--  ---------------------------------------------------------------------
--  device_status answers "how much is on the counter NOW" and is one row per
--  device, updated in place. It cannot answer "how fast is it going", which
--  is the question worth the most operationally -- burn rate, and from it
--  "Rice runs out ~13:42". That needs a series, and this is the series.
--
--  ---------------------------------------------------------------------
--  APPENDED ON CHANGE, NOT ON THE HEARTBEAT
--  ---------------------------------------------------------------------
--  The firmware posts its current state every 20 s. If every one of those
--  became a row this table would take ~4,300 rows per device per day --
--  103,000 a day across 24 stations, for a number that mostly has not moved.
--  status_events avoids that by appending only on real change, and takes
--  ~282 rows/day across the whole bowl fleet as a result.
--
--  A weight is continuous and never stops moving, so "real change" needs a
--  threshold rather than an equality test. The firmware uses 250 g -- below
--  any single serving, far above the noise floor -- plus every change of
--  weight_state and every boot. See WEIGHT_EVENT_G in
--  src/loadcell/scale_telemetry.cpp.
-- =====================================================================

begin;

-- ---------------------------------------------------------------------
-- 0. Prerequisite.
-- ---------------------------------------------------------------------
do $$
begin
  if not exists (select 1 from information_schema.columns
                  where table_schema = 'public' and table_name = 'devices'
                    and column_name = 'kind') then
    raise exception
      'Run supabase/migrate_loadcell.sql FIRST -- this table is only '
      'meaningful for devices.kind = ''scale'', and the guard trigger below '
      'reads that column. Nothing has been changed.';
  end if;
end $$;

-- ---------------------------------------------------------------------
-- 1. The table.
-- ---------------------------------------------------------------------
create table if not exists public.weight_samples (
  -- identity, not bigserial: serial compiles to nextval(), whose ACL is
  -- checked against the invoker, so anon would need GRANT USAGE ON SEQUENCE.
  -- Identity is evaluated internally with that check skipped, and the id
  -- cannot be spoofed by the client either. Same reasoning as status_events.
  id             bigint generated always as identity primary key,

  device_id      text        not null
                   references public.devices(device_id) on delete restrict,

  -- Idempotency without ON CONFLICT: a retried batch violates this and raises
  -- 23505, which the firmware already treats as "already recorded". Granting
  -- anon the SELECT that ON CONFLICT needs would let any device read the whole
  -- fleet's history.
  boot_id        bigint      not null,
  seq            bigint      not null check (seq >= 0),

  -- bigint, not int: the device sends (uint32_t)(millis() - eventMs), which
  -- can exceed INT32_MAX. PostgREST would reject that with a 400 BEFORE the
  -- trigger could clamp it, and the device would retry the batch forever.
  age_ms         bigint      not null check (age_ms between 0 and 604800000),

  recorded_at    timestamptz not null,  -- now() - age_ms, set by trigger
  received_at    timestamptz not null,  -- raw arrival time, set by trigger

  reason         text        not null
                   check (reason in ('boot','change','periodic')),

  -- THE INVERSION, AND IT IS THE WHOLE DESIGN. The STATE is always known; the
  -- NUMBER is not. A scale that has never seen a known mass has counts and no
  -- grams -- so weight_state is NOT NULL and weight_g is nullable, which is
  -- the opposite way round from every instinct.
  --
  -- NOT NULL here where device_status leaves it nullable, because that row
  -- exists before the device has ever spoken and this one is only ever
  -- created BY a device that is speaking.
  weight_state   text        not null
                   check (weight_state in ('ok','uncalibrated','untared',
                                           'settling','cells_partial',
                                           'no_cells','over_range')),
  weight_g       integer     check (weight_g is null
                                    or weight_g between -5000 and 100000),

  -- The honesty rule, made structural -- and UNCONDITIONAL here, unlike
  -- device_status, because weight_state cannot be null. A gram figure exists
  -- exactly when the state says it does.
  constraint weight_samples_agrees_ck
    check ((weight_state = 'ok') = (weight_g is not null)),

  cells_online   smallint    check (cells_online is null
                                    or cells_online between 0 and 8),

  -- Raw counts net of the platform zero and the session tare. Kept even when
  -- there are no grams: it is the evidence that the cells are alive and
  -- moving, and on an uncalibrated unit it is the ONLY measurement there is.
  net_counts     bigint,
  counts_per_gram numeric(9,3),

  -- Same 6000 mV ceiling as device_status, and coupled to
  -- config::BATTERY_PUBLISH_MAX_MV for the same reason: a value the firmware
  -- can produce but this rejects is a 400 that fails the whole INSERT.
  battery_mv     integer     check (battery_mv is null
                                    or battery_mv between 0 and 6000),
  battery_level  text        check (battery_level in ('good','medium','low','critical')),

  firmware       text        not null,

  constraint weight_samples_once unique (device_id, boot_id, seq)
);

-- The read path the burn-rate question will take: one device, most recent
-- first. Mirrors status_events_device_time_idx exactly.
create index if not exists weight_samples_device_time_idx
  on public.weight_samples (device_id, recorded_at desc);
create index if not exists weight_samples_recorded_at_idx
  on public.weight_samples (recorded_at);

comment on table public.weight_samples is
  'Append-only weight history for devices.kind = ''scale''. Separate from '
  'status_events because that table is NOT NULL on five bowl-shaped columns '
  'a load cell cannot supply. Appended on CHANGE, not on the heartbeat: the '
  'current figure lives in device_status, and this is what makes burn rate '
  'and projected run-out answerable.';

comment on column public.weight_samples.weight_state is
  'Why weight_g is or is not a number. NOT NULL -- the state is always '
  'knowable even when the weight is not, which is the inversion this whole '
  'table is built around.';

-- ---------------------------------------------------------------------
-- 2. Clock-free timestamps.
--
-- The device has no RTC and no guaranteed NTP: it reports how long ago each
-- buffered sample happened and the server supplies the absolute reference.
-- This is what lets offline samples replay with correct times.
--
-- Byte-for-byte the same logic as tg_status_events_stamp, deliberately -- two
-- histories that timestamp differently would be impossible to read together,
-- and reading them together is the entire point of having both.
-- ---------------------------------------------------------------------
create or replace function public.tg_weight_samples_stamp()
returns trigger language plpgsql set search_path = '' as $$
declare v_age bigint := greatest(coalesce(new.age_ms, 0), 0);
begin
  -- Clamp rather than reject: a rejected batch is retried forever by a device
  -- that cannot fix its own arithmetic. (millis() wraps at 49.7 days, so a
  -- week is already generous.)
  v_age := least(v_age, 604800000);

  -- now() is transaction_timestamp, so every row of one batched POST shares a
  -- reference instant and relative ordering within the batch is exact.
  -- clock_timestamp() would drift across rows.
  new.age_ms      := v_age;
  new.recorded_at := now() - (v_age * interval '1 millisecond');
  new.received_at := now();
  return new;
end $$;

drop trigger if exists weight_samples_stamp on public.weight_samples;
create trigger weight_samples_stamp
  before insert on public.weight_samples
  for each row execute function public.tg_weight_samples_stamp();

-- ---------------------------------------------------------------------
-- 3. THE GUARD status_events NEVER HAD.
--
-- A bowl counter must not be able to write here, and the foreign key cannot
-- say so -- it only checks the device exists. A CHECK cannot say so either:
-- it may not reference another table.
--
-- This matters because the whole value of two tables is that each one is
-- ENTIRELY one kind of reading. A stray stack row in here would silently
-- corrupt any burn-rate calculation built on it, and would do so while
-- looking perfectly well-formed.
--
-- The mirror guard on status_events -- rejecting scales -- is deliberately
-- NOT added by this migration. It would be correct, but status_events is the
-- live bowl-counter path with 7,898 rows and 15 devices writing to it, and a
-- new trigger there is a change to a working product for no benefit today: no
-- scale can reach it anyway, because it cannot satisfy the NOT NULLs.
-- ---------------------------------------------------------------------
-- SECURITY DEFINER, and it is not optional. The trigger reads public.devices,
-- and the role doing the INSERT is anon -- which has NO SELECT on that table,
-- deliberately: a device must not be able to enumerate the fleet.
--
-- Without this the guard does not merely fail to guard, it REJECTS EVERY
-- INSERT with 42501 "permission denied for table devices". Caught locally
-- before this reached anything; the symptom in the field would have been a
-- load cell reporting current state perfectly while its history stayed
-- permanently empty, backing off from a permission error nobody was watching
-- for. tg_devices_create_status() carries the same modifier for exactly this
-- reason -- it inserts into device_status on behalf of a caller who cannot.
--
-- Safe because it reads one column of one row by primary key and returns no
-- data to the caller: the only thing it can leak is whether an insert
-- succeeds, which the caller learns anyway.
create or replace function public.tg_weight_samples_kind()
returns trigger language plpgsql security definer set search_path = '' as $$
declare v_kind text;
begin
  select kind into v_kind from public.devices where device_id = new.device_id;
  if v_kind is distinct from 'scale' then
    raise exception
      'weight_samples accepts only devices.kind = ''scale''; % is %',
      new.device_id, coalesce(v_kind, 'not registered')
      using errcode = 'check_violation';
  end if;
  return new;
end $$;

drop trigger if exists weight_samples_kind on public.weight_samples;
create trigger weight_samples_kind
  before insert on public.weight_samples
  for each row execute function public.tg_weight_samples_kind();

-- ---------------------------------------------------------------------
-- 4. Security -- the same posture as status_events.
--
-- INSERT only for anon, on named columns, with no SELECT of any kind: a
-- device can append its own history and cannot read anybody's. The duplicate
-- that idempotency relies on surfaces as 23505 from the unique constraint
-- rather than needing ON CONFLICT, which would require a SELECT grant.
-- ---------------------------------------------------------------------
alter table public.weight_samples enable row level security;

revoke all on public.weight_samples from anon, authenticated, public;

drop policy if exists weight_samples_insert_device on public.weight_samples;
create policy weight_samples_insert_device on public.weight_samples
  for insert to anon with check (true);

drop policy if exists weight_samples_select_staff on public.weight_samples;
create policy weight_samples_select_staff on public.weight_samples
  for select to authenticated using (true);

-- recorded_at and received_at are NOT granted: the trigger sets them, and a
-- device that could write them could backdate its own history.
grant insert (device_id, boot_id, seq, age_ms, reason,
              weight_state, weight_g, cells_online, net_counts,
              counts_per_gram, battery_mv, battery_level, firmware)
  on public.weight_samples to anon;

grant select on public.weight_samples to authenticated;

commit;

-- ---------------------------------------------------------------------
--  Verify.
-- ---------------------------------------------------------------------
select 'table'      as object, count(*)::text as detail from pg_tables
  where schemaname='public' and tablename='weight_samples'
union all
select 'columns',   count(*)::text from information_schema.columns
  where table_schema='public' and table_name='weight_samples'
union all
select 'triggers',  count(*)::text from pg_trigger tg
  join pg_class cl on cl.oid=tg.tgrelid where cl.relname='weight_samples'
    and not tg.tgisinternal
union all
select 'policies',  count(*)::text from pg_policy pol
  join pg_class cl on cl.oid=pol.polrelid where cl.relname='weight_samples'
union all
select 'anon insert cols', count(*)::text from information_schema.column_privileges
  where table_schema='public' and table_name='weight_samples'
    and grantee='anon' and privilege_type='INSERT'
union all
select 'anon select cols', count(*)::text from information_schema.column_privileges
  where table_schema='public' and table_name='weight_samples'
    and grantee='anon' and privilege_type='SELECT';
-- Expect: table 1, columns 16, triggers 2, policies 2,
--         anon insert cols 13, anon select cols 0
