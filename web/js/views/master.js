// ====================================================================
//  Master — quantity per dish position, broken down by serving area.
//
//  Reads slot_quantity, which groups by food_slot ACROSS every area. That
//  is the difference between this screen and Stock: Stock answers "how much
//  rice is left at Darshanarthi position 1" — one hall, in bowls — and
//  Master answers "how much dal is left, and in which hall" — every hall,
//  in kilograms.
//
//  THERE IS NO SITE-WIDE TOTAL, DELIBERATELY.
//  An earlier cut showed one big number at the top: rice + dal + curry +
//  roti added together. That figure is arithmetically fine and
//  operationally meaningless — nobody cooks, orders or refills "212 kg of
//  food" — and a number nobody can act on at the top of a screen teaches
//  people to skip the top of the screen. Quantity is only meaningful per
//  dish, so the totals stop at the slot.
//
//  Nothing here re-sums anything. The view weighed each hall against ITS
//  OWN dish before adding up, which is what keeps a slot correct on the
//  days Darshanarthi serves Curry while Tiffin serves Bhaji.
// ====================================================================

import { h, empty, banner } from '../ui.js';
import {
  LOCATION_NAMES, SERVING_LOCATIONS,
  slotQuantity, fmtWeight,
  deviceOffline, slotOffline, fmtClock, fmtDay, serviceState, mealAtLocalClock,
} from '../domain.js';

export function renderMaster(state) {
  const frag = document.createDocumentFragment();
  const rows = [...(state.quantity || [])].sort((a, b) => a.food_slot - b.food_slot);
  const { inService, meal, tz } = serviceState(state.devices);

  // The database has not been migrated yet. Say THAT, rather than the empty
  // state below: the two look identical on screen and send someone to
  // opposite places — one to the Supabase SQL editor, the other out to the
  // hall to check stations that are working fine.
  if (state.quantityError) {
    frag.append(banner('warning', '◆',
      h('b', {}, 'Master needs one migration. '),
      'This database has no ', h('code', {}, 'slot_quantity'), ' view yet. Run ',
      h('code', {}, 'supabase/migrate_bowl_weight.sql'),
      ' in the Supabase SQL editor, then reload — it adds a column and a view, '
      + 'drops nothing, and is safe to run mid-service. Every other tab keeps '
      + 'working meanwhile.'));
    frag.append(h('div', { class: 'mt-hint' }, state.quantityError));
    return frag;
  }

  if (!rows.length) {
    frag.append(empty(
      'No dish position has reported yet. Master lists a slot once one of its '
      + 'devices has spoken at least once — assign and power a station on the '
      + 'Devices tab.'));
    return frag;
  }

  frag.append(mealStrip(rows));

  const grid = h('div', { class: 'master-rows' });
  // Keyed by slot so a card can find its own rate without a second loop, and
  // absent entirely on a database without migrate_burn_rate.sql -- in which
  // case every card simply says nothing about consumption.
  //
  // ONE ROW PER HALL PER SLOT NOW, so this cannot be a plain Map(...) over the
  // list any more -- that kept whichever hall happened to come last. slot 3 is
  // a different dish in each hall, and the view used to sum all three into a
  // single curve and a single runs_out_at; keying it by (location, food_slot)
  // is what fixed that, and the cost is that the card has to choose.
  //
  // IT CHOOSES THE EARLIEST DEADLINE, because the question the card answers is
  // "does this need a second production run", and the hall that empties first
  // is the one that decides. A row with no runs_out_at loses to one that has
  // it; if none has one, the first is kept so g_per_hour and is_partial still
  // render.
  const burn = new Map();
  for (const b of (state.burn || [])) {
    const k = Number(b.food_slot);
    const prev = burn.get(k);
    if (!prev) { burn.set(k, b); continue; }
    if (b.runs_out_at == null) continue;
    if (prev.runs_out_at == null ||
        new Date(b.runs_out_at) < new Date(prev.runs_out_at)) burn.set(k, b);
  }
  // Grouped once here rather than filtered per card: the series is one row per
  // slot per five minutes, so filtering inside the loop is quadratic over a
  // list that grows with both the fleet and the window.
  const series = new Map();
  for (const p of (state.series || [])) {
    if (p.total_g == null) continue;
    const k = Number(p.food_slot);
    if (!series.has(k)) series.set(k, []);
    series.get(k).push({ t: new Date(p.at_ts).getTime(), v: Number(p.total_g) });
  }
  for (const row of rows)
    grid.append(slotCard(row, state.devices, inService, tz, meal,
                         burn.get(Number(row.food_slot)),
                         series.get(Number(row.food_slot))));
  frag.append(grid);

  frag.append(h('div', { class: 'legend-line' },
    h('span', {}, h('b', {}, '≥'), ' a lower bound — some bowls have no weight set'),
    h('span', {}, h('b', { class: 'st-fault' }, '▲'), ' fault'),
    h('span', {}, h('b', { class: 'st-off' }, '✕'), ' offline'),
    h('span', {}, h('b', { class: 'st-deg' }, '◐'), ' degraded')));

  return frag;
}

/**
 * Which meal these figures describe, and how live they are.
 *
 * This is the header now that the site-wide total is gone, and it earns the
 * space: every number below belongs to ONE meal, and between meals that is
 * the meal that just FINISHED. Unsaid, last night's dinner reads as tonight's.
 */
function mealStrip(rows) {
  const r = rows[0] || {};
  if (r.menu_is_live) {
    return banner('info', '●',
      h('b', {}, `${r.menu_meal_type || 'Service'} service is on. `),
      'Quantities refresh automatically.');
  }
  if (r.menu_meal_type) {
    return banner('info', '◷',
      h('b', {}, `From ${r.menu_meal_type} · ${fmtDay(r.menu_meal_date)} — last known. `),
      'Devices are powered only during meal service, so these are the counts '
      + 'from the end of that service, not live readings.');
  }
  return banner('info', '◷',
    h('b', {}, 'No menu for this meal. '),
    'Bowls are counted, but there is no dish to weigh them against — enter one '
    + 'on the Menu tab.');
}

/** Is this slot's figure something to act on, or something to check first?
 *  Red means exactly one thing across this whole app: the number is
 *  compromised. Never "nearly empty" — quantity is the number's own job. */
function compromised(row) {
  return slotOffline(row) || !!row.any_degraded || !!row.any_fault;
}

/** What is wrong at one hall's share of one dish position. This breakdown is
 *  what turns "slot 3 is compromised" into an instruction: a refill, or a
 *  repair, is dispatched to a HALL, never to a slot number. */
function areaHealth(devices, loc, slot) {
  const mine = devices.filter(d => d.location === loc
    && Number(d.food_slot) === Number(slot)
    && !d.awaiting_deployment);
  return {
    fault: mine.some(d => d.stack_status === 'discontiguous'),
    offline: mine.some(deviceOffline),
    degraded: mine.some(d => d.stack_status === 'degraded'),
  };
}

// --- one dish position ------------------------------------------------

function slotCard(row, devices, inService, tz, meal, rate, series) {
  const q = slotQuantity(row);
  const alert = compromised(row);
  const dishes = row.dishes || [];
  const areas = row.areas || [];

  const card = h('div', { class: `card mslot${alert ? ' is-alert' : ''}` });

  // The slot header: dish on the left, this slot's own total on the right.
  const head = h('div', { class: 'mslot-head' });
  head.append(h('div', { class: 'mslot-id' },
    h('span', { class: 'mrow-slot' }, `Slot ${row.food_slot}`),
    // Several dish names at one position is NOT an error — the halls
    // genuinely diverge — so they are listed, never flagged. The total
    // behind them is still exact: each hall was weighed against its own dish
    // before the sum.
    dishes.length
      ? h('div', { class: 'mrow-dish', title: dishes.join(' / ') }, dishes.join(' / '))
      : h('div', { class: 'mrow-dish unset' },
          inService ? 'No menu entered' : 'No current dish')));

  const fig = h('div', { class: 'mrow-figure' });
  if (q.kind === 'nodata') {
    fig.append(h('span', { class: 'slot-nodata' }, 'No data'));
  } else if (q.kind === 'noweight') {
    fig.append(
      h('span', { class: 'mrow-kg is-unset' }, q.headline),
      h('a', { class: 'mrow-fix', href: weightLink(row, meal) }, 'Set weight ›'));
  } else {
    // A faulted position still states its figure, in red — slotQuantity()
    // decides that, and the ▲ on the offending hall's line below says where
    // the problem is.
    fig.append(h('span', {
      class: `mrow-kg${alert ? ' is-alert' : ''}${q.partial ? ' is-bound' : ''}`,
      title: q.note || (alert
        ? 'A stack at this position is not reporting valid data — this figure '
          + 'is what the healthy stacks hold.'
        : undefined),
    }, q.headline));
    if (q.trusted != null) {
      fig.append(h('span', { class: 'mslot-of' }, `${q.trusted} of ${q.capacity} bowls`));
    }
    if (q.partial) {
      fig.append(h('a', {
        class: 'mrow-fix', href: weightLink(row, meal), title: q.note,
      }, 'Set weight ›'));
    }

    // NEITHER A SOURCE CHIP NOR A MISMATCH WARNING, and both were removed for
    // the same reason: they treated the buffer and the counter as rival
    // answers. They are different food in different places, which the view now
    // ADDS -- so there is no source to name and no disagreement to flag. What
    // the card shows instead is

  }

  head.append(fig);
  card.append(head);

  // --- consumption ---------------------------------------------------
  // THE LINE THAT STARTS A SECOND PRODUCTION RUN. Everything above says how
  // much is left; this says how fast it is going and when it runs out, which
  // is the only pair of facts you can act on while there is still time to
  // cook.
  //
  // Absent when there is no rate rather than rendered as zero -- a station
  // that has been reporting for four minutes has not told anybody it is
  // consuming nothing.
  if (rate && rate.g_per_hour != null) {
    card.append(burnLine(rate, tz, series));
  }

  // One line per serving area, in a fixed order so the eye can run down the
  // page and compare the same hall across every slot.
  const list = h('div', { class: 'marea-list' });
  for (const loc of SERVING_LOCATIONS) {
    const a = areas.find(x => x.location === loc);
    if (!a) continue;                     // this hall has no stack at this slot
    list.append(areaLine(a, areaHealth(devices, loc, row.food_slot), dishes.length > 1));
  }
  if (list.childNodes.length) card.append(list);

  if (!inService && row.oldest_update) {
    card.append(h('div', { class: 'mrow-areas dim' },
      `As of ${fmtClock(row.oldest_update, tz)}`));
  }
  return card;
}

/** One hall's share of one dish position: name, bowls, mass. */
// Says how a hall's kilograms were arrived at, in the terms the view actually
// publishes: est_weight_g is the buffered bowls, measured_weight_g is what its
// scales weigh, and the figure on screen is their sum.
export function areaWeightNote(a, bowls) {
  const kg = g => (Number(g) / 1000).toFixed(1);
  const buf = a.est_weight_g, ctr = a.measured_weight_g;
  const bowlPart = a.bowl_weight_g != null
    ? `${bowls == null ? '—' : bowls} bowls x ${kg(a.bowl_weight_g)} kg, from the Menu tab`
    : 'buffered bowls';
  const scalePart = `weighed at the counter${a.scales > 1 ? ` by ${a.scales} scales` : ''}`;
  if (buf != null && ctr != null) {
    return `${kg(buf)} kg buffered (${bowlPart}) + ${kg(ctr)} kg ${scalePart}.`;
  }
  if (ctr != null) return `${kg(ctr)} kg, ${scalePart}.`;
  if (buf != null) return `${bowlPart}.`;
  return undefined;
}

function areaLine(a, health, showDish) {
  const bowls = a.bowls_trusted == null ? null : Number(a.bowls_trusted);
  const grams = a.weight_g == null ? null : Number(a.weight_g);

  // Priority mirrors severity everywhere else in this app: an impossible
  // reading outranks silence outranks a dead sensor.
  const glyph = health.fault
      ? { cls: 'fault', g: '▲', word: 'impossible reading — check the sensors' }
    : health.offline
      ? { cls: 'off', g: '✕', word: 'offline — this is its last known value' }
    : health.degraded
      ? { cls: 'deg', g: '◐', word: 'a sensor is down — the count is a lower bound' }
    : null;

  const line = h('div', { class: `marea${glyph ? ' is-alert' : ''}` });

  // EXACTLY three children, always: name, bowls, mass. An earlier cut emitted
  // an empty placeholder span when there was no dish to show — four children
  // in a three-column grid — so the kilograms wrapped onto a line of their own
  // and every hall cost two rows instead of one.
  //
  // The dish therefore lives INSIDE the name cell rather than in a column of
  // its own, which also reads better: it belongs to the hall above it.
  line.append(h('div', { class: 'marea-name' },
    h('span', { class: 'marea-hall' },
      LOCATION_NAMES[a.location] || a.location,
      glyph ? h('span', { class: `st st-${glyph.cls}`, title: glyph.word }, ` ${glyph.g}`) : null),
    // Only when the halls disagree — repeating "Roti" three times under a
    // heading that already says Roti is noise.
    showDish && a.food_name
      ? h('span', { class: 'marea-dish dim', title: a.food_name }, a.food_name)
      : null));

  // "5/12", not "5 of 12 bowls". Four cards abreast leaves a hall line about
  // 250px wide, and the verbose form wraps there — a wrapped line is taller
  // than the row it saved. The slot header above already says "of 20 bowls"
  // in full, so the context is not lost, and the tooltip spells it out.
  line.append(h('span', {
    class: 'marea-bowls',
    title: bowls == null ? 'No stack here has reported'
      : `${bowls} of ${a.bowls_capacity} bowls`,
  }, bowls == null ? '—' : `${bowls}/${a.bowls_capacity}`));

  // NULL is not zero, here as everywhere: a hall with nothing to weigh with
  // says what it is missing rather than printing "0.0 kg", which would read as
  // an empty counter and send someone to refill a full one.
  //
  // A weighed hall is NOT marked in the row itself: four cards abreast leaves
  // ~250 px here, and every glyph added is a wrap risk on a row already tuned
  // to fit in one. The tooltip carries the arithmetic instead, which is where
  // somebody who wants to know goes anyway.
  // THREE REASONS A HALL HAS NO KILOGRAMS, and they are not the same fact.
  //
  //   no dish        this hall is not serving this meal -- breakfast runs at
  //                  Darshanarthi only, and Mahatma is not missing anything
  //   no weight set  it IS serving, and nobody has typed a kg/bowl. This is
  //                  the one worth prompting about
  //   no reading     the dish and the weight are both set; the stacks are
  //                  silent
  //
  // The first used to render as "no weight set", which asked somebody to go
  // and configure a dish that does not exist at that hall.
  line.append(grams == null
    ? h('span', {
        class: 'marea-kg unset',
        title: a.food_name == null
          ? 'This hall is not serving at this position for the current meal.'
          : a.bowl_weight_g == null
            ? 'Serving, but no kg per bowl has been entered on the Menu tab.'
            : 'No stack here has reported.',
      }, a.food_name == null ? 'no dish'
         : a.bowl_weight_g == null ? 'no weight set' : '—')
    : h('span', {
        class: `marea-kg${glyph ? ' is-alert' : ''}`,
        // THE TOOLTIP HAS TO SHOW THE SUM, because the figure it is attached
        // to is one. It read `a.weight_source === 'measured' ? ... : ...`,
        // and weight_source was deleted from the view along with the
        // precedence rule -- so against the live API it was always undefined
        // and always took the buffer branch, printing "2 bowls x 18.0 kg"
        // beside a figure of 54.0 kg that included 18 kg weighed at the
        // counter. The tooltip contradicted its own number. Against mock.js,
        // which still emitted the field, it took the other branch and was
        // wrong differently -- the same one-source-two-answers split
        // CLAUDE.md warns about.
        title: areaWeightNote(a, bowls),
      }, fmtWeight(grams)));

  return line;
}

/** Deep-link to the Menu tab, narrowed to the halls that still need a weight
 *  and the meal on screen, so the fix is one tap rather than a search. */
function weightLink(row, meal) {
  const locs = (row.areas_without_weight && row.areas_without_weight.length)
    ? row.areas_without_weight
    : (row.areas || []).map(a => a.location);
  const q = new URLSearchParams({
    locs: [...new Set(locs)].join(',') || SERVING_LOCATIONS.join(','),
    meal: row.menu_meal_type || row.current_meal || meal || mealAtLocalClock(),
  });
  return `#/menu?${q}`;
}

/**
 * The history, and where it is heading — the battery-screen shape.
 *
 * NO RATE, NO WARNING COLOUR, NO CHIP. An earlier cut put "38.60 kg/min ·
 * runs out 00:17 · at the earliest" on every card, in amber. Three
 * problems with that, and only the first was a bug:
 *
 *   * The number was wrong by a thousand — grams per minute wearing a kg
 *     label. Fixed, but it should never have been the headline.
 *   * A rate is working, not an answer. Nobody schedules a production run
 *     from kg/min; they schedule it from "this is empty at 00:17".
 *   * Colour-coding urgency on a card that already carries fault and
 *     offline colour gives the eye three competing alarms. A phone does
 *     not turn its battery graph red at 20%; it draws the curve and says
 *     when it runs out, and the reader decides.
 *
 * So: the curve, and one quiet line under it. Everything else moved to the
 * tooltip, where somebody reconciling against a delivery note can find it.
 */
function burnLine(rate, tz, series) {
  const outAt = rate.runs_out_at ? new Date(rate.runs_out_at) : null;
  const minsLeft = outAt ? Math.round((outAt - Date.now()) / 60000) : null;

  // Grams per hour to kilograms per hour: one division, at the point of
  // display, exactly as every other weight on this screen.
  const kgPerHour = Number(rate.g_per_hour) / 1000;

  const bits = [];
  if (outAt && minsLeft != null && minsLeft >= 0) {
    bits.push(h('span', { class: 'mburn-out' },
      // The hall is named because the deadline is now ONE hall's, not the
      // slot's: without it "Empty about 19:30" on a slot served in three halls
      // reads as a claim about all three.
      `Empty about ${fmtClock(rate.runs_out_at, tz)}`
        + (rate.location ? `, ${LOCATION_NAMES[rate.location] || rate.location}` : '')));
    bits.push(h('span', { class: 'dim' }, humanLeft(minsLeft)));
  } else {
    // A rate with no projection means the stock is not falling. Say that
    // rather than leaving the line blank, which reads as missing data.
    bits.push(h('span', { class: 'dim' }, 'Not falling'));
  }

  const line = h('div', {
    class: 'mburn',
    title: `${kgPerHour.toFixed(1)} kg/h over the last ${rate.covered || 'hour'}`
         + `, from ${((rate.total_g ?? 0) / 1000).toFixed(1)} kg on hand `
         + `(${((rate.buffer_g ?? 0) / 1000).toFixed(1)} buffered `
         + `+ ${((rate.counter_g ?? 0) / 1000).toFixed(1)} on the counter)`
         + (rate.is_partial
             ? '. Some hall’s buffer has no per-bowl weight, so this is the '
               + 'earliest it could run out, not the likeliest.'
             : ''),
  }, ...bits);

  if (series && series.length > 1) {
    const wrap = h('div', { class: 'mburn-wrap' });
    wrap.append(sparkline(series));
    wrap.append(line);
    return wrap;
  }
  return line;
}

/** "· 1 h 38 m left". Hours and minutes rather than a raw minute count,
 *  because past about ninety minutes "94 min" stops being a duration anyone
 *  reads at a glance. */
function humanLeft(mins) {
  if (mins < 60) return ` · ${mins} min left`;
  const hh = Math.floor(mins / 60), mm = mins % 60;
  return mm ? ` · ${hh} h ${mm} m left` : ` · ${hh} h left`;
}

/**
 * Total stock over the window, drawn small.
 *
 * NO AXES, NO TICKS, NO LABELS. It sits under a figure that already states
 * the current value and beside a sentence that states the rate, so anything
 * it repeated would be clutter -- its whole job is the SHAPE. The card's
 * tooltip carries the numbers.
 *
 * Scaled from zero rather than from the minimum. A curve auto-scaled to its
 * own range makes a counter that drifted 200 g look exactly like one that
 * emptied, which on a stock screen is the one misreading that matters.
 */
function sparkline(pts) {
  const W = 168, H = 34, PAD = 2;
  const vals = pts.map(p => p.v);
  const top = Math.max(...vals, 1);
  const t0 = pts[0].t, t1 = pts[pts.length - 1].t;
  const span = Math.max(1, t1 - t0);
  const x = t => PAD + ((t - t0) / span) * (W - PAD * 2);
  const y = v => PAD + (1 - v / top) * (H - PAD * 2);

  const NS = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  svg.setAttribute('width', String(W));
  svg.setAttribute('height', String(H));
  svg.setAttribute('class', 'mspark');
  svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label',
    `Stock over the last hour, ${(vals[0] / 1000).toFixed(1)} to `
    + `${(vals[vals.length - 1] / 1000).toFixed(1)} kg`);

  const area = document.createElementNS(NS, 'path');
  area.setAttribute('class', 'mspark-fill');
  area.setAttribute('d',
    `M${x(t0)},${H - PAD}`
    + pts.map(p => `L${x(p.t)},${y(p.v)}`).join('')
    + `L${x(t1)},${H - PAD}Z`);
  svg.append(area);

  const line = document.createElementNS(NS, 'path');
  line.setAttribute('class', 'mspark-line');
  line.setAttribute('d', pts.map((p, i) => `${i ? 'L' : 'M'}${x(p.t)},${y(p.v)}`).join(''));
  svg.append(line);

  const dot = document.createElementNS(NS, 'circle');
  dot.setAttribute('class', 'mspark-dot');
  dot.setAttribute('cx', String(x(t1)));
  dot.setAttribute('cy', String(y(vals[vals.length - 1])));
  dot.setAttribute('r', '2.4');
  svg.append(dot);

  return svg;
}
