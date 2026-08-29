-- =====================================================================
--  Example meal mappings -- so the front-end test bed shows dish names
--
--  Run as owner, after assign_devices.sql. IDEMPOTENT: upserts on the natural
--  key, so re-running refreshes names without duplicating rows.
--
--  This is SAMPLE DATA, not configuration. Real menus are entered from the admin
--  page (Part 3). It exists so that a dashboard built against the simulated fleet
--  renders "Rice" rather than "slot 1", which is the difference between being able
--  to review a screen and having to imagine it.
--
--  Seeded for TODAY and YESTERDAY, both in Asia/Kolkata:
--    - today   gives the dashboard something current to display
--    - yesterday gives meal_mapping_preload() something to inherit from, which is
--      the Part 4 behaviour and is otherwise untestable on a fresh database
-- =====================================================================

begin;

with menus(location, meal_type, food_slot, food_name) as (
  values
    -- Darshanarthi, the example given
    ('D', 'Breakfast', 1, 'Idli'),
    ('D', 'Breakfast', 2, 'Sambar'),
    ('D', 'Breakfast', 3, 'Chutney'),
    ('D', 'Breakfast', 4, 'Tea'),
    ('D', 'Breakfast', 5, 'Upma'),

    ('D', 'Lunch',     1, 'Rice'),
    ('D', 'Lunch',     2, 'Dal'),
    ('D', 'Lunch',     3, 'Curry'),
    ('D', 'Lunch',     4, 'Roti'),
    ('D', 'Lunch',     5, 'Khichdi'),

    ('D', 'Dinner',    1, 'Rice'),
    ('D', 'Dinner',    2, 'Kadhi'),
    ('D', 'Dinner',    3, 'Sabzi'),
    ('D', 'Dinner',    4, 'Roti'),
    ('D', 'Dinner',    5, 'Sweet'),

    -- Mahatma
    ('M', 'Breakfast', 1, 'Poha'),
    ('M', 'Breakfast', 2, 'Jalebi'),
    ('M', 'Breakfast', 3, 'Tea'),
    ('M', 'Breakfast', 4, 'Banana'),
    ('M', 'Breakfast', 5, 'Milk'),

    ('M', 'Lunch',     1, 'Rice'),
    ('M', 'Lunch',     2, 'Dal'),
    ('M', 'Lunch',     3, 'Sabzi'),
    ('M', 'Lunch',     4, 'Roti'),
    ('M', 'Lunch',     5, 'Salad'),

    ('M', 'Dinner',    1, 'Khichdi'),
    ('M', 'Dinner',    2, 'Kadhi'),
    ('M', 'Dinner',    3, 'Sabzi'),
    ('M', 'Dinner',    4, 'Roti'),
    ('M', 'Dinner',    5, 'Papad'),

    -- Tiffin
    ('T', 'Breakfast', 1, 'Thepla'),
    ('T', 'Breakfast', 2, 'Chutney'),
    ('T', 'Breakfast', 3, 'Tea'),
    ('T', 'Breakfast', 4, 'Fruit'),
    ('T', 'Breakfast', 5, 'Curd'),

    ('T', 'Lunch',     1, 'Rice'),
    ('T', 'Lunch',     2, 'Dal'),
    ('T', 'Lunch',     3, 'Bhaji'),
    ('T', 'Lunch',     4, 'Roti'),
    ('T', 'Lunch',     5, 'Chaas'),

    ('T', 'Dinner',    1, 'Pulao'),
    ('T', 'Dinner',    2, 'Dal Fry'),
    ('T', 'Dinner',    3, 'Sabzi'),
    ('T', 'Dinner',    4, 'Puri'),
    ('T', 'Dinner',    5, 'Halwa')
),
-- Two service dates. The date is taken in Asia/Kolkata, not the server's zone:
-- a 21:00 dinner in India is already the next day in UTC, so now()::date would
-- file the evening meal against tomorrow.
dates(meal_date) as (
  values (public.current_meal_date('Asia/Kolkata')),
         (public.current_meal_date('Asia/Kolkata') - 1)
)
insert into public.meal_food_mapping
       (location, meal_type, meal_date, food_slot, food_name)
select m.location, m.meal_type, d.meal_date, m.food_slot::smallint, m.food_name
  from menus m cross join dates d
    on conflict (location, meal_date, meal_type, food_slot)
    do update set food_name = excluded.food_name;

commit;

-- ---------------------------------------------------------------------
--  Verify
-- ---------------------------------------------------------------------
select meal_date, location, meal_type,
       string_agg(food_slot || ':' || food_name, '  ' order by food_slot) as slots
  from public.meal_food_mapping
 group by meal_date, location, meal_type
 order by meal_date desc, location, meal_type;

-- What the dashboard resolves right now, per dish position.
select location, food_slot, current_food, current_meal,
       devices, bowls_trusted, any_fault, any_degraded
  from public.slot_overview
 order by location, food_slot;
