// ====================================================================
//  Stock — the primary screen.
//
//  Reads slot_overview, NOT device_overview: several stacks serve one dish
//  position (Darshanarthi runs three), so remaining stock for a dish is the
//  sum across them. The view does that sum, and the trust rules that go with
//  it; nothing is re-aggregated here.
//
//  device_overview is read alongside for ATTRIBUTION only — naming the stack
//  that is faulty or silent, which is what turns "something is wrong at slot 3"
//  into an instruction someone can act on.
// ====================================================================

import { h, badge, empty, banner } from '../ui.js';
import {
  LOCATION_NAMES, SERVING_LOCATIONS, slotStock, stockSeverity, deviceStack,
  fmtClock, fmtRelative, serviceState,
} from '../domain.js';

export function renderStock(state) {
  const frag = document.createDocumentFragment();
  const { slots, devices } = state;
  const { inService, meal, tz } = serviceState(devices);

  const byPosition = new Map();
  for (const d of devices) {
    if (d.location == null || d.food_slot == null) continue;
    const k = `${d.location}|${d.food_slot}`;
    if (!byPosition.has(k)) byPosition.set(k, []);
    byPosition.get(k).push(d);
  }

  if (!inService) {
    frag.append(banner('info', '◷',
      h('b', {}, 'Outside service hours. '),
      'These are last-known values from the previous service, not live readings. ',
      'Devices are powered only during meal service.'));
  } else if (meal) {
    frag.append(banner('info', '●', h('b', {}, `${meal} service is on. `),
      'Counts refresh automatically.'));
  }

  const serving = slots.filter(sl => SERVING_LOCATIONS.includes(sl.location));
  if (!serving.length) {
    frag.append(empty('No dish positions are configured. Assign devices on the Devices tab.'));
    return frag;
  }

  for (const loc of SERVING_LOCATIONS) {
    const rows = serving
      .filter(sl => sl.location === loc)
      .sort((a, b) => a.food_slot - b.food_slot);
    if (!rows.length) continue;

    const totalTrusted = rows.reduce((n, r) => n + (r.bowls_trusted == null ? 0 : Number(r.bowls_trusted)), 0);
    const totalCap = rows.reduce((n, r) => n + Number(r.bowls_capacity || 0), 0);

    frag.append(h('div', { class: 'area-head' },
      h('h2', {}, LOCATION_NAMES[loc] || loc),
      h('span', { class: 'dim' }, `${totalTrusted} of ${totalCap} bowls across ${rows.length} positions`)));

    const grid = h('div', { class: 'slot-grid' });
    for (const sl of rows) grid.append(slotCard(sl, byPosition.get(`${loc}|${sl.food_slot}`) || [], inService, tz));
    frag.append(grid);
  }

  return frag;
}

function slotCard(sl, stacks, inService, tz) {
  const stock = slotStock(sl);
  const okStacks = stacks.filter(d => d.stack_status === 'ok' && d.reported).length;
  const severity = stock.kind === 'count' ? stockSeverity(stock.trusted, okStacks) : stock.severity;

  const card = h('div', { class: 'slot' });

  card.append(h('div', { class: 'slot-top' },
    h('span', { class: 'slot-pos' }, `Slot ${sl.food_slot}`)));

  card.append(sl.current_food
    ? h('div', { class: 'slot-food' }, sl.current_food)
    : h('div', { class: 'slot-food unset' },
        inService ? 'No menu entered' : 'No current dish'));

  // The headline. A discontiguous stack anywhere in this position means the
  // total is not a number to act on, so it is not rendered as one.
  if (stock.kind === 'fault') {
    card.append(h('div', { class: 'slot-figure' },
      h('span', { class: 'slot-fault' }, '▲ Check station')));
  } else if (stock.kind === 'nodata') {
    card.append(h('div', { class: 'slot-figure' },
      h('span', { class: 'slot-nodata' }, 'No data')));
  } else {
    card.append(h('div', { class: 'slot-figure' },
      h('span', { class: `slot-count${severity === 'critical' ? ' is-critical' : ''}` },
        sl.any_degraded ? `≥${stock.trusted}` : String(stock.trusted)),
      h('span', { class: 'slot-of' }, `of ${stock.capacity} bowls`)));
  }

  const pct = stock.capacity && stock.trusted != null
    ? Math.max(0, Math.min(100, (stock.trusted / stock.capacity) * 100)) : 0;
  card.append(h('div', { class: severity === 'idle' ? 'meter' : `meter is-${severity}` },
    h('i', { style: `width:${pct}%` })));

  const notes = h('div', { class: 'slot-notes' });
  if (sl.any_fault) {
    notes.append(badge('critical', '▲', 'Impossible reading'));
    if (stock.trusted != null) {
      card.append(h('div', { class: 'slot-sub' },
        `${stock.trusted} bowls on the ${okStacks} stack${okStacks === 1 ? '' : 's'} still reading correctly.`));
    }
  }
  if (sl.any_degraded) notes.append(badge('warning', '◐', 'Lower bound'));
  if (sl.any_offline) notes.append(badge('critical', '✕', 'Stack offline'));
  if (sl.any_battery_warn) notes.append(badge('warning', '▮', 'Battery'));
  if (Number(sl.devices_reported) < Number(sl.devices)) {
    notes.append(badge('warning', '◔',
      `${Number(sl.devices) - Number(sl.devices_reported)} of ${sl.devices} not reporting`));
  }
  if (notes.childElementCount) card.append(notes);

  if (!inService && sl.oldest_update) {
    card.append(h('div', { class: 'slot-sub dim' }, `As of ${fmtClock(sl.oldest_update, tz)}`));
  }

  // Which stack is which. With at most three per position this fits, and it is
  // the difference between "slot 3 is wrong" and "tell someone to look at
  // BWL-008".
  if (stacks.length) {
    const pills = h('div', { class: 'slot-notes' });
    for (const d of stacks.sort((a, b) => a.device_id.localeCompare(b.device_id))) {
      const st = deviceStack(d);
      const cls = d.offline ? 'critical'
                : st.kind === 'fault' ? 'critical'
                : st.kind === 'bound' ? 'warning'
                : st.kind === 'none' ? 'idle' : 'good';
      const glyph = d.offline ? '✕'
                  : st.kind === 'fault' ? '▲'
                  : st.kind === 'bound' ? '◐'
                  : st.kind === 'none' ? '◌' : '●';
      pills.append(h('a', {
        class: `badge is-${cls}`,
        href: `#/device/${encodeURIComponent(d.device_id)}`,
        style: 'text-decoration:none',
        title: `${d.device_id} — ${d.offline ? 'offline' : st.note || 'reading OK'} · updated ${fmtRelative(d.updated_at)}`,
      },
        h('span', { class: 'g', 'aria-hidden': 'true' }, glyph),
        `${d.device_id.replace(/^BWL-/, '')} · ${d.offline ? 'offline' : st.text}`));
    }
    card.append(pills);
  }

  return card;
}
