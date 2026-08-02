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

import { h, empty, banner, batteryBar } from '../ui.js';
import {
  LOCATION_NAMES, SERVING_LOCATIONS, MAX_BOWLS, slotStock, deviceStack,
  deviceOffline, slotOffline, fleetSummary, weekdayOf, serviceDate,
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

  // The problem strip: one thin red bar that says "something needs a person"
  // and takes them to Health to see what. Stock is the screen watched through
  // service, so it carries the pointer, not the diagnosis — counts only, no
  // wall of device IDs. Every count is a server-computed flag (deviceOffline
  // wraps the two offline flags and nothing else), so this cannot false-alarm
  // on ordinary dark hours.
  const s = fleetSummary(devices);
  const issues = [];
  if (s.offline) issues.push(['✕', `${s.offline} offline`]);
  if (s.fault) issues.push(['▲', `${s.fault} sensor fault${s.fault === 1 ? '' : 's'}`]);
  if (s.degraded) issues.push(['◐', `${s.degraded} degraded`]);
  if (s.batteryWarn) issues.push(['▮', `${s.batteryWarn} battery`]);
  if (issues.length) {
    frag.append(h('button', {
      class: 'alert-strip',
      title: 'Open the Health page to see which devices and why',
      onclick: () => { location.hash = '#/health?f=problems'; },
    },
      ...issues.flatMap(([g, label]) => [
        h('span', { class: 'g', 'aria-hidden': 'true' }, g),
        h('span', {}, label),
      ]),
      h('span', { class: 'go' }, 'Diagnose in Health ›')));
  }

  // The one place symbols meet words. The cards below are glyph-only —
  // minimal by request — so the legend carries the pairing the
  // colour-never-alone rule demands, once per page instead of per chip.
  frag.append(h('div', { class: 'legend-line' },
    h('span', {}, h('b', { class: 'st-ok' }, '●'), ' reporting'),
    h('span', {}, h('b', { class: 'st-off' }, '✕'), ' offline'),
    h('span', {}, h('b', { class: 'st-fault' }, '▲'), ' fault'),
    h('span', {}, h('b', { class: 'st-deg' }, '◐'), ' degraded'),
    h('span', {}, h('b', { class: 'st-none' }, '◌'), ' no reading')));

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
    for (const sl of rows) {
      grid.append(slotCard(sl, byPosition.get(`${loc}|${sl.food_slot}`) || [],
        inService, tz, state.template || []));
    }
    frag.append(grid);
  }

  return frag;
}

function slotCard(sl, stacks, inService, tz, template) {
  const stock = slotStock(sl);

  // The server's flags are the authority; the per-device rows only quantify
  // the wording. Never derived from updated_at.
  const offlineStacks = stacks.filter(deviceOffline).length;
  const anyOffline = slotOffline(sl) || offlineStacks > 0;
  // Red on the NUMBER means exactly one thing: this figure is compromised —
  // offline, degraded or faulted somewhere in the position. Quantity no
  // longer colours it: a colour per meaning is all a human can register,
  // and "how much is left" is the number's own job.
  const compromised = anyOffline || !!sl.any_degraded || !!sl.any_fault;

  const card = h('div', { class: 'slot' });

  card.append(h('div', { class: 'slot-top' },
    h('span', { class: 'slot-pos' }, `Slot ${sl.food_slot}`)));

  // The dish comes ONLY from dated rows (sl.current_food) — the weekly
  // template is never displayed as the menu, or a service could pass with a
  // dish on screen and no record behind it. But when the template HAS a plan
  // for this very slot and nobody has applied it, "No menu entered" is the
  // wrong message: the menu exists, it just needs one press of Apply. Say
  // that instead; the planned dish rides in the tooltip only.
  const plan = !sl.current_food && inService && sl.current_meal
    ? template.find(t => t.location === sl.location
        && Number(t.food_slot) === Number(sl.food_slot)
        && t.meal_type === sl.current_meal
        && t.weekday === weekdayOf(serviceDate(tz)))
    : null;
  card.append(sl.current_food
    ? h('div', { class: 'slot-food' }, sl.current_food)
    : plan
      ? h('div', {
          class: 'slot-food unset',
          title: `The weekly template plans "${plan.food_name}" here, but it is not `
            + 'recorded for today. Menu → Weekly template → Apply to dates.',
        }, 'Menu not applied — see Menu › Apply')
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
    // An offline stack's last count is KEPT — blanking it would send someone
    // to a station the screen just went silent about. Ink by default; red
    // only when compromised. The fault and no-data branches above are
    // untouched: they render no numeral.
    card.append(h('div', { class: 'slot-figure' },
      h('span', {
        class: `slot-count${compromised ? ' is-alert' : ''}`,
        title: anyOffline
          ? 'A stack here stopped reporting during service — this is its last known count.'
          : undefined,
      }, sl.any_degraded ? `≥${stock.trusted}` : String(stock.trusted)),
      h('span', { class: 'slot-of' }, `of ${stock.capacity} bowls`)));
  }

  // The capsule bar carries DATA CONFIDENCE, not a quantity colour band:
  //   solid green    bowls confirmed by live, healthy stacks
  //   red stripes    the share of this position whose data is NOT valid —
  //                  each offline/degraded/faulted stack's whole capacity
  //   grey track     confirmed empty space
  // Three stacks with one offline: a third of the bar is striped, and the
  // reader knows exactly how much of the figure to trust.
  {
    const capacity = Number(sl.bowls_capacity) || (stacks.length * MAX_BOWLS);
    const bad = stacks.filter(d => deviceOffline(d)
      || d.stack_status === 'degraded' || d.stack_status === 'discontiguous'
      || (d.reported && d.sensors_online === 0));
    const confirmed = stacks
      .filter(d => !bad.includes(d) && d.reported && d.stack_status === 'ok')
      .reduce((n, d) => n + (d.stack_count ?? 0), 0);
    // Fallback when the per-device rows are not in yet: trust the server
    // aggregates — all-or-nothing striping is still honest.
    const invalidCap = stacks.length ? bad.length * MAX_BOWLS
      : (slotOffline(sl) || sl.any_degraded || sl.any_fault ? capacity : 0);
    const validFill = stacks.length ? confirmed
      : (invalidCap ? 0 : Math.max(0, Number(stock.trusted ?? 0)));

    const pctFill = capacity ? Math.min(100, (validFill / capacity) * 100) : 0;
    const pctBad = capacity ? Math.min(100 - pctFill, (invalidCap / capacity) * 100) : 0;
    card.append(h('div', {
      class: 'meter',
      title: `${validFill} bowls confirmed by healthy stacks · `
        + `${invalidCap ? `${Math.round(pctBad)}% of this position is not reporting valid data` : 'all stacks reporting valid data'}`,
    },
      h('i', { class: 'fill', style: `width:${pctFill}%` }),
      h('i', { class: 'invalid', style: `width:${pctBad}%` })));
  }

  const okStacks = stacks.filter(d => d.stack_status === 'ok' && d.reported).length;
  // Fault is rare and actionable, so its one sentence stays.
  if (sl.any_fault && stock.trusted != null) {
    card.append(h('div', { class: 'slot-sub' },
      `${stock.trusted} bowls on the ${okStacks} stack${okStacks === 1 ? '' : 's'} still reading correctly.`));
  }
  if (!inService && sl.oldest_update) {
    card.append(h('div', { class: 'slot-sub dim' }, `As of ${fmtClock(sl.oldest_update, tz)}`));
  }

  // One symbolic line per stack: state glyph · id · count · battery. No
  // chips, no words — the words live in the tooltip, on the Health page, and
  // in the page legend. Glyph priority mirrors severity: an impossible
  // reading outranks silence outranks a dead sensor.
  if (stacks.length) {
    const strip = h('div', { class: 'dev-strip' });
    for (const d of stacks.sort((a, b) => a.device_id.localeCompare(b.device_id))) {
      const st = deviceStack(d);
      const gone = deviceOffline(d);
      const [cls, glyph, word] =
        st.kind === 'fault' || d.stack_status === 'discontiguous'
          ? ['fault', '▲', 'impossible reading — check the sensors']
          : gone
            ? ['off', '✕', 'offline — showing its last value']
            : d.stack_status === 'degraded'
              ? ['deg', '◐', 'a sensor is down']
              : st.kind === 'none'
                ? ['none', '◌', 'no reading']
                : ['ok', '●', 'reporting'];
      const battWord = d.battery_level == null
        ? 'no battery detected'
        : `battery ${d.battery_level}`
          + `${d.battery_mv ? ` (${d.battery_mv} mV)` : ''}`
          + `${d.charging ? ' — charging' : ''}`;
      strip.append(h('a', {
        class: 'dev-line',
        href: `#/device/${encodeURIComponent(d.device_id)}`,
        title: `${d.device_id} — ${word} · updated ${fmtRelative(d.updated_at)} · ${battWord}`,
      },
        h('span', { class: `st st-${cls}`, 'aria-hidden': 'true' }, glyph),
        h('span', { class: 'id' }, d.device_id.replace(/^BWL-/, '')),
        h('span', { class: 'ct' }, st.text),
        batteryBar(d.battery_level, d.charging, `${d.device_id}: ${battWord}`)));
    }
    card.append(strip);
  }

  return card;
}