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

import { h, badge, empty, banner, copyText, fillSlot, cellColumn } from '../ui.js';
import { unwrap, describeError } from '../supa.js';
import { stepChart, weightChart, statusTimeline, STATUS_STYLE } from '../chart.js';
import {
  batteryInfo, deviceStack, deviceWeight, isScale, isBuffer, isWeighed, cellsTotal,
  deviceSeverity, deviceOffline, positionLabel,
  serviceState, fmtRelative, fmtDateTime, fmtUptime, fmtWeight, bowlsText,
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

  // A LOAD CELL GETS ITS OWN CARD, not the bowl one with different words in
  // it. The stack card is built around four levels that a scale does not have,
  // and `dev.levels || ['unknown' x4]` was drawing all four of them striped --
  // a fabricated ladder on a station whose actual reading is a single number.
  // TRIAL HARNESS: only ever appears on a scale that has actually reported an
  // estimate, so every other device and every other deployment is untouched.
  // isScale, not isWeighed: the knob is the counter's, and trial_manual_fill
  // filters kind = 'scale' anyway.
  const trial = isScale(dev) ? trialCard(dev, state, ctx) : null;
  if (trial) grid.append(trial);

  grid.append(isWeighed(dev) ? weightCard(dev) : h('div', { class: 'card' },
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
      'f4 on top, f1 the bottom bowl · blue: bowl present · outline: empty · striped: sensor not answering')));

  grid.append(h('div', { class: 'card' },
    h('div', { class: 'chart-title' }, 'Power'),
    h('div', { style: 'margin:.6rem 0' },
      badge(batt.status === 'idle' ? 'idle' : batt.status, batt.glyph, batt.label),
      ' ',
      // THE BADGE FOLLOWS THE SAME FOUR STATES AS THE ROW BELOW IT. It used to
      // show a bare "?" whenever `charging` was null, which is every unmodified
      // board -- so a plugged-in station wore a question mark beside a battery
      // the device could see perfectly well was on mains.
      powerBadge(dev, state)),
    h('dl', { class: 'kv' },
      kv('Cell', dev.battery_mv != null ? `${dev.battery_mv} mV` : 'not detected'),
      kv('Band', dev.battery_level ?? 'none'),
      // THREE STATES, NOT TWO, and only on the board that needs it. The
      // panel's charger drives its LED and reaches no GPIO, so "not charging"
      // is a claim the hardware cannot support -- null means unreadable, and
      // rendering that as a missing badge would read as "no".
      // FOUR STATES, because two different things are known to different
      // degrees and this row used to report only the one that is not.
      //
      // `charging` comes from the ETA6098's STAT pin, which reaches no GPIO
      // unless the mod is fitted -- so it is usually null and "no sense pin"
      // was true of it. But the board CAN see VBUS, through the divider on
      // IO10, and saying nothing about that threw away a fact the device had:
      // somebody looking at a plugged-in station was told only that something
      // was unreadable.
      //
      // "on mains" is deliberately not "yes". Plugged in is all this hardware
      // can see without the STAT wire; whether current is still flowing is
      // exactly what it cannot tell.
      isWeighed(dev) ? kv('Charging', chargeText(dev, state)) : null),
    h('div', { class: 'dim', style: 'font-size:.75rem;margin-top:.5rem;line-height:1.4' },
      'The band is hysteretic — it leaves a level lower than it re-enters it, so a band ',
      'that has not moved while the millivolts have is correct, not stale. There is no ',
      'percentage, deliberately.')));

  grid.append(h('div', { class: 'card' },
    h('div', { class: 'chart-title' }, 'Device'),
    h('dl', { class: 'kv', style: 'margin-top:.6rem' },
      isWeighed(dev)
        ? kv('Load cells', dev.cells_online != null
              ? `${dev.cells_online} of ${cellsTotal(dev)} converting` : '—')
        : kv('Sensors', dev.sensors_online != null ? `${dev.sensors_online} of 4 online` : '—'),
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
      h('span', { class: 'count' }, isWeighed(dev)
        ? 'sampled every two minutes and on every change'
        : 'one row per real change — gaps are steady state')),
    bar, historyBox));

  if (!fresh) {
    loadHistory(ctx.client, id, hours, isWeighed(dev))
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

async function loadHistory(client, deviceId, hours, weighed = false) {
  const since = new Date(Date.now() - hours * 3600_000).toISOString();

  // TWO TABLES, because there are two shapes of product. A scale or a buffer
  // cannot appear in status_events at all -- five of its NOT NULL columns are
  // bowl-shaped -- so its history lives in weight_samples. Same shape of
  // query, same ordering, same cap.
  //
  // `*`, NOT A COLUMN LIST. A buffer's rows carry bowls / bowls_confirmed /
  // gross_g, which exist only once migrate_buffer.sql has run -- and naming a
  // column PostgREST does not have is a 400 that blanks the whole history. `*`
  // asks for whatever the table has, so one query serves both databases.
  if (weighed) {
    return unwrap(await client
      .from('weight_samples')
      .select('*')
      .eq('device_id', deviceId)
      .gte('recorded_at', since)
      .order('recorded_at', { ascending: false })
      .limit(1000)) || [];
  }

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
    frag.append(empty(isWeighed(dev)
      ? `No weight recorded in the last ${hours} hours. That means the station `
        + `was not powered, or supabase/migrate_weight_samples.sql has not been run.`
      : `No recorded changes in the last ${hours} hours. `
        + `That means nothing changed, or the device was not powered.`));
    return frag;
  }

  if (isWeighed(dev)) return renderWeightHistory(rowsDesc, dev, tz, hours);

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

// ====================================================================
//  The load cell's reading card -- the counterpart to "Stack now".
//
//  A SINGLE NUMBER AND THE REASON IT IS OR IS NOT ONE, which is the whole
//  shape of a scale's report: weight_state always arrives, weight_g only
//  when that state is 'ok'. So the state is rendered first and the number
//  second, and when there is no number the card says WHY rather than
//  showing a dash and leaving somebody to guess.
// ====================================================================
// The badge beside the battery, sharing chargeText()'s four states so the two
// cannot disagree about the same instant -- a bolt for charging, a plug glyph
// for mains-but-not-charging, and a question mark ONLY when nothing is known.
function powerBadge(dev, state) {
  const row = (state.power || []).find(p => p.device_id === dev.device_id);
  const ext = row ? row.external_power : null;
  if (dev.charging) return badge('good', '⚡', 'Charging');
  if (ext === true) return badge('good', '⚡', dev.charging === false
                                   ? 'On mains — charge complete' : 'On mains');
  if (ext === false) return badge('idle', '▮', 'On battery');
  if (dev.charging === null && isWeighed(dev)) {
    return badge('idle', '?', 'Charge state unknown — no sense pin');
  }
  return '';
}

// See the Charging row above for why this is four states and not two.
function chargeText(dev, state) {
  // external_power lives in device_power, not device_overview -- the view it
  // belongs in is defined in three files that must agree, so it has its own for
  // now. See supabase/migrate_vbus_sense.sql.
  const row = (state.power || []).find(p => p.device_id === dev.device_id);
  const ext = row ? row.external_power : null;
  if (dev.charging != null) {
    if (dev.charging) return 'yes';
    // Not charging AND on mains is the terminated case -- the cell is full and
    // the charger has stopped, which is worth distinguishing from running down.
    return ext ? 'no — charged' : 'no';
  }
  if (ext == null) return 'unknown — no sense pin';
  return ext ? 'on mains' : 'on battery';
}

function weightCard(dev) {
  const w = deviceWeight(dev);
  const stale = deviceOffline(dev);

  // Everything below the headline: the figures that explain it. net_counts is
  // what the cells are actually producing, and it is the ONLY measurement an
  // uncalibrated station has -- so it is shown even when there are no grams,
  // which is exactly when it matters most.
  const detail = [];
  if (dev.counts_per_gram != null) {
    detail.push(kv('Calibration', `${Number(dev.counts_per_gram).toFixed(3)} counts/g`));
  } else if (dev.reported) {
    detail.push(kv('Calibration', 'never calibrated'));
  }
  if (dev.net_counts != null) {
    detail.push(kv('Raw', `${Number(dev.net_counts).toLocaleString()} counts`));
  }
  // A BUFFER'S two extra facts, each only when the row carries it: both are
  // NULL whenever there is no weight, and a dash there is the honest answer.
  // Unconfirmed is spelled out here because this is where somebody goes to
  // find out why the dashboard put a ? beside the count.
  const buffer = isBuffer(dev);
  if (buffer) {
    detail.push(kv('Bowls', dev.bowls == null ? '—'
      : bowlsText(dev.bowls) + (dev.bowls_confirmed === false
          ? ' — unconfirmed: remembered from before a power cycle' : '')));
    detail.push(kv('Gross', dev.gross_g == null ? '—'
      : `${fmtWeight(dev.gross_g)} (food + 2.5 kg per bowl)`));
  }

  return h('div', { class: 'card' },
    h('div', { class: 'chart-title' }, buffer ? 'Buffer now' : 'Counter now'),
    h('div', { style: 'display:flex;gap:1.2rem;align-items:center;margin-top:.5rem' },
      // One mark per cell. The direct analogue of the level ladder, and round
      // rather than rectangular so the two are not confused on a page that
      // may show either.
      cellColumn(dev.cells_online, cellsTotal(dev), true),
      h('div', {},
        h('div', {
          class: 'hero' + (stale && w.kind === 'weight' ? ' is-offline' : ''),
        }, w.text),
        h('div', { class: 'muted', style: 'font-size:.82rem' },
          stale
            ? `Last known — ${fmtRelative(dev.updated_at)}${w.note ? ` · ${w.note}` : ''}`
            : w.note || (buffer ? 'food on the buffer' : 'weighed at the counter')))),
    detail.length
      ? h('dl', { class: 'kv', style: 'margin-top:.7rem' }, ...detail)
      : null,
    h('div', { class: 'dim', style: 'font-size:.75rem;margin-top:.5rem;line-height:1.4' },
      ...(buffer ? [
        'One 200 kg cell under the platform. The figure is FOOD: the whole load ',
        'less 2.5 kg for each bowl the platform has counted, from the steps as ',
        'bowls go on and come off. After a power cycle the count is remembered ',
        'but unconfirmed until the next bowl moves or the platform reads empty. ',
        'Zero is a real weight: an empty platform reads 0.0 kg and means refill me.',
      ] : [
        'Three cells under one platform, and they SUM — so a cell dropping out ',
        'does not add noise, it makes the total read LOW. That is why fewer than ',
        'three reports no weight at all rather than a partial one. Zero is a real ',
        'weight: a tared, empty platform reads 0.0 kg and means refill me.',
      ])));
}

// ====================================================================
//  TRIAL HARNESS -- the manual estimate against the measured weight.
//
//  ##  TEMPORARY. ONE DEVICE. NOT PRODUCTION.  ##
//
//  This card is the experiment's readout. LDC-001 has a knob beside its load
//  cell; the attendant judges the vessel by eye and dials in a percentage,
//  and the cell keeps weighing the same food independently. What a reviewer
//  wants is the GAP, so the card shows both figures and the signed error
//  between them rather than making anyone subtract in their head.
//
//  SIGNED, not absolute. People round up, because a full-looking vessel is
//  the safe answer -- and a systematic lean is the most useful thing in the
//  dataset. An absolute error would average it away.
//
//  Delete this function, its call site, and the two fetches in app.js to
//  remove the harness from the dashboard.
// ====================================================================
function trialCard(dev, state, ctx) {
  const row = (state.trialFill || []).find(r => r.device_id === dev.device_id);
  if (!row || row.manual_fill_pct == null) return null;

  const pct = Number(row.manual_fill_pct);
  const ageS = row.manual_fill_age_s == null ? null : Number(row.manual_fill_age_s);
  // STALE IS A RESULT, NOT A NUISANCE. An estimate nobody refreshes is the
  // failure mode a knob-based system actually has, so the card says so rather
  // than presenting an old number as a current one.
  const stale = ageS != null && ageS >= 600;

  // Whichever meal is running. The capacity is per meal because the vessel is;
  // with none set the percentage cannot become kilograms, and the card says
  // that instead of inventing a denominator.
  const meal = state.quantity && state.quantity.length
    ? state.quantity[0].current_meal : null;
  const capRow = (state.trialCap || []).find(c => c.meal_type === meal);
  const capG = capRow ? Number(capRow.capacity_g) : null;

  const manualG = capG == null ? null : (pct / 100) * capG;
  const measuredG = row.weight_state === 'ok' && row.weight_g != null
    ? Number(row.weight_g) : null;
  const errG = (manualG != null && measuredG != null) ? manualG - measuredG : null;

  const kg = g => `${(g / 1000).toFixed(1)} kg`;

  const detail = [];
  detail.push(kv('Estimated', manualG == null
    ? `${pct}% — no vessel capacity set for ${meal || 'this meal'}`
    : `${pct}% of ${kg(capG)} = ${kg(manualG)}`));
  detail.push(kv('Measured', measuredG == null
    ? `no usable weight (${row.weight_state || 'unknown'})`
    : kg(measuredG)));
  if (errG != null) {
    // A PERCENTAGE OF ALMOST NOTHING IS NOT A PERCENTAGE. Divide a 9 kg
    // discrepancy by a scale reading 4 g and you get +224900%, which is
    // arithmetically correct and useless to everyone -- it says nothing about
    // the attendant's judgement and everything about the denominator.
    //
    // Two ways to get there and both are real: the counter is idle with no
    // vessel on it, or somebody set the full-vessel capacity wrong. Neither is
    // a measurement of estimation error, so neither earns a precise-looking
    // number.
    //
    // Below the floor the ratio is not reported at all -- the kilograms still
    // are, because those remain true. Above it, anything past 100% is shown as
    // a bound: beyond that the exact figure carries no information a person
    // would act on differently.
    const EMPTY_FLOOR_G = 200;
    let pctText = '';
    if (measuredG != null && measuredG < EMPTY_FLOOR_G) {
      pctText = ' — scale reads empty, so a ratio would be meaningless';
    } else if (measuredG > 0) {
      const pct = (errG / measuredG) * 100;
      pctText = pct > 100 ? ' (>+100%)'
              : pct < -100 ? ' (<-100%)'
              : ` (${pct >= 0 ? '+' : ''}${pct.toFixed(0)}%)`;
    }
    detail.push(kv('Error', `${errG >= 0 ? '+' : ''}${kg(errG)}${pctText}`));
  }
  if (ageS != null) {
    detail.push(kv('Estimate age', ageS < 60
      ? `${ageS} s`
      : `${Math.round(ageS / 60)} min${stale ? ' — overdue' : ''}`));
  }

  return h('div', { class: 'card' },
    h('div', { class: 'chart-title' }, 'Trial — knob vs scale'),
    h('div', { style: 'display:flex;gap:1.4rem;align-items:baseline;margin-top:.5rem' },
      h('div', { class: 'hero' + (stale ? ' is-offline' : '') }, `${pct}%`),
      h('div', { class: 'muted', style: 'font-size:.82rem' },
        manualG == null ? 'set a vessel capacity below' : kg(manualG))),
    h('dl', { class: 'kv', style: 'margin-top:.7rem' }, ...detail),
    capacityEditor(state, meal, ctx),
    h('div', { class: 'dim', style: 'font-size:.75rem;margin-top:.5rem;line-height:1.4' },
      'Temporary. The knob is a cheap alternative to a load cell and this card ',
      'is how the two get compared — the estimate feeds nothing, so the stock ',
      'and burn-rate figures elsewhere remain the measured ones. Error is ',
      'signed on purpose: a consistent lean one way is the finding, and an ',
      'absolute value would hide it.'));
}

// The vessel capacity, per meal, saved SERVER-SIDE so every browser and every
// reviewer sees the same denominator. A number kept in the page would be a
// different experiment per laptop.
function capacityEditor(state, meal, ctx) {
  if (!meal) return null;
  const cur = (state.trialCap || []).find(c => c.meal_type === meal);

  // EVERY ELEMENT IS LOOKED UP FROM THE LIVE DOM AT CLICK TIME, never captured
  // in the closure, and that is not defensiveness -- it is the bug this had.
  //
  // The dashboard re-renders on every poll through morphNode(), which KEEPS the
  // existing node and swaps in the new render's listeners. So a handler that
  // closed over the `input` it was built beside was holding the node morph had
  // just discarded: it read the value the render had put there and never the
  // one being typed into the element actually on screen. Typing 13 and pressing
  // Save saved 18, for ever, and the status text was written to a detached node
  // so the failure was invisible too.
  //
  // Looking the elements up through the event target sidesteps it entirely --
  // whatever node is on screen is the node the handler reads.
  const wrap = h('div', {
    dataset: { capEditor: '1' },
    style: 'margin-top:.6rem;display:flex;gap:.5rem;align-items:center;flex-wrap:wrap',
  });

  const input = h('input', {
    type: 'number', min: '0.1', max: '200', step: '0.1',
    value: cur ? (Number(cur.capacity_g) / 1000).toFixed(1) : '',
    placeholder: 'kg',
    dataset: { capInput: '1' },
    style: 'width:5.5rem',
  });
  const note = h('span', {
    class: 'dim', dataset: { capNote: '1' }, style: 'font-size:.75rem',
  }, '');

  const save = h('button', { class: 'ghost', onclick: async (ev) => {
    const box = ev.target.closest('[data-cap-editor]');
    if (!box) return;
    const live = box.querySelector('[data-cap-input]');
    const msg = box.querySelector('[data-cap-note]');
    const kg = parseFloat(live && live.value);
    if (!(kg > 0)) { if (msg) msg.textContent = 'enter a number of kilograms'; return; }
    if (msg) msg.textContent = 'saving…';
    try {
      const { error } = await ctx.client
        .from('trial_vessel_capacity')
        .upsert({ meal_type: meal, capacity_g: Math.round(kg * 1000) },
                { onConflict: 'meal_type' });
      if (error) throw error;
      if (msg) msg.textContent = 'saved';
      // Refetch, so the kilograms above are the ones the server now holds
      // rather than the ones the browser assumed it wrote.
      ctx.refresh();
    } catch (err) {
      // NAMED, not swallowed. A capacity that silently failed to save makes
      // every kilogram on this card wrong for everyone else, with the person
      // who typed it being the only one who cannot tell.
      if (msg) msg.textContent = `not saved: ${err.message || err}`;
    }
  } }, 'Save');

  wrap.append(
    h('span', { class: 'muted', style: 'font-size:.82rem' },
      `Full vessel at ${meal}:`),
    input, save, note);
  return wrap;
}

// ====================================================================
//  The load cell's history — a weight curve, where a bowl counter gets a
//  step chart.
//
//  The chart is the point of the whole table. A number on the Master card
//  says how much is left; only the curve says whether it is falling fast,
//  whether somebody refilled it twenty minutes ago, and whether the last
//  half hour looks like the one before it.
// ====================================================================
function renderWeightHistory(rowsDesc, dev, tz, hours) {
  const frag = document.createDocumentFragment();
  const rows = [...rowsDesc].reverse();     // oldest first for plotting

  // v = null wherever the state was not 'ok'. That is not a zero and it is not
  // a low reading -- it is the absence of a weight, and weightChart breaks the
  // line across it rather than drawing a plunge to the axis that never
  // happened.
  const points = rows.map(r => ({
    t: new Date(r.recorded_at).getTime(),
    v: r.weight_state === 'ok' ? r.weight_g : null,
    state: r.weight_state,
    reason: r.reason,
  }));

  const width = Math.max(300, (document.getElementById('view')?.clientWidth || 640) - 34);
  const buffer = isBuffer(dev);

  frag.append(h('div', { class: 'card', style: 'margin-bottom:.7rem' },
    weightChart({
      points, tz, width,
      title: buffer ? 'Food on this buffer' : 'Weight on this counter',
      subtitle: 'Sampled every two minutes and on every change. A gap is a '
              + 'stretch with no trustworthy weight, not an empty platform.',
    })));

  // What the curve cannot show: how much of the window had a usable reading
  // at all. A beautiful line over four samples is still four samples.
  const ok = rows.filter(r => r.weight_state === 'ok').length;
  const states = [...new Set(rows.map(r => r.weight_state))].filter(x => x !== 'ok');
  frag.append(h('div', { class: 'card', style: 'margin-bottom:.7rem' },
    h('dl', { class: 'kv' },
      kv('Samples', `${rows.length} in ${hours} h`),
      kv('With a weight', `${ok} of ${rows.length}`),
      states.length ? kv('Other states', states.join(', ')) : null)));

  // A buffer's rows carry its bowl count and gross load; a counter's never do,
  // so the two columns appear only where they can hold something.
  const table = h('table', {},
    h('thead', {}, h('tr', {},
      h('th', {}, 'Recorded'), h('th', {}, 'Reason'), h('th', { class: 'num' }, 'Weight'),
      ...(buffer ? [h('th', { class: 'num' }, 'Bowls'), h('th', { class: 'num' }, 'Gross')] : []),
      h('th', {}, 'State'), h('th', { class: 'num' }, 'Cells'),
      h('th', { class: 'num' }, 'Raw counts'), h('th', {}, 'Battery'),
      h('th', { class: 'num' }, 'Boot/seq'), h('th', {}, 'Delay'))),
    h('tbody', {}, ...rowsDesc.map(r => {
      const delayMs = new Date(r.received_at) - new Date(r.recorded_at);
      return h('tr', {},
        h('td', {}, fmtDateTime(r.recorded_at, tz)),
        h('td', {}, r.reason),
        // 0 g renders as 0.00, never as a dash: a measured empty platform is a
        // reading. Only a non-ok state has no number.
        h('td', { class: 'num' },
          r.weight_state === 'ok' ? `${(r.weight_g / 1000).toFixed(2)} kg` : '—'),
        ...(buffer ? [
          h('td', { class: 'num' }, r.bowls == null ? '—'
            : `${r.bowls}${r.bowls_confirmed === false ? '?' : ''}`),
          h('td', { class: 'num' }, r.gross_g == null ? '—'
            : `${(r.gross_g / 1000).toFixed(2)} kg`),
        ] : []),
        h('td', {}, r.weight_state),
        h('td', { class: 'num' }, r.cells_online ?? '—'),
        h('td', { class: 'num' },
          r.net_counts == null ? '—' : Number(r.net_counts).toLocaleString()),
        h('td', {}, r.battery_level ?? '—'),
        h('td', { class: 'num' }, `${r.boot_id ?? '—'}/${r.seq}`),
        h('td', {}, Number.isFinite(delayMs) ? `${Math.round(delayMs / 1000)}s` : '—'));
    })));
  // COLLAPSED, matching the bowl-counter page above rather than inventing a
  // second treatment. The chart is what the page is for; the table is the
  // evidence behind it, wanted occasionally and by one person. Left open it
  // pushed the chart off the top of the screen on any window worth reading.
  //
  // ui.js's patcher preserves the `open` attribute across a re-render, so a
  // table somebody expanded stays expanded through the 15 s poll.
  frag.append(h('details', { class: 'table-view section' },
    h('summary', {}, `Show all ${rowsDesc.length} samples`),
    h('div', { class: 'table-wrap card',
               style: 'margin-top:.5rem;max-height:60vh;overflow:auto' }, table)));

  return frag;
}
