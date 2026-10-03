-- =====================================================================
--  The two burn-rate views EXACTLY as the live database had them on
--  2026-10-04, before migrate_buffer.sql. Captured read-only from
--  pg_views / pg_class on project foodcount (vsqefdifovknyxtgpami).
--
--  WHY THIS FILE EXISTS. The live database never received the server side
--  of 3fecb53 (2026-09-01: burn rate keyed per hall), so its
--  slot_stock_series and slot_burn_rate are the OLDER single-total bodies,
--  without a `location` column. Everything else migrate_buffer.sql touches
--  matched the repo exactly (fingerprinted the same day). rollback_buffer.sql
--  restores the REPO's version of these two -- the per-hall one -- which is
--  the newer, corrected view, not today's live one.
--
--  RUN THIS ONLY AFTER rollback_buffer.sql, and only if you want those two
--  views back exactly as they were. Dashboard 1.41 reads both shapes
--  (it selects food_slot, at_ts, total_g); 1.42 needs the per-hall one for
--  its sparklines, so do not run this with 1.42 live.
--
--  Run as owner in the Supabase SQL editor. One transaction.
-- =====================================================================

begin;

drop view if exists public.slot_burn_rate;     -- depends on slot_stock_series
drop view if exists public.slot_stock_series;

create view public.slot_stock_series with (security_invoker = true) as
 WITH t AS (
         SELECT burn_rate_tuning.window_span,
            burn_rate_tuning.refill_g,
            burn_rate_tuning.min_span
           FROM burn_rate_tuning() burn_rate_tuning(window_span, refill_g, min_span)
        ), grid AS (
         SELECT g.g AS at_ts
           FROM t,
            LATERAL generate_series((now() - t.window_span), now(), '00:05:00'::interval) g(g)
        ), wt AS (
         SELECT d.location,
            d.food_slot,
            max(m.bowl_weight_g) AS bowl_weight_g
           FROM ((devices d
             CROSS JOIN LATERAL last_served_meal(d.timezone) lm(meal_date, meal_type))
             LEFT JOIN meal_food_mapping m ON (((m.location = d.location) AND (m.food_slot = d.food_slot) AND (m.meal_date = lm.meal_date) AND (m.meal_type = lm.meal_type))))
          WHERE ((d.location = ANY (ARRAY['D'::text, 'M'::text, 'T'::text])) AND (d.food_slot IS NOT NULL))
          GROUP BY d.location, d.food_slot
        ), buffer AS (
         SELECT g.at_ts,
            d.food_slot,
            sum((e.stack_count * w.bowl_weight_g)) AS buffer_g,
            bool_or(((w.bowl_weight_g IS NULL) AND (e.stack_count > 0))) AS partial
           FROM (((grid g
             JOIN devices d ON (((d.kind = 'stack'::text) AND (d.location = ANY (ARRAY['D'::text, 'M'::text, 'T'::text])) AND (d.food_slot IS NOT NULL))))
             JOIN wt w ON (((w.location = d.location) AND (w.food_slot = d.food_slot))))
             CROSS JOIN LATERAL ( SELECT ev.stack_count
                   FROM status_events ev
                  WHERE ((ev.device_id = d.device_id) AND (ev.recorded_at <= g.at_ts) AND (ev.stack_status = 'ok'::text))
                  ORDER BY ev.recorded_at DESC
                 LIMIT 1) e)
          GROUP BY g.at_ts, d.food_slot
        ), counter AS (
         SELECT g.at_ts,
            d.food_slot,
            sum(s.weight_g) AS counter_g,
            count(*) AS scales
           FROM ((grid g
             JOIN devices d ON (((d.kind = 'scale'::text) AND (d.location = ANY (ARRAY['D'::text, 'M'::text, 'T'::text])) AND (d.food_slot IS NOT NULL))))
             CROSS JOIN LATERAL ( SELECT ws.weight_g
                   FROM weight_samples ws
                  WHERE ((ws.device_id = d.device_id) AND (ws.recorded_at <= g.at_ts) AND (ws.weight_state = 'ok'::text) AND (ws.weight_g IS NOT NULL))
                  ORDER BY ws.recorded_at DESC
                 LIMIT 1) s)
          GROUP BY g.at_ts, d.food_slot
        )
 SELECT COALESCE(b.at_ts, c.at_ts) AS at_ts,
    COALESCE(b.food_slot, c.food_slot) AS food_slot,
    b.buffer_g,
    c.counter_g,
        CASE
            WHEN ((b.buffer_g IS NULL) AND (c.counter_g IS NULL)) THEN NULL::bigint
            ELSE (COALESCE(b.buffer_g, (0)::bigint) + COALESCE(c.counter_g, (0)::bigint))
        END AS total_g,
    COALESCE(b.partial, false) AS buffer_is_partial,
    COALESCE(c.scales, (0)::bigint) AS scales
   FROM (buffer b
     FULL JOIN counter c ON (((c.at_ts = b.at_ts) AND (c.food_slot = b.food_slot))));

create view public.slot_burn_rate with (security_invoker = true) as
 WITH t AS (
         SELECT burn_rate_tuning.window_span,
            burn_rate_tuning.refill_g,
            burn_rate_tuning.min_span
           FROM burn_rate_tuning() burn_rate_tuning(window_span, refill_g, min_span)
        ), pts AS (
         SELECT slot_stock_series.at_ts,
            slot_stock_series.food_slot,
            slot_stock_series.buffer_g,
            slot_stock_series.counter_g,
            slot_stock_series.total_g,
            slot_stock_series.buffer_is_partial,
            slot_stock_series.scales
           FROM slot_stock_series
          WHERE (slot_stock_series.total_g IS NOT NULL)
        ), lagged AS (
         SELECT p.at_ts,
            p.food_slot,
            p.buffer_g,
            p.counter_g,
            p.total_g,
            p.buffer_is_partial,
            p.scales,
            lag(p.total_g) OVER w AS prev_g
           FROM pts p
          WINDOW w AS (PARTITION BY p.food_slot ORDER BY p.at_ts)
        ), steps AS (
         SELECT l.at_ts,
            l.food_slot,
            l.buffer_g,
            l.counter_g,
            l.total_g,
            l.buffer_is_partial,
            l.scales,
            l.prev_g,
            count(*) FILTER (WHERE ((l.total_g - l.prev_g) >= ( SELECT t.refill_g
                   FROM t))) OVER (PARTITION BY l.food_slot ORDER BY l.at_ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS segment
           FROM lagged l
        ), seg AS (
         SELECT s.food_slot,
            s.segment,
            min(s.at_ts) AS seg_first_at,
            max(s.at_ts) AS seg_last_at,
            ((array_agg(s.total_g ORDER BY s.at_ts))[1] - (array_agg(s.total_g ORDER BY s.at_ts DESC))[1]) AS drop_g,
            EXTRACT(epoch FROM (max(s.at_ts) - min(s.at_ts))) AS seg_s,
            bool_or(s.buffer_is_partial) AS partial
           FROM steps s
          GROUP BY s.food_slot, s.segment
         HAVING (count(*) >= 2)
        ), agg AS (
         SELECT g.food_slot,
            min(g.seg_first_at) AS first_at,
            max(g.seg_last_at) AS last_at,
            sum(GREATEST(g.drop_g, (0)::bigint)) AS consumed_g,
            sum(g.seg_s) FILTER (WHERE (g.drop_g > 0)) AS consuming_s,
            max(g.seg_first_at) FILTER (WHERE (g.segment > 0)) AS last_delivery_at,
            bool_or(g.partial) AS partial
           FROM seg g
          GROUP BY g.food_slot
        )
 SELECT a.food_slot,
    ( SELECT array_agg(DISTINCT m.food_name) AS array_agg
           FROM ((devices d
             CROSS JOIN LATERAL last_served_meal(d.timezone) lm(meal_date, meal_type))
             JOIN meal_food_mapping m ON (((m.location = d.location) AND (m.food_slot = d.food_slot) AND (m.meal_date = lm.meal_date) AND (m.meal_type = lm.meal_type))))
          WHERE ((d.food_slot = a.food_slot) AND (d.location = ANY (ARRAY['D'::text, 'M'::text, 'T'::text])) AND (m.food_name IS NOT NULL))) AS dishes,
    (a.last_at - a.first_at) AS covered,
    a.consumed_g AS consumed_g_window,
    n.buffer_g,
    n.counter_g,
    n.total_g,
    a.last_delivery_at,
    a.partial AS is_partial,
        CASE
            WHEN (((a.last_at - a.first_at) >= ( SELECT t.min_span
               FROM t)) AND (COALESCE(a.consuming_s, (0)::numeric) > (0)::numeric)) THEN round((a.consumed_g / (a.consuming_s / 3600.0)), 0)
            ELSE NULL::numeric
        END AS g_per_hour,
        CASE
            WHEN (((a.last_at - a.first_at) >= ( SELECT t.min_span
               FROM t)) AND (COALESCE(a.consuming_s, (0)::numeric) > (0)::numeric) AND (a.consumed_g > (0)::numeric) AND (n.total_g IS NOT NULL)) THEN (now() + make_interval(secs => (((n.total_g)::numeric / (a.consumed_g / a.consuming_s)))::double precision))
            ELSE NULL::timestamp with time zone
        END AS runs_out_at,
        CASE
            WHEN (((a.last_at - a.first_at) >= ( SELECT t.min_span
               FROM t)) AND (COALESCE(a.consuming_s, (0)::numeric) > (0)::numeric) AND (a.consumed_g > (0)::numeric) AND (n.total_g IS NOT NULL)) THEN ((now() + make_interval(secs => (((n.total_g)::numeric / (a.consumed_g / a.consuming_s)))::double precision)) < (last_service_window_end('Asia/Kolkata'::text, NULL::text) + ('1 day'::interval * (0)::double precision)))
            ELSE NULL::boolean
        END AS short_before_close
   FROM (agg a
     JOIN LATERAL ( SELECT s.buffer_g,
            s.counter_g,
            s.total_g
           FROM slot_stock_series s
          WHERE ((s.food_slot = a.food_slot) AND (s.total_g IS NOT NULL))
          ORDER BY s.at_ts DESC
         LIMIT 1) n ON (true));

revoke all on public.slot_stock_series from anon, authenticated, public;
revoke all on public.slot_burn_rate    from anon, authenticated, public;
grant select on public.slot_stock_series to authenticated;
grant select on public.slot_burn_rate    to authenticated;

comment on view public.slot_stock_series is
  'Total food per dish position over the last hour, on a 5-minute grid: buffered bowls (BWL, valued at the menu per-bowl weight) PLUS the active counter (LDC, weighed). Summing both is what makes a buffer-to-counter refill read as flat rather than as a phantom serving.';
comment on view public.slot_burn_rate is
  'Consumption per dish position, over buffered stock PLUS the active counter combined -- so moving bowls from the buffer onto the counter reads as flat rather than as a serving. g_per_hour and runs_out_at are NULL when the series is too short to support them; is_partial marks a total that could not value every hall''s buffer, which makes the rate a lower bound.';

commit;

select viewname, md5(definition) as fingerprint,
       case viewname when 'slot_stock_series' then '5c83a4eb7e01d1fd8b054f4ba1530b04'
                     when 'slot_burn_rate'    then '63a53f2d88dd59dfcd867b1eb7763990' end as live_on_20261004
  from pg_views where schemaname = 'public' and viewname in ('slot_stock_series','slot_burn_rate');
-- Expect the two fingerprints to equal live_on_20261004.
