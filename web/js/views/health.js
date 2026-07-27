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
  fmtRelative, fmtClock, serviceState,
} from '../domain.js';

const FILTERS = {
  all:      { label: 'All', test: () => true },
  problems: { label: 'Needs attention', test: d => !d.awaiting_deployment && deviceSeverity(d).rank >= 50 },
  offline:  { label: 'Offline', test: d => d.offline },
  battery:  { label: 'Battery', test: d => d.battery_level === 'low' || d.battery_level === 'critical' },
  fault:    { label: 'Faults', test: d => d.stack_status === 'discontiguous' || d.stack_status === 'degraded' },
};

export function renderHealth(state, params) {
  const frag = document.createDocumentFragment();
  const devices = state.devices;
  const { tz } = serviceState(devices);
  const active = FILTERS[params.get('f')] ? params.get('f') : 'all';
  const locFilter = params.get('loc') || '';

  const bar = h('div', { class: 'toolbar' });
  for (const [key, f] of Object.entries(FILTERS)) {
    bar.append(h('button', {
      class: 'ghost',
      'aria-pressed': String(key === active),
      onclick: () => setParam('f', key === 'all' ? null : key),
    }, f.label));
  }
  bar.append(h('select', {
    'aria-label': 'Area',
    onchange: e => setParam('loc', e.target.value || null),
  },
    h('option', { value: '' }, 'All areas'),
    ...['D', 'M', 'T', 'R'].map(l =>
      h('option', { value: l, selected: locFilter === l }, l))));
  frag.append(bar);

  const live = devices
    .filter(d => !d.awaiting_deployment)
    .filter(FILTERS[active].test)
    .filter(d => !locFilter || d.location === locFilter)
    .sort(compareDevices);

  const waiting = devices
    .filter(d => d.awaiting_deployment)
    .filter(d => !locFilter || d.location === locFilter)
    .sort((a, b) => a.device_id.localeCompare(b.device_id));

  if (!live.length && !waiting.length) {
    frag.append(empty('Nothing matches this filter.'));
    return frag;
  }

  if (live.length) {
    const list = h('div', { class: 'dev-list' });
    for (const d of live) list.append(deviceRow(d, tz));
    frag.append(h('div', { class: 'section' },
      h('div', { class: 'section-head' },
        h('h2', {}, active === 'all' ? 'Deployed' : FILTERS[active].label),
        h('span', { class: 'count' }, `${live.length} device${live.length === 1 ? '' : 's'}`)),
      list));
  } else if (active !== 'all') {
    frag.append(banner('info', '✓', 'Nothing in this category. '));
  }

  if (waiting.length && active === 'all') {
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
    if (d.data_is_stale) badges.append(badge('idle', '◷', `As of ${fmtClock(d.updated_at, tz)}`));
  }

  const mid = h('div', { class: 'dev-mid' },
    h('div', {},
      h('span', { class: 'dev-id' }, d.device_id),
      ' ',
      h('span', { class: 'dev-where' }, positionLabel(d)),
      d.current_food ? h('span', { class: 'dev-where' }, ` · ${d.current_food}`) : null),
    badges,
    h('div', { class: 'dev-where' },
      d.awaiting_deployment ? 'Registered, awaiting installation'
        : `Updated ${fmtRelative(d.updated_at)}${d.firmware ? ` · fw ${d.firmware}` : ''}`));

  return h('a', {
    class: `dev sev-${sev.level}`,
    href: `#/device/${encodeURIComponent(d.device_id)}`,
    style: 'text-decoration:none;color:inherit',
    title: sev.reasons.join(' · ') || 'Healthy',
  },
    levelColumn(d.levels),
    mid,
    h('div', { class: 'dev-right' },
      h('span', { class: `dev-count${stack.kind === 'count' || stack.kind === 'bound' ? '' : ' na'}` },
        stack.text)));
}
