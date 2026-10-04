// Statistics -- how fast each dish drains.
//
// For one slot and one meal: the food on hand (buffer + counter) through the
// meal, summed over D, M and T, with each area's own line; what was eaten, how
// fast, the busiest quarter hour, and whether it lasted. Then the same slot and
// meal day by day, and every measured slot of the meal ranked by kg/h.
//
// A slot is the same dish in all three areas at a given meal (owner,
// 2026-10-04), so the slot is the unit and the dish name is its label.
//
// Reads slot_meal_series / slot_meal_stats (supabase/migrate_statistics.sql),
// cached for a minute: the page re-renders on every 15 s poll, and a past meal
// does not change.

import { h, empty, banner, fillSlot } from '../ui.js';
import { unwrap, describeError } from '../supa.js';
import { weightChart } from '../chart.js';
import {
  MEAL_TYPES, LOCATION_NAMES, SERVING_LOCATIONS, SERVICE_WINDOWS, DEFAULT_TZ,
  FOOD_SLOTS, fmtWeight, fmtClock, fmtDay, serviceDate, addDays,
} from '../domain.js';

const TTL_MS = 60_000;
const cache = new Map();     // key -> { at, data }
const loading = new Map();   // key -> promise, so a poll does not ask twice

/** The ⟳ button means "the current truth": it clears this cache too. */
export function clearStatsCache() { cache.clear(); }

const kg = g => (g == null ? '—' : fmtWeight(g));
const kgh = g => (g == null ? '—' : `${(Number(g) / 1000).toFixed(1)} kg/h`);

/** The meal under way now, else the last one that started -- the one with data. */
function lastStartedMeal(tz) {
  const hhmm = new Intl.DateTimeFormat('en-GB', {
    hour: '2-digit', minute: '2-digit', hour12: false, timeZone: tz,
  }).format(new Date());
  const started = SERVICE_WINDOWS.filter(w => hhmm >= w.start);
  return started.length
    ? { date: serviceDate(tz), meal: started[started.length - 1].meal }
    : { date: addDays(serviceDate(tz), -1), meal: 'Dinner' };
}

/** Cached RPC: whatever is cached now -- stale or not, so a poll's re-render
 *  never blanks the page to "Loading" -- and a refetch with a fill when it lands
 *  if that copy is older than TTL_MS. Null only the first time. */
function fetchOnce(client, key, fn, args, onData, onError) {
  const hit = cache.get(key);
  if (hit && Date.now() - hit.at < TTL_MS) return hit.data;
  if (!loading.has(key)) {
    loading.set(key, client.rpc(fn, args)
      .then(res => unwrap(res) || [])
      .then(data => { cache.set(key, { at: Date.now(), data }); return data; })
      .finally(() => loading.delete(key)));
  }
  loading.get(key).then(onData, onError);
  return hit ? hit.data : null;
}

export function renderStatistics(state, params, ctx) {
  const tz = DEFAULT_TZ;
  const dflt = lastStartedMeal(tz);
  const date = /^\d{4}-\d{2}-\d{2}$/.test(params.get('d') || '') ? params.get('d') : dflt.date;
  const meal = MEAL_TYPES.includes(params.get('m')) ? params.get('m') : dflt.meal;
  const slot = FOOD_SLOTS.includes(Number(params.get('s'))) ? Number(params.get('s')) : 1;
  const days = params.get('r') === '30' ? 30 : 7;
  const go = (patch) => {
    const p = new URLSearchParams({ d: date, m: meal, s: String(slot), r: String(days), ...patch });
    location.hash = `#/statistics?${p}`;
  };

  const frag = document.createDocumentFragment();

  // --- the meal's ranking first: it also names each slot's dish ---------------
  const rankKey = `rank|${date}|${meal}`;
  const rankBox = h('div', { id: 'stats-rank' });
  const renderRank = rows => {
    if (!rows.length) {
      return empty('No slot was weighed during this meal.');
    }
    const sorted = [...rows].sort((a, b) => (Number(b.g_per_hour) || 0) - (Number(a.g_per_hour) || 0));
    return h('div', { class: 'stats-rank' }, ...sorted.map(r => h('a', {
      class: `stats-rank-row${r.food_slot === slot ? ' is-target' : ''}`,
      href: `#/statistics?${new URLSearchParams({ d: date, m: meal, s: String(r.food_slot), r: String(days) })}`,
    },
      h('span', { class: 'stats-slot' }, `${r.food_slot}`),
      h('span', { class: 'stats-dish' }, r.food_name || `Slot ${r.food_slot}`),
      h('b', {}, kgh(r.g_per_hour)),
      h('span', { class: 'dim' }, `${kg(r.consumed_g)} eaten`),
      r.short_before_close ? h('span', { class: 'stats-out' }, `ran out ${fmtClock(r.ran_out_at, tz)}`) : null)));
  };
  const rankRows = fetchOnce(ctx.client, rankKey, 'slot_meal_stats',
    { p_from: date, p_to: date, p_meal: meal },
    rows => {
      fillSlot('stats-rank', rankBox, renderRank(rows));
      refreshSlotNames(rows);
      const title = document.getElementById('stats-title');
      if (title) title.textContent = `Slot ${slot}${dishOf(rows) ? ` · ${dishOf(rows)}` : ''}`;
    },
    err => fillSlot('stats-rank', rankBox, banner('critical', '✕', describeError(err))));
  rankBox.append(rankRows ? renderRank(rankRows) : empty('Loading…'));

  const dishOf = rows => {
    const r = (rows || []).find(x => x.food_slot === slot);
    return r && r.food_name ? r.food_name : null;
  };

  // --- toolbar: day, meal, slot ---------------------------------------------
  const today = serviceDate(tz);
  const dayChips = h('div', { class: 'toolbar stats-days' },
    ...Array.from({ length: 7 }, (_, i) => addDays(today, i - 6)).map(d => h('button', {
      class: 'ghost', 'aria-pressed': String(d === date), onclick: () => go({ d }),
    }, d === today ? 'Today' : new Intl.DateTimeFormat('en-GB', { weekday: 'short', day: '2-digit' })
      .format(new Date(`${d}T12:00:00Z`)))),
    h('input', { type: 'date', value: date, max: today, 'aria-label': 'Date',
      onchange: e => e.target.value && go({ d: e.target.value }) }));

  const slotSelect = h('select', { id: 'stats-slot', 'aria-label': 'Slot', onchange: e => go({ s: e.target.value }) },
    ...FOOD_SLOTS.map(n => h('option', { value: String(n), selected: n === slot }, `Slot ${n}`)));
  // `sel` EXPLICIT while rendering: the page re-renders every poll, and looking it
  // up by id then found the OLD select still on screen -- labelled it, and the new
  // unlabelled one replaced it a moment later. The async fill uses the live one.
  function refreshSlotNames(rows, sel = document.getElementById('stats-slot') || slotSelect) {
    for (const o of sel.options) {
      const r = rows.find(x => x.food_slot === Number(o.value));
      o.textContent = r && r.food_name ? `${o.value} · ${r.food_name}` : `Slot ${o.value}`;
    }
  }
  if (rankRows) refreshSlotNames(rankRows, slotSelect);

  frag.append(h('div', { class: 'section' },
    dayChips,
    h('div', { class: 'toolbar' },
      ...MEAL_TYPES.map(m => h('button', {
        class: 'ghost', 'aria-pressed': String(m === meal), onclick: () => go({ m }),
      }, m)),
      h('div', { class: 'grow' }),
      slotSelect)));

  // --- the meal: food on hand + the figures ------------------------------------
  const seriesKey = `series|${date}|${meal}|${slot}`;
  const mealKey = `meal|${date}|${meal}|${slot}`;
  const chartBox = h('div', { id: 'stats-chart' });
  const cardsBox = h('div', { id: 'stats-cards' });

  const renderChart = rows => {
    if (!rows.length) return empty('No platform on this slot reported during this meal.');
    const byT = new Map();
    for (const r of rows) {
      const t = Date.parse(r.at_ts);
      if (!byT.has(t)) byT.set(t, []);
      byT.get(t).push(r);
    }
    const ts = [...byT.keys()].sort((a, b) => a - b);
    const total = ts.map(t => {
      const vals = byT.get(t).map(r => r.total_g).filter(v => v != null).map(Number);
      return { t, v: vals.length ? vals.reduce((a, b) => a + b, 0) : null };
    });
    const areas = SERVING_LOCATIONS.filter(a => rows.some(r => r.location === a));
    const series = [{ points: total, label: areas.length > 1 ? 'D+M+T' : 'Total', color: 'var(--ink)', width: 3 }];
    if (areas.length > 1) {
      for (const a of areas) {
        series.push({
          points: ts.map(t => {
            const r = byT.get(t).find(x => x.location === a);
            return { t, v: r && r.total_g != null ? Number(r.total_g) : null };
          }),
          label: LOCATION_NAMES[a], color: 'var(--area-accent)', cls: `area-${a}`, width: 1.5,
        });
      }
    }
    const width = Math.max(300, (document.getElementById('view')?.clientWidth || 640) - 34);
    return weightChart({ series, now: ts[ts.length - 1], width, tz, height: 280 });
  };

  const card = (label, value, sub) => h('div', { class: 'card stats-card' },
    h('div', { class: 'dim' }, label), h('div', { class: 'stats-value' }, value),
    sub ? h('div', { class: 'dim stats-sub' }, sub) : null);

  const renderCards = rows => {
    const r = rows[0];
    if (!r) return null;
    const out = r.ran_out_at
      ? card('Ran out', fmtClock(r.ran_out_at, tz), r.short_before_close ? 'before the meal closed' : 'at the close')
      : card('Lasted', 'the whole meal', `${kg(r.end_g)} left`);
    return h('div', {},
      h('div', { class: 'stats-cards' },
        card('Eaten', kg(r.consumed_g), `over ${r.covered_min} min`),
        card('Average', kgh(r.g_per_hour)),
        card('Rush', kgh(r.rush_g_per_hour),
          r.rush_at ? `${fmtClock(new Date(Date.parse(r.rush_at) - 15 * 60_000).toISOString(), tz)}–${fmtClock(r.rush_at, tz)}` : null),
        card('Delivered', kg(r.delivered_g), 'by the kitchen'),
        card('On hand', `${kg(r.start_g)} → ${kg(r.end_g)}`, 'start → end'),
        out),
      h('div', { class: 'dim stats-sub' },
        `Measured in ${(r.areas || []).map(a => LOCATION_NAMES[a] || a).join(', ') || '—'}.`),
      r.partial ? banner('warning', '!', 'Some readings were missing during this meal — the figures may read low.') : null);
  };

  const seriesRows = fetchOnce(ctx.client, seriesKey, 'slot_meal_series',
    { p_date: date, p_meal: meal, p_slot: slot },
    rows => fillSlot('stats-chart', chartBox, renderChart(rows)),
    err => fillSlot('stats-chart', chartBox, banner('critical', '✕', describeError(err))));
  chartBox.append(seriesRows ? renderChart(seriesRows) : empty('Loading…'));

  const mealRows = fetchOnce(ctx.client, mealKey, 'slot_meal_stats',
    { p_from: date, p_to: date, p_meal: meal, p_slot: slot },
    rows => fillSlot('stats-cards', cardsBox, renderCards(rows)),
    err => fillSlot('stats-cards', cardsBox, banner('critical', '✕', describeError(err))));
  if (mealRows) cardsBox.append(renderCards(mealRows));

  frag.append(h('div', { class: 'section' },
    h('div', { class: 'section-head' },
      h('h2', { id: 'stats-title' }, `Slot ${slot}${dishOf(rankRows) ? ` · ${dishOf(rankRows)}` : ''}`),
      h('span', { class: 'count' }, `${meal}, ${fmtDay(date)} — food on hand`)),
    chartBox, cardsBox));

  // --- day by day ------------------------------------------------------------------
  const from = addDays(date, -(days - 1));
  const rangeKey = `range|${from}|${date}|${meal}|${slot}`;
  const rangeBox = h('div', { id: 'stats-range' });
  const renderRange = rows => {
    if (!rows.length) return empty('Nothing weighed on this slot in this range.');
    const max = Math.max(...rows.map(r => Number(r.consumed_g) || 0), 1);
    return h('div', { class: 'stats-bars' }, ...[...rows].reverse().map(r => h('a', {
      class: `stats-bar-row${r.meal_date === date ? ' is-target' : ''}`,
      href: `#/statistics?${new URLSearchParams({ d: r.meal_date, m: meal, s: String(slot), r: String(days) })}`,
    },
      h('span', { class: 'stats-day' }, new Intl.DateTimeFormat('en-GB', { weekday: 'short', day: '2-digit' })
        .format(new Date(`${r.meal_date}T12:00:00Z`))),
      h('span', { class: 'stats-bar' }, h('i', { style: `width:${(100 * (Number(r.consumed_g) || 0) / max).toFixed(1)}%` })),
      h('b', {}, kg(r.consumed_g)),
      h('span', { class: 'dim' }, kgh(r.g_per_hour)),
      r.short_before_close ? h('span', { class: 'stats-out' }, `out ${fmtClock(r.ran_out_at, tz)}`) : null)));
  };
  const rangeRows = fetchOnce(ctx.client, rangeKey, 'slot_meal_stats',
    { p_from: from, p_to: date, p_meal: meal, p_slot: slot },
    rows => fillSlot('stats-range', rangeBox, renderRange(rows)),
    err => fillSlot('stats-range', rangeBox, banner('critical', '✕', describeError(err))));
  rangeBox.append(rangeRows ? renderRange(rangeRows) : empty('Loading…'));

  frag.append(h('div', { class: 'section' },
    h('div', { class: 'section-head' },
      h('h2', {}, 'Day by day'),
      h('span', { class: 'count' }, `${meal}, kg eaten`)),
    h('div', { class: 'toolbar' },
      ...[7, 30].map(n => h('button', {
        class: 'ghost', 'aria-pressed': String(n === days), onclick: () => go({ r: String(n) }),
      }, `Last ${n} days`))),
    rangeBox));

  frag.append(h('div', { class: 'section' },
    h('div', { class: 'section-head' },
      h('h2', {}, 'Draining fastest'),
      h('span', { class: 'count' }, `${meal}, ${fmtDay(date)} — every weighed slot`)),
    rankBox));

  return frag;
}
