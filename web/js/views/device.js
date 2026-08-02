// ====================================================================
//  Device detail — the field-trial screen.
//
//  status_events holds one row per REAL CHANGE, never per report, so:
//    - gaps between rows are steady state, not missing data;
//    - the series is a step, not a sample;
//    - `seq` is assigned when an event is ENQUEUED, so a gap in seq means
//      events were genuinely dropped rather than never having happened.
//  Order by recorded_at, never by id or received_at: a batch is written at
//  most every 5 s and several rows can share an arrival instant while
//  describing moments up to 5 s apart.
// ====================================================================

import { h, badge, empty, banner, copyText, fillSlot } from '../ui.js';
import { unwrap, describeError } from '../supa.js';
import { stepChart, statusTimeline, STATUS_STYLE } from '../chart.js';
import {
  batteryInfo, deviceStack, deviceSeverity, deviceOffline, positionLabel,
  serviceState, fmtRelative, fmtDateTime, fmtUptime,
} from '../domain.js';
import { APP_VERSION } from '../version.js';

const WINDOWS = [
  { key: '2',  label: '2 h',  hours: 2 },
  { key: '6',  label: '6 h',  hours: 6 },
  { key: '12', label: '12 h', hours: 12 },
  { key: '24', label: '1 d',  hours: 24 },
  { key: '48', label: '2 d',  hours: 48 },
];

// The page redraws on the 20 s fleet poll so the header stays live, but history
// is up to a thousand rows and only changes when the device changes state.
// Re-querying it every poll would be the app's heaviest traffic by far.
const HISTORY_TTL_MS = 60_000;
const historyCache = new Map();
const HISTORY_SLOT_ID = 'device-history';

/** The ⟳ button means "get me the current truth", so it must reach history too. */
export function clearHistoryCache() { historyCache.clear(); }

export function renderDevice(state, params, ctx) {
  const id = decodeURIComponent(params.get('id') || '');
  const dev = state.devices.find(d => d.device_id === id);
  const frag = document.createDocumentFragment();

  frag.append(h('a', { class: 'back', href: '#/health' }, '← All devices'));

  if (!dev) {
    frag.append(empty(`No device ${id} in the registry.`));
    return frag;
  }

  const { tz } = serviceState(state.devices);
  const sev = deviceSeverity(dev);
  const stack = deviceStack(dev);
  const batt = batteryInfo(dev.battery_level);

  frag.append(h('div', { class: 'section-head' },
    h('h1', {}, dev.device_id),
    h('span', { class: 'count' }, positionLabel(dev)),
    dev.label ? h('span', { class: 'count' }, `· ${dev.label}`) : null));

  if (dev.awaiting_deployment) {
    frag.append(banner('info', '◌',
      h('b', {}, 'Awaiting deployment. '),
      'Registered but never heard from. This is not a fault.'));
  } else if (dev.offline) {
    frag.append(banner('critical', '✕',
      h('b', {}, 'Offline during service. '),
      `Should be reporting and is not — last update ${fmtRelative(dev.updated_at)}.`));
  } else if (dev.missed_last_service) {
    frag.append(banner('critical', '✕',
      h('b', {}, 'Went dark during service. '),
      `Was not reporting when the last service window ended — `,
      `last heard ${fmtRelative(dev.updated_at)} (${fmtDateTime(dev.updated_at, tz)}). `,
      'Values below are its last known state.'));
  } else if (dev.data_is_stale) {
    frag.append(banner('info', '◷',
      h('b', {}, 'Outside service hours. '),
      `Values below are last-known, as of ${fmtDateTime(dev.updated_at, tz)}.`));
  }

  if (dev.stack_status === 'discontiguous') {
    frag.append(banner('critical', '▲',
      h('b', {}, 'Impossible level pattern. '),
      'A bowl is being detected above an empty level. Bowls rest on each other and ',
      'cannot float, so this is a failed sensor, a misaligned mount, or an obstruction ',
      '— not a count.'));
  } else if (dev.stack_status === 'degraded') {
    if (stack.kind === 'none') {
      frag.append(banner('critical', '◉',
        h('b', {}, 'No working sensors. '),
        'Every level sensor is offline, so there is no bowl reading at all — ',
        'any count in the payload is a leftover, not a measurement.'));
    } else if (stack.kind === 'count') {
      frag.append(banner('warning', '◐',
        h('b', {}, 'A sensor is down — but the count is still exact. '),
        'The stack is full, and a full stack leaves a dead sensor nothing to hide.'));
    } else {
      frag.append(banner('warning', '◐',
        h('b', {}, 'Count is a lower bound. '),
        'A sensor between the top bowl and the first empty level is down, so there may ',
        'be more bowls than shown.'));
    }
  }

  // --- current state -------------------------------------------------
  const grid = h('div', { class: 'detail-grid section' });

  grid.append(h('div', { class: 'card' },
    h('div', { class: 'chart-title' }, 'Stack now'),
    h('div', { style: 'display:flex;gap:1.2rem;align-items:center;margin-top:.5rem' },
      // ONE labelled column — f-label, cell, state word. It used to be an
      // unlabelled column here PLUS a labelled repeat of the same four
      // levels below the count: the same fact drawn twice.
      h('div', { class: 'level-rows' },
        ...(dev.levels || ['unknown', 'unknown', 'unknown', 'unknown']).map((v, i) =>
          h('div', { class: 'level-row', title: `f${i + 1}: ${v}` },
            h('span', { class: 'k' }, `f${i + 1}`),
            h('span', { class: `bar ${v}` }))).reverse()),
      h('div', {},
        // The critical banner above already carries the glyph and the word,
        // so the red here is an accelerator, not a lone colour signal. Only
        // a real count is reddened — `!` and `—` are not last values.
        h('div', {
          class: 'hero'
            + (deviceOffline(dev) && (stack.kind === 'count' || stack.kind === 'bound')
                ? ' is-offline' : ''),
        }, stack.text),
        h('div', { class: 'muted', style: 'font-size:.82rem' },
          // "Last known" prefixes the note rather than replacing it: a stale
          // lower bound is still a lower bound, and hiding the degraded/fault
          // explanation because the device also went silent would drop the
          // more actionable half of the story.
          deviceOffline(dev)
            ? `Last known — ${fmtRelative(dev.updated_at)}${stack.note ? ` · ${stack.note}` : ''}`
            : stack.note || `of 4 bowls · ${dev.stack_status || 'no status'}`))),
    h('div', { class: 'dim', style: 'font-size:.75rem;margin-top:.5rem' },
      'f4 on top, f1 the bottom bowl · blue: bowl present · striped: sensor not answering · blank: empty')));

  grid.append(h('div', { class: 'card' },
    h('div', { class: 'chart-title' }, 'Power'),
    h('div', { style: 'margin:.6rem 0' },
      badge(batt.status === 'idle' ? 'idle' : batt.status, batt.glyph, batt.label),
      ' ',
      dev.charging ? badge('good', '⚡', 'Charging') : ''),
    h('dl', { class: 'kv' },
      kv('Cell', dev.battery_mv != null ? `${dev.battery_mv} mV` : 'not detected'),
      kv('Band', dev.battery_level ?? 'none')),
    h('div', { class: 'dim', style: 'font-size:.75rem;margin-top:.5rem;line-height:1.4' },
      'The band is hysteretic — it leaves a level lower than it re-enters it, so a band ',
      'that has not moved while the millivolts have is correct, not stale. There is no ',
      'percentage, deliberately.')));

  grid.append(h('div', { class: 'card' },
    h('div', { class: 'chart-title' }, 'Device'),
    h('dl', { class: 'kv', style: 'margin-top:.6rem' },
      kv('Sensors', dev.sensors_online != null ? `${dev.sensors_online} of 4 online` : '—'),
      kv('Firmware', dev.firmware ?? '—'),
      kv('Uptime', fmtUptime(dev.uptime_s)),
      kv('Last report', dev.updated_at ? `${fmtRelative(dev.updated_at)} (${fmtDateTime(dev.updated_at, tz)})` : 'never'),
      kv('In service', dev.in_service ? 'yes' : 'no'),
      kv('Timezone', dev.timezone),
      kv('Board MAC', dev.mac ?? '—'),
      kv('Dish now', dev.current_food ?? '—'),
      kv('Meal', dev.current_meal ?? 'none')),
    h('div', { class: 'dim', style: 'font-size:.75rem;margin-top:.5rem' },
      'The MAC identifies the board. A replaced board keeps this device_id — only the MAC changes.')));

  frag.append(grid);

  if (sev.reasons.length && !dev.awaiting_deployment) {
    frag.append(h('div', { class: 'slot-notes section' },
      ...sev.reasons.map(r => badge(sev.level, '•', r))));
  }

  // --- history -------------------------------------------------------
  const hoursKey = WINDOWS.some(w => w.key === params.get('h')) ? params.get('h') : '24';
  const hours = WINDOWS.find(w => w.key === hoursKey).hours;

  const bar = h('div', { class: 'toolbar' });
  for (const w of WINDOWS) {
    bar.append(h('button', {
      class: 'ghost', 'aria-pressed': String(w.key === hoursKey),
      onclick: () => { location.hash = `#/device/${encodeURIComponent(id)}?h=${w.key}`; },
    }, w.label));
  }
  bar.append(h('div', { class: 'grow' }));
  bar.append(h('button', {
    class: 'ghost',
    onclick: () => copyText(diagnostics(dev, historyState.rows, hours), 'Diagnostics copied'),
  }, 'Copy diagnostics'));

  const cacheKey = `${id}|${hours}`;
  const cached = historyCache.get(cacheKey);
  const fresh = cached && Date.now() - cached.at < HISTORY_TTL_MS;

  const historyState = { rows: fresh ? cached.rows : [] };
  const historyBox = h('div', { id: HISTORY_SLOT_ID }, fresh
    ? renderHistory(cached.rows, dev, tz, hours)
    : h('div', { class: 'empty' }, 'Loading history…'));

  frag.append(h('div', { class: 'section' },
    h('div', { class: 'section-head' }, h('h2', {}, 'History'),
      h('span', { class: 'count' }, 'one row per real change — gaps are steady state')),
    bar, historyBox));

  if (!fresh) {
    loadHistory(ctx.client, id, hours)
      .then(rows => {
        historyCache.set(cacheKey, { at: Date.now(), rows });
        historyState.rows = rows;
        fillSlot(HISTORY_SLOT_ID, historyBox, renderHistory(rows, dev, tz, hours));
      })
      .catch(err => {
        fillSlot(HISTORY_SLOT_ID, historyBox, banner('critical', '✕', describeError(err)));
      });
  }

  return frag;
}

function kv(k, v) {
  const dt = h('dt', {}, k), dd = h('dd', {}, v);
  const f = document.createDocumentFragment();
  f.append(dt, dd);
  return f;
}

async function loadHistory(client, deviceId, hours) {
  const since = new Date(Date.now() - hours * 3600_000).toISOString();
  return unwrap(await client
    .from('status_events')
    .select('recorded_at, received_at, reason, seq, boot_id, stack_count, stack_status, levels, sensors_online, battery_level, charging, firmware')
    .eq('device_id', deviceId)
    .gte('recorded_at', since)
    .order('recorded_at', { ascending: false })
    .limit(1000)) || [];
}

function renderHistory(rowsDesc, dev, tz, hours) {
  const frag = document.createDocumentFragment();
  if (!rowsDesc.length) {
    frag.append(empty(`No recorded changes in the last ${hours} hours. ` +
      `That means nothing changed, or the device was not powered.`));
    return frag;
  }

  const rows = [...rowsDesc].reverse();     // oldest first for plotting
  const points = rows.map(r => ({
    t: new Date(r.recorded_at).getTime(),
    v: r.stack_count,
    status: r.stack_status,
    battery: r.battery_level,
    reason: r.reason,
  }));

  const width = Math.max(300, (document.getElementById('view')?.clientWidth || 640) - 34);

  frag.append(h('div', { class: 'card', style: 'margin-bottom:.7rem' },
    stepChart({
      points, yMax: 4, tz, width,
      title: 'Bowls on this stack',
      subtitle: 'Steps hold between changes. Trustworthy readings only — a fault leaves a gap.',
    })));

  frag.append(h('div', { class: 'card', style: 'margin-bottom:.7rem' },
    statusTimeline({ points, tz, width, dev })));

  frag.append(reliability(rows, hours, tz));

  // The table view: every value the charts show, reachable without hover.
  const table = h('table', {},
    h('thead', {}, h('tr', {},
      h('th', {}, 'Recorded'), h('th', {}, 'Reason'), h('th', { class: 'num' }, 'Bowls'),
      h('th', {}, 'Status'), h('th', {}, 'Levels (f1→f4)'), h('th', { class: 'num' }, 'Sensors'),
      h('th', {}, 'Battery'), h('th', { class: 'num' }, 'Boot/seq'), h('th', {}, 'Delay'))),
    h('tbody', {}, ...rowsDesc.map(r => {
      const delayMs = new Date(r.received_at) - new Date(r.recorded_at);
      return h('tr', {},
        h('td', {}, fmtDateTime(r.recorded_at, tz)),
        h('td', {}, r.reason),
        h('td', { class: 'num' }, r.stack_status === 'discontiguous' ? '—' : r.stack_count),
        h('td', {}, `${(STATUS_STYLE[r.stack_status] || {}).glyph || ''} ${r.stack_status}`),
        h('td', {}, (r.levels || []).join(' ')),
        h('td', { class: 'num' }, r.sensors_online),
        h('td', {}, `${r.battery_level ?? 'none'}${r.charging ? ' ⚡' : ''}`),
        h('td', { class: 'num' }, `${r.boot_id}/${r.seq}`),
        h('td', {}, delayMs > 5000 ? `${Math.round(delayMs / 1000)}s buffered` : '—'));
    })));

  frag.append(h('details', { class: 'table-view section' },
    h('summary', {}, `Show all ${rowsDesc.length} events`),
    h('div', { class: 'table-wrap card', style: 'margin-top:.5rem;max-height:60vh;overflow:auto' }, table)));

  return frag;
}

/**
 * The numbers a field trial is actually run to collect. Everything here is
 * derived from data the firmware already sends; none of it needs new schema.
 */
export function analyseHistory(rows) {
  const boots = new Set();
  let dropped = 0, buffered = 0, maxDelayMs = 0, faults = 0, degraded = 0, bandChanges = 0;
  const bySeq = new Map();

  for (const r of rows) {
    boots.add(String(r.boot_id));
    const delay = new Date(r.received_at) - new Date(r.recorded_at);
    if (delay > 5000) { buffered++; maxDelayMs = Math.max(maxDelayMs, delay); }
    if (r.stack_status === 'discontiguous') faults++;
    if (r.stack_status === 'degraded') degraded++;
    if (!bySeq.has(String(r.boot_id))) bySeq.set(String(r.boot_id), []);
    bySeq.get(String(r.boot_id)).push(Number(r.seq));
  }

  // A gap in seq means events were enqueued and never arrived. seq increments
  // on enqueue precisely so this is visible server-side instead of vanishing.
  const gaps = [];
  for (const [boot, seqs] of bySeq) {
    seqs.sort((a, b) => a - b);
    for (let i = 1; i < seqs.length; i++) {
      const missing = seqs[i] - seqs[i - 1] - 1;
      if (missing > 0) { dropped += missing; gaps.push({ boot, from: seqs[i - 1], to: seqs[i], missing }); }
    }
  }

  let prevBand;
  for (const r of rows) {
    if (r.battery_level !== prevBand) { if (prevBand !== undefined) bandChanges++; prevBand = r.battery_level; }
  }

  return { boots: boots.size, dropped, gaps, buffered, maxDelayMs, faults, degraded, bandChanges, events: rows.length };
}

function reliability(rows, hours, tz) {
  const a = analyseHistory(rows);
  const card = h('div', { class: 'card', style: 'margin-bottom:.7rem' },
    h('div', { class: 'chart-title' }, `Reliability over ${hours} h`));

  const stats = h('div', { class: 'slot-notes', style: 'margin-top:.6rem' },
    badge(a.boots > 3 ? 'warning' : 'idle', '⟳', `${a.boots} boot${a.boots === 1 ? '' : 's'}`),
    badge(a.dropped ? 'critical' : 'good', a.dropped ? '▲' : '✓',
      a.dropped ? `${a.dropped} events dropped` : 'no dropped events'),
    badge(a.buffered ? 'warning' : 'idle', '◷',
      a.buffered ? `${a.buffered} buffered offline (max ${Math.round(a.maxDelayMs / 1000)}s)` : 'nothing buffered'),
    badge(a.faults ? 'critical' : 'idle', '▲', `${a.faults} fault transitions`),
    badge(a.degraded ? 'warning' : 'idle', '◐', `${a.degraded} degraded transitions`),
    badge(a.bandChanges > 6 ? 'warning' : 'idle', '▮', `${a.bandChanges} battery band changes`),
    badge('idle', '≡', `${a.events} events`));
  card.append(stats);

  if (a.gaps.length) {
    card.append(h('div', { class: 'slot-sub', style: 'margin-top:.5rem' },
      'Dropped: ' + a.gaps.map(g => `boot ${g.boot} seq ${g.from}→${g.to} (${g.missing})`).join(', ')));
  }
  card.append(h('div', { class: 'dim', style: 'font-size:.75rem;margin-top:.5rem;line-height:1.4' },
    'Buffered events are backdated from a device-reported age — a large delay after a ',
    'network outage is correct, not a bug. Repeated boots or a rising battery-band ',
    'change count are the two signals worth chasing.'));
  return card;
}

function diagnostics(dev, rows, hours) {
  return JSON.stringify({
    captured_at: new Date().toISOString(),
    dashboard_version: APP_VERSION,
    window_hours: hours,
    device: dev,
    history_summary: rows.length ? analyseHistory([...rows].reverse()) : null,
    recent_events: rows.slice(0, 40),
  }, null, 2);
}
