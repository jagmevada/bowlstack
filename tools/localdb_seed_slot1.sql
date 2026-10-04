-- LOCAL SUPABASE ONLY (bash tools/localdb.sh seed). Dummy history for the
-- Statistics page: BWL-001 (buffer) + LDC-001 (counter) on D slot 1, every meal
-- of the last 10 days, a sample every 2 minutes.
--
-- The model, per meal: the buffer starts with 3 bowls (16.5 kg of food each) and
-- the counter with one bowl's food. People eat from the counter at a base rate
-- with a rush through the middle of the meal; when the counter drops under 2 kg a
-- bowl goes from the buffer to it (flat on buffer + counter). Even days get a
-- kitchen delivery of 2 bowls at 45% of the window; every third day eats faster,
-- so some meals run out before close.
--
-- REFUSES TO RUN if BWL-001 or LDC-001 hold readings that are not this seed's --
-- on live they do, so this can never write production history. It also clears
-- the other slot-1 platforms' history (fleet_sim's) so slot 1 is exactly these two.
do $$
declare
  dd date;
  w record;
  t timestamptz;
  t_end timestamptz;
  n_steps int;
  i int;
  frac float8;
  rate_g float8;
  eat float8;
  buf_bowls int;
  counter_g float8;
  delivered boolean;
  seq bigint;
  boot bigint;
  rush float8;
begin
  if exists (select 1 from public.weight_samples
              where device_id in ('BWL-001', 'LDC-001') and firmware not like 'seed%') then
    raise exception 'BWL-001/LDC-001 hold real readings -- this seed is for the local stack only';
  end if;

  delete from public.weight_samples where firmware like 'seed%';
  delete from public.weight_samples
   where device_id in (select device_id from public.devices
                        where food_slot = 1 and device_id not in ('BWL-001', 'LDC-001'));

  alter table public.weight_samples disable trigger user;

  for dd in select generate_series(current_date - 9, current_date, interval '1 day')::date loop
    for w in select label, starts_at, ends_at from public.service_windows
              where device_id is null order by starts_at loop
      t := (dd + w.starts_at) at time zone 'Asia/Kolkata';
      t_end := least((dd + w.ends_at) at time zone 'Asia/Kolkata', now());
      continue when t_end <= t;
      n_steps := (extract(epoch from t_end - t) / 120)::int;

      -- base kg/min by meal, varied by day; every third day eats 40% faster.
      frac := abs(hashtext(dd::text || w.label)) % 1000 / 1000.0;
      rate_g := case w.label when 'breakfast' then 180 when 'lunch' then 300 else 250 end
                * (0.8 + 0.4 * frac)
                * (case when extract(doy from dd)::int % 3 = 0 then 1.4 else 1.0 end);
      buf_bowls := 3;
      counter_g := 16500;
      delivered := false;
      seq := 0;
      boot := abs(hashtext(dd::text || w.label))::bigint;

      for i in 0 .. n_steps loop
        -- the rush: 1.8x through 35-60% of the window
        rush := case when i >= n_steps * 0.35 and i <= n_steps * 0.60 then 1.8 else 1.0 end;
        if i > 0 then
          eat := rate_g * 2 * rush;
          counter_g := greatest(counter_g - eat, 0);
        end if;
        if not delivered and extract(day from dd)::int % 2 = 0 and i >= n_steps * 0.45 then
          buf_bowls := buf_bowls + 2;   -- the kitchen restocks the buffer
          delivered := true;
        end if;
        if counter_g < 2000 and buf_bowls > 0 then
          buf_bowls := buf_bowls - 1;   -- a bowl carried buffer -> counter
          counter_g := counter_g + 16500;
        end if;

        insert into public.weight_samples
          (device_id, boot_id, seq, age_ms, recorded_at, received_at, reason, weight_state,
           weight_g, cells_online, firmware, bowls, bowls_confirmed, gross_g)
        values
          ('BWL-001', boot, seq, 0, t, t, 'periodic', 'ok',
           buf_bowls * 16500, 1, 'seed-stats', buf_bowls, true, buf_bowls * 19000),
          ('LDC-001', boot, seq, 0, t, t, 'periodic', 'ok',
           round(counter_g)::int, 3, 'seed-stats', null, null, null);
        seq := seq + 1;
        t := t + interval '2 minutes';
      end loop;
    end loop;

    insert into public.meal_food_mapping (location, meal_type, meal_date, food_slot, food_name)
    values ('D', 'Breakfast', dd, 1, 'Poha'), ('D', 'Lunch', dd, 1, 'Dal'),
           ('D', 'Dinner', dd, 1, 'Kadhi')
    on conflict do nothing;
  end loop;

  alter table public.weight_samples enable trigger user;
end $$;

select device_id, count(*) as samples, min(recorded_at) as first, max(recorded_at) as last
  from public.weight_samples where firmware = 'seed-stats' group by 1 order by 1;
