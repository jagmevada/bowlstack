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

import { h, empty, levelColumn, banner, batteryBar } from '../ui.js';
import {
  compareDevices, deviceSeverity, deviceStack, deviceGlyph,
  deviceOffline, fmtRelative,
} from '../domain.js';

// `fault` and `degraded` are DIFFERENT failures and get different filters —
// they used to share one, so the "1 faults" chip opened a 3-row list.
//   fault     the sensors all answer, but the reading is physically
//             impossible (a bowl above an empty level — bowls are stacked,
//             f2 cannot exist without f1). Mount/obstruction/sensor lying.
//   degraded  a sensor itself is down, so the count is a lower bound.
const FILTERS = {
  all:      { label: 'All', test: () => true },
  problems: { label: 'Needs attention', test: d => !d.awaiting_deployment && deviceSeverity(d).rank >= 50 },
  offline:  { label: 'Offline', test: deviceOffline },
  battery:  { label: 'Battery', test: d => d.battery_level === 'low' || d.battery_level === 'critical' },
  fault:    { label: 'Faults', test: d => d.stack_status === 'discontiguous' },
  degraded: { label: 'Degraded', test: d => d.stack_status === 'degraded' },
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

  if (live.length) {
    const list = h('div', { class: 'dev-list compact' });
    for (const d of live) list.append(deviceRow(d));
    frag.append(h('div', { class: 'section' },
      h('div', { class: 'section-head' },
        h('h2', {}, searching ? 'Matches' : active === 'all' ? 'Deployed' : FILTERS[active].label),
        h('span', { class: 'count' }, `${live.length} device${live.length === 1 ? '' : 's'}`)),
      list));
  } else if (active !== 'all' && active !== 'awaiting' && !searching) {
    frag.append(banner('info', '✓', 'Nothing in this category. '));
  }

  if (waiting.length && waitingShown) {
    const list = h('div', { class: 'dev-list compact' });
    for (const d of waiting) list.append(deviceRow(d));
    frag.append(h('div', { class: 'section' },
      h('div', { class: 'section-head' },
        h('h2', { class: 'muted' }, 'Awaiting deployment'),
        h('span', { class: 'count' }, `${waiting.length} registered, never reported — not a fault`)),
      list));
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

// One line per device: glyph · id · position · levels · count · battery.
// Everything the old badge stack said now rides the tooltip; the device page
// says it in full sentences. The full device_id stays (this is the roster
// someone searches), the position compresses to D3-style.
function deviceRow(d) {
  const sev = deviceSeverity(d);
  const stack = deviceStack(d);
  const g = deviceGlyph(d);
  const battWord = d.battery_level == null
    ? 'no battery detected'
    : `battery ${d.battery_level}`
      + `${d.battery_mv ? ` (${d.battery_mv} mV)` : ''}`
      + `${d.charging ? ' — charging' : ''}`;
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
    miniLevels(d.levels),
    // Red only where there IS a last value to redden — the fault (`!`) and
    // never-reported (`—`) renderings are not counts, so the `na` grey owns
    // them and the offline red must not touch them.
    h('span', {
      class: 'dev-count'
        + (stack.kind === 'count' || stack.kind === 'bound'
            ? (deviceOffline(d) ? ' is-offline' : '')
            : ' na'),
    }, stack.text),
    d.awaiting_deployment
      ? h('span', { class: 'batt-slot', 'aria-hidden': 'true' })
      : batteryBar(d.battery_level, d.charging, battWord));
}
