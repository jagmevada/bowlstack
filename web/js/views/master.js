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
  for (const row of rows) grid.append(slotCard(row, state.devices, inService, tz, meal));
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

function slotCard(row, devices, inService, tz, meal) {
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
  }
  head.append(fig);
  card.append(head);

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

  // NULL is not zero, here as everywhere: a hall with no per-bowl weight says
  // what it is missing rather than printing "0.0 kg", which would read as an
  // empty counter and send someone to refill a full one.
  line.append(grams == null
    ? h('span', { class: 'marea-kg unset' },
        a.bowl_weight_g == null ? 'no weight set' : '—')
    : h('span', { class: `marea-kg${glyph ? ' is-alert' : ''}` }, fmtWeight(grams)));

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
