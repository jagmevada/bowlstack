// ====================================================================
//  Health — sorted by severity, never by device_id.
//
//  The point of this screen is to surface the one station that needs
//  attention, not to enumerate thirty healthy ones.
//
//  Three states that look alike and mean different things:
//    awaiting_deployment  registered, never installed   -> greyed, own section
//    data_is_stale        outside service hours         -> shown, "as of <time>"
//    offline              should be reporting, is not   -> the alarm
//  `offline` is already service-hour aware. Nothing here recomputes staleness
//  from updated_at.
// ====================================================================

import { h, badge, empty, levelColumn, banner } from '../ui.js';
import {
  compareDevices, deviceSeverity, deviceStack, batteryInfo, positionLabel,
  deviceOffline, fmtRelative, fmtDateTime, serviceState,
} from '../domain.js';

const FILTERS = {
  all:      { label: 'All', test: () => true },
  problems: { label: 'Needs attention', test: d => !d.awaiting_deployment && deviceSeverity(d).rank >= 50 },
  offline:  { label: 'Offline', test: deviceOffline },
  battery:  { label: 'Battery', test: d => d.battery_level === 'low' || d.battery_level === 'critical' },
  fault:    { label: 'Faults', test: d => d.stack_status === 'discontiguous' || d.stack_status === 'degraded' },
};

export function renderHealth(state, params) {
  const frag = document.createDocumentFragment();
  const devices = state.devices;
  const { tz } = serviceState(devices);
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
  const hiddenCount = devices.length - live.length - (searching || active === 'all' ? waiting.length : 0);
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
    const list = h('div', { class: 'dev-list' });
    for (const d of live) list.append(deviceRow(d, tz));
    frag.append(h('div', { class: 'section' },
      h('div', { class: 'section-head' },
        h('h2', {}, searching ? 'Matches' : active === 'all' ? 'Deployed' : FILTERS[active].label),
        h('span', { class: 'count' }, `${live.length} device${live.length === 1 ? '' : 's'}`)),
      list));
  } else if (active !== 'all' && !searching) {
    frag.append(banner('info', '✓', 'Nothing in this category. '));
  }

  if (waiting.length && (searching || active === 'all')) {
    const list = h('div', { class: 'dev-list' });
    for (const d of waiting) list.append(deviceRow(d, tz));
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

function deviceRow(d, tz) {
  const sev = deviceSeverity(d);
  const stack = deviceStack(d);
  const batt = batteryInfo(d.battery_level);

  const badges = h('div', { class: 'dev-badges' });

  if (d.awaiting_deployment) {
    badges.append(badge('idle', '◌', 'Never reported'));
  } else {
    if (d.offline) badges.append(badge('critical', '✕', 'Offline'));
    else if (d.missed_last_service) {
      badges.append(badge('critical', '✕', 'Missed last service',
        'Did not report at all during the most recently completed service window.'));
    }
    if (d.stack_status === 'discontiguous') badges.append(badge('critical', '▲', 'Impossible reading'));
    if (d.stack_status === 'degraded') badges.append(badge('warning', '◐', 'Count is a lower bound'));
    if (d.sensors_online != null && d.sensors_online < 4) {
      badges.append(badge(d.sensors_online === 0 ? 'critical' : 'warning', '◉',
        `${d.sensors_online}/4 sensors`));
    }
    badges.append(badge(batt.status === 'idle' ? 'idle' : batt.status, batt.glyph,
      batt.absent ? 'No battery' : batt.label,
      d.battery_mv ? `${d.battery_mv} mV at the cell` : 'No cell detected'));
    if (d.charging) badges.append(badge('good', '⚡', 'Charging'));
    // fmtDateTime, not fmtClock: a time-only "As of 20:05" made a device dead
    // since last Tuesday pixel-identical to one that reported at yesterday's
    // dinner. The date is the information.
    if (d.data_is_stale) badges.append(badge('idle', '◷', `As of ${fmtDateTime(d.updated_at, tz)}`));
  }

  // The status line under the badges. For a silent device it says OFFLINE in
  // so many words — a badge among five other badges was too easy to read past,
  // and every other number on the row is last-known, which deserves stating.
  const gone = deviceOffline(d);
  const statusLine = d.awaiting_deployment
    ? h('div', { class: 'dev-where' }, 'Registered, awaiting installation')
    : gone
      ? h('div', { class: 'dev-offline-line' },
          h('span', { class: 'g', 'aria-hidden': 'true' }, '✕'),
          `OFFLINE — no telemetry. Last heard ${fmtRelative(d.updated_at)}`,
          h('span', { class: 'dim', style: 'font-weight:400' },
            ` · values shown are last known${d.firmware ? ` · fw ${d.firmware}` : ''}`))
      : h('div', { class: 'dev-where' },
          `Updated ${fmtRelative(d.updated_at)}${d.firmware ? ` · fw ${d.firmware}` : ''}`);

  const mid = h('div', { class: 'dev-mid' },
    h('div', {},
      h('span', { class: 'dev-id' }, d.device_id),
      ' ',
      h('span', { class: 'dev-where' }, positionLabel(d)),
      d.current_food ? h('span', { class: 'dev-where' }, ` · ${d.current_food}`) : null),
    badges,
    statusLine);

  return h('a', {
    class: `dev sev-${sev.level}`,
    href: `#/device/${encodeURIComponent(d.device_id)}`,
    style: 'text-decoration:none;color:inherit',
    title: sev.reasons.join(' · ') || 'Healthy',
  },
    levelColumn(d.levels),
    mid,
    h('div', { class: 'dev-right' },
      // Red only where there IS a last value to redden — the fault (`!`) and
      // never-reported (`—`) renderings are not counts, so the `na` grey owns
      // them and the offline red must not touch them. Reusing the same
      // kind-test keeps the two classes structurally unable to disagree.
      h('span', {
        class: 'dev-count'
          + (stack.kind === 'count' || stack.kind === 'bound'
              ? (deviceOffline(d) ? ' is-offline' : '')
              : ' na'),
      }, stack.text)));
}
