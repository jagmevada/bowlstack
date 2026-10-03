// ====================================================================
//  Health — sorted by severity, never by device_id.
//
//  The point of this screen is to surface the one station that needs
//  attention, not to enumerate thirty healthy ones. Rows are SYMBOLIC —
//  one glyph line per device, same vocabulary as Stock — so a phone shows
//  the whole fleet without scrolling. The words live in each row's
//  tooltip and, in full, on the device page the row links to.
//
//  Three states that look alike and mean different things:
//    awaiting_deployment  registered, never installed   -> greyed, own section
//    data_is_stale        outside service hours         -> shown, "as of <time>"
//    offline              should be reporting, is not   -> the alarm
//  `offline` is already service-hour aware. Nothing here recomputes staleness
//  from updated_at.
// ====================================================================

import { h, empty, levelColumn, cellColumn, banner, batteryBar } from '../ui.js';
import {
  compareDevices, deviceSeverity, deviceStack, deviceGlyph, deviceWeight, isWeighed,
  cellsTotal, isFault, isDegraded, isHub, isBatteryWarn, nodeHealthText, nodeHealthList,
  deviceOffline, fmtRelative,
  powerState,
} from '../domain.js';

// `fault` and `degraded` are DIFFERENT failures and get different filters —
// they used to share one, so the "1 faults" chip opened a 3-row list.
//   fault     the reading cannot be trusted at all: a stack's impossible level
//             pattern, or a load cell not answering or saturated.
//   degraded  the reading is a lower bound or unproven: a stack sensor down,
//             or a scale not calibrated or tared. See isFault/isDegraded.
const FILTERS = {
  all:      { label: 'All', test: () => true },
  problems: { label: 'Needs attention', test: d => !d.awaiting_deployment && deviceSeverity(d).rank >= 50 },
  offline:  { label: 'Offline', test: deviceOffline },
  battery:  { label: 'Battery', test: isBatteryWarn },
  // BOTH PRODUCTS, because these are filters on a FAULT, not on a sensor type.
  // Keyed on stack_status alone they silently excluded every load cell: a
  // station with no cell answering is the most serious thing on this page and
  // would not have appeared under "Faults" at all.
  fault:    { label: 'Faults', test: isFault },
  degraded: { label: 'Degraded', test: isDegraded },
  // Registered but never heard from. These live in their own section rather
  // than the deployed list, so the filter's job is to show THAT section
  // alone — see the render conditions below.
  awaiting: { label: 'Not deployed', test: d => d.awaiting_deployment },
};

export function renderHealth(state, params) {
  const frag = document.createDocumentFragment();
  const devices = state.devices;
  const active = FILTERS[params.get('f')] ? params.get('f') : 'all';
  const locFilter = params.get('loc') || '';
  const query = (params.get('q') || '').trim();

  // Searching means "find me this device", so it overrides the filters rather
  // than intersecting with them. A healthy unit ranks 0 and is hidden by every
  // problem filter -- which is correct for triage and useless when someone is
  // holding a board and wants to know whether it is reporting.
  const searching = query.length > 0;
  const matches = d => {
    const q = query.toLowerCase();
    return d.device_id.toLowerCase().includes(q)
      || (d.label || '').toLowerCase().includes(q)
      || (d.current_food || '').toLowerCase().includes(q);
  };

  const bar = h('div', { class: 'toolbar' });
  for (const [key, f] of Object.entries(FILTERS)) {
    bar.append(h('button', {
      class: 'ghost',
      'aria-pressed': String(!searching && key === active),
      disabled: searching,
      onclick: () => setParam('f', key === 'all' ? null : key),
    }, f.label));
  }
  bar.append(h('select', {
    'aria-label': 'Area',
    disabled: searching,
    onchange: e => setParam('loc', e.target.value || null),
  },
    h('option', { value: '' }, 'All areas'),
    ...['D', 'M', 'T', 'R'].map(l =>
      h('option', { value: l, selected: locFilter === l }, l))));

  const search = h('input', {
    type: 'search', value: query, placeholder: 'Find a device…',
    'aria-label': 'Find a device by ID, label or dish',
    style: 'max-width:14rem',
  });
  let debounce = null;
  search.addEventListener('input', () => {
    clearTimeout(debounce);
    debounce = setTimeout(() => setParam('q', search.value.trim() || null), 250);
  });
  bar.append(search);
  frag.append(bar);

  // Same glyph/word pairing as Stock, once per page — the rows themselves
  // never print a state word.
  frag.append(h('div', { class: 'legend-line' },
    h('span', {}, h('b', { class: 'st-ok' }, '●'), ' reporting'),
    h('span', {}, h('b', { class: 'st-off' }, '✕'), ' offline'),
    h('span', {}, h('b', { class: 'st-fault' }, '▲'), ' fault'),
    h('span', {}, h('b', { class: 'st-deg' }, '◐'), ' degraded'),
    h('span', {}, h('b', { class: 'st-none' }, '◌'), ' no reading')));

  const deployed = devices.filter(d => !d.awaiting_deployment);

  const live = (searching ? deployed.filter(matches) : deployed
      .filter(FILTERS[active].test)
      .filter(d => !locFilter || d.location === locFilter))
    .sort(compareDevices);

  const waiting = devices
    .filter(d => d.awaiting_deployment)
    .filter(d => searching ? matches(d) : (!locFilter || d.location === locFilter))
    .sort((a, b) => a.device_id.localeCompare(b.device_id));

  // Say out loud when devices are being withheld. A pressed toolbar button is
  // too quiet: arriving here from a fleet chip lands on `?f=offline`, and a
  // healthy device then looks absent rather than filtered out.
  const waitingShown = searching || active === 'all' || active === 'awaiting';
  const hiddenCount = devices.length - live.length - (waitingShown ? waiting.length : 0);
  if (searching) {
    frag.append(banner('info', '⌕',
      h('b', {}, `Showing ${live.length + waiting.length} matching “${query}”. `),
      'Search ignores the filters, so a healthy device still turns up. ',
      h('a', { href: '#', onclick: e => { e.preventDefault(); setParam('q', null); } }, 'Clear search')));
  } else if (hiddenCount > 0) {
    frag.append(banner('warning', '⌕',
      h('b', {}, `${hiddenCount} of ${devices.length} devices hidden `),
      `by ${active === 'all' ? '' : `“${FILTERS[active].label}”`}`,
      active !== 'all' && locFilter ? ' and ' : '',
      locFilter ? `area ${locFilter}` : '',
      '. ',
      h('a', {
        href: '#',
        onclick: e => { e.preventDefault(); location.hash = '#/health'; },
      }, 'Show all devices')));
  }

  if (!live.length && !waiting.length) {
    frag.append(empty(searching
      ? `No device matches “${query}”.`
      : 'Nothing matches this filter.'));
    return frag;
  }

  const section = (title, devs, note, muted = false) => {
    const list = h('div', { class: 'dev-list compact' });
    for (const d of devs) list.append(deviceRow(d, state.power));
    return h('div', { class: 'section' },
      h('div', { class: 'section-head' },
        h('h2', muted ? { class: 'muted' } : {}, title),
        h('span', { class: 'count' }, note)),
      list);
  };

  // HUBS FIRST, in a section of their own. There are three at most and each is
  // its area's power and link to the platforms -- a hub on its backup battery
  // is the explanation for whatever goes quiet below it next. Severity still
  // orders the rows within each section.
  const hubs = live.filter(isHub);
  const rest = live.filter(d => !isHub(d));
  if (hubs.length) frag.append(section('Hubs', hubs, `${hubs.length} area hub${hubs.length === 1 ? '' : 's'}`));
  if (rest.length) {
    frag.append(section(searching ? 'Matches' : active === 'all' ? 'Deployed' : FILTERS[active].label,
      rest, `${rest.length} device${rest.length === 1 ? '' : 's'}`));
  } else if (!live.length && active !== 'all' && active !== 'awaiting' && !searching) {
    frag.append(banner('info', '✓', 'Nothing in this category. '));
  }

  if (waiting.length && waitingShown) {
    frag.append(section('Awaiting deployment', waiting,
      `${waiting.length} registered, never reported — not a fault`, true));
  }

  return frag;
}

function setParam(key, value) {
  const [path, query = ''] = location.hash.slice(1).split('?');
  const p = new URLSearchParams(query);
  if (value == null) p.delete(key); else p.set(key, value);
  const qs = p.toString();
  location.hash = `#${path}${qs ? `?${qs}` : ''}`;
}

function miniLevels(levels) {
  const col = levelColumn(levels);
  col.classList.add('mini');
  return col;
}

// One line per device: glyph · id · position · levels · count · battery (node
// health on a platform; charging and battery on a hub). Everything the old badge stack said now rides the tooltip; the device page
// says it in full sentences. The full device_id stays (this is the roster
// someone searches), the position compresses to D3-style.
// `power` threaded in rather than reached for: this is called from two
// places and neither had state in scope, which is why the chip here kept
// the old behaviour after the Power card was fixed.
function deviceRow(d, power) {
  const sev = deviceSeverity(d);
  const hub = isHub(d);
  const scale = isWeighed(d);   // a counter scale or a buffer platform
  // Same six columns whichever product this is, so the roster stays a single
  // scannable grid -- only what column 4 and 5 CONTAIN differs. A scale that
  // laid itself out differently would break the alignment the whole page is.
  const reading = scale ? deviceWeight(d) : deviceStack(d);
  const g = deviceGlyph(d);
  // Same omission the Stock chips had: the charge state was appended only when
  // `charging` was truthy, and that is null on every board without the STAT
  // mod -- so a plugged-in unit's tooltip mentioned power not at all.
  const pw = powerState(d, power);
  // A PLATFORM HAS NO BATTERY -- its hub does -- so the slot the battery held
  // carries what the hub polls from it instead.
  const node = scale ? nodeHealthText(d) : null;
  const nodeList = scale ? nodeHealthList(d) : null;
  const battWord = scale ? `node: ${node}`
    : d.battery_level == null
    ? 'no battery detected'
    : `battery ${d.battery_level}`
      + `${d.battery_mv ? ` (${d.battery_mv} mV)` : ''}`
      + ` — ${pw.word}`;
  const title = [
    sev.reasons.join(' · ') || 'Healthy',
    d.updated_at ? `updated ${fmtRelative(d.updated_at)}` : 'never reported',
    d.awaiting_deployment ? null : battWord,
    d.firmware ? `fw ${d.firmware}` : null,
  ].filter(Boolean).join(' · ');

  return h('a', {
    class: `dev devc sev-${sev.level}`,
    href: `#/device/${encodeURIComponent(d.device_id)}`,
    style: 'text-decoration:none;color:inherit',
    title,
  },
    h('span', { class: `st st-${g.cls}`, 'aria-hidden': 'true' }, g.glyph),
    h('span', { class: 'dev-id' }, d.device_id),
    h('span', { class: 'devc-pos' },
      d.location != null && d.food_slot != null ? `${d.location}${d.food_slot}`
        : d.location != null ? d.location : '—'),
    // A hub measures nothing: no ladder, no cells -- an empty cell keeps the grid.
    hub ? h('span', { 'aria-hidden': 'true' })
      : scale ? cellColumn(d.cells_online, cellsTotal(d)) : miniLevels(d.levels),
    // Red only where there IS a last value to redden — the fault (`!`) and
    // never-reported (`—`) renderings are not counts, so the `na` grey owns
    // them and the offline red must not touch them.
    hub ? h('span', { class: 'dev-count na' }, pw.word) : h('span', {
      class: 'dev-count'
        + (reading.kind === 'count' || reading.kind === 'bound'
           || reading.kind === 'weight'
            ? (deviceOffline(d) ? ' is-offline' : '')
            : ' na')
        + (scale ? ' is-weight' : ''),
      title: reading.note || undefined,
    }, reading.text),
    d.awaiting_deployment
      ? h('span', { class: 'batt-slot', 'aria-hidden': 'true' })
      // One span per fact, so a phone wraps BETWEEN them -- never "no-" / "load".
      : scale ? h('span', { class: 'devc-node' }, nodeList.length
          ? nodeList.map((p, i) => [i ? ' · ' : null, h('span', {}, p)]) : node)
      : batteryBar(d.battery_level, pw.powered, battWord));
}
