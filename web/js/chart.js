// ====================================================================
//  Two small SVG figures for the device page.
//
//  status_events holds one row per REAL CHANGE, never per report — so the
//  value between two rows is genuinely constant and a step-after line is
//  the honest shape. A smoothed or point-to-point line would invent a
//  ramp across steady state.
// ====================================================================

import { h } from './ui.js';
import { fmtDateTime, fmtClock } from './domain.js';

const SVG_NS = 'http://www.w3.org/2000/svg';

function s(tag, attrs = {}, ...kids) {
  const el = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    el.setAttribute(k, String(v));
  }
  el.append(...kids.filter(Boolean));
  return el;
}

const STATUS_STYLE = {
  ok:            { css: 'var(--good)',     glyph: '●', label: 'OK' },
  degraded:      { css: 'var(--warning)',  glyph: '◐', label: 'Degraded' },
  discontiguous: { css: 'var(--critical)', glyph: '▲', label: 'Fault' },
};

/**
 * Step chart of bowl count over time.
 *
 * @param points  oldest-first [{ t: ms, v: number, status, reason }]
 * @param yMax    ceiling, from the physical stack height — never inferred
 * @param now     right edge; the last value holds until it
 */
export function stepChart({ points, yMax = 4, now = Date.now(), width = 640, tz,
                            title, subtitle, height = 240 }) {
  // 240, up from 152. The device page is a page somebody has DELIBERATELY
  // opened to read a history -- unlike the cards on Stock and Master, which
  // are scanned. At 152 a four-bowl range squeezed into ~116 px of plot and
  // every change read as the same size step; the extra height is what makes a
  // slow drain distinguishable from a sudden one.
  const H = height, PAD = { t: 12, r: 44, b: 24, l: 28 };
  const W = Math.max(280, width);
  const iw = W - PAD.l - PAD.r;
  const ih = H - PAD.t - PAD.b;

  const wrap = h('div', { class: 'chart-card' });
  if (title) wrap.append(h('div', { class: 'chart-title' }, title));
  if (subtitle) wrap.append(h('div', { class: 'chart-sub' }, subtitle));

  if (!points.length) {
    wrap.append(h('div', { class: 'empty' }, 'No recorded changes in this window.'));
    return wrap;
  }

  const t0 = points[0].t;
  const t1 = Math.max(now, points[points.length - 1].t);
  const span = Math.max(1, t1 - t0);
  const top = Math.max(yMax, ...points.map(p => p.v ?? 0));

  const x = t => PAD.l + ((t - t0) / span) * iw;
  const y = v => PAD.t + ih - (v / (top || 1)) * ih;

  const svg = s('svg', {
    width: W, height: H, viewBox: `0 0 ${W} ${H}`,
    role: 'img', 'aria-label': `${title || 'Bowl count'} over time`,
  });

  // Grid: solid hairlines, one step off the surface, never dashed.
  for (let v = 0; v <= top; v++) {
    svg.append(s('line', {
      x1: PAD.l, x2: W - PAD.r, y1: y(v), y2: y(v),
      class: v === 0 ? 'axisline' : 'gridline',
    }));
    svg.append(s('text', { x: PAD.l - 6, y: y(v) + 3.5, 'text-anchor': 'end', class: 'tick' }, String(v)));
  }

  // Time ticks — a handful, at the ends and between.
  const TICKS = W > 520 ? 5 : 3;
  for (let i = 0; i < TICKS; i++) {
    const t = t0 + (span * i) / (TICKS - 1);
    const anchor = i === 0 ? 'start' : i === TICKS - 1 ? 'end' : 'middle';
    svg.append(s('text', {
      x: x(t), y: H - 8, 'text-anchor': anchor, class: 'tick',
    }, fmtClock(new Date(t).toISOString(), tz)));
  }

  // The line, broken across spans the count cannot be trusted through.
  // `discontiguous` is not a low reading — it is not a reading at all, so the
  // line must not bridge it.
  const runs = [];
  let run = [];
  for (const p of points) {
    if (p.status === 'discontiguous' || p.v == null) {
      if (run.length) runs.push(run);
      run = [];
    } else {
      run.push(p);
    }
  }
  if (run.length) runs.push(run);

  runs.forEach((r, ri) => {
    let d = '';
    r.forEach((p, i) => {
      const px = x(p.t), py = y(p.v);
      d += i === 0 ? `M${px},${py}` : `L${px},${py}`;
      const next = r[i + 1];
      // Extend the tread to the next change, or to `now` for the final run.
      const endT = next ? next.t : (ri === runs.length - 1 ? t1 : p.t);
      if (endT > p.t) d += `L${x(endT)},${py}`;
    });
    svg.append(s('path', {
      d, fill: 'none', stroke: 'var(--series-1)', 'stroke-width': 2,
      'stroke-linejoin': 'round', 'stroke-linecap': 'round',
    }));
  });

  // End marker + the one direct label. Labelling every step would be chaos;
  // the axis and the tooltip carry the rest.
  const last = points[points.length - 1];
  if (last.status !== 'discontiguous' && last.v != null) {
    svg.append(s('circle', {
      cx: x(t1), cy: y(last.v), r: 5,
      fill: 'var(--series-1)', stroke: 'var(--surface)', 'stroke-width': 2,
    }));
    svg.append(s('text', {
      x: x(t1) + 10, y: y(last.v) + 4, class: 'tick',
      fill: 'var(--ink)', 'font-size': 12, 'font-weight': 600,
    }, String(last.v)));
  }

  const cursor = s('line', {
    y1: PAD.t, y2: PAD.t + ih, class: 'gridline', stroke: 'var(--axis)', opacity: 0,
  });
  svg.append(cursor);

  const chartWrap = h('div', { class: 'chart-wrap' }, svg);
  const tip = h('div', { class: 'chart-tip', hidden: true });
  chartWrap.append(tip);

  // Hover reads the value; it never gates it — the table view below has
  // every row.
  const nearest = clientX => {
    const rect = svg.getBoundingClientRect();
    const px = ((clientX - rect.left) / rect.width) * W;
    const t = t0 + ((px - PAD.l) / iw) * span;
    let best = points[0];
    for (const p of points) if (p.t <= t) best = p; else break;
    return { p: best, px: Math.max(PAD.l, Math.min(W - PAD.r, px)) };
  };

  const show = clientX => {
    const { p, px } = nearest(clientX);
    cursor.setAttribute('x1', px);
    cursor.setAttribute('x2', px);
    cursor.setAttribute('opacity', 1);
    const st = STATUS_STYLE[p.status] || { label: p.status };
    tip.innerHTML = '';
    tip.append(
      h('div', {}, h('b', {}, p.status === 'discontiguous' ? 'No valid count' : `${p.v} bowls`)),
      h('div', { class: 'dim' }, `${st.label}${p.reason ? ` · ${p.reason}` : ''}`),
      h('div', { class: 'dim' }, fmtDateTime(new Date(p.t).toISOString(), tz)),
    );
    tip.style.left = `${(px / W) * 100}%`;
    tip.style.top = `${PAD.t + ih * 0.35}px`;
    tip.hidden = false;
  };
  const hide = () => { tip.hidden = true; cursor.setAttribute('opacity', 0); };

  chartWrap.addEventListener('pointermove', e => show(e.clientX));
  chartWrap.addEventListener('pointerdown', e => show(e.clientX));
  chartWrap.addEventListener('pointerleave', hide);

  wrap.append(chartWrap);
  return wrap;
}

/**
 * Reading-status band. The palette is inverted from the old one on field
 * request — red is reserved for the battery, and silence is quiet:
 *   green \u25cf reading OK          blue  \u25b2 impossible reading (fault)
 *   amber \u25d0 degraded            red   \u25ae battery low/critical
 *   grey  \u2715 offline during service (live tail only, from the server's flag)
 *   blank      powered off / not reporting outside service
 *
 * Honesty rules with a change-only history:
 *   - A long gap ending in a `boot` row means the device was OFF for most of
 *     it: the prior state is held 10 minutes, the rest stays blank track.
 *   - The live tail is held to `updated_at` (the 20 s heartbeat proves
 *     liveness even when nothing changes), then grey if the server says
 *     `offline` \u2014 that flag is service-window aware, so dark hours are
 *     blank, never grey.
 */
const TIMELINE_STYLE = {
  ok:       { css: 'var(--good)',        glyph: '\u25cf', label: 'reading OK' },
  fault:    { css: 'var(--series-1)',    glyph: '\u25b2', label: 'fault' },
  degraded: { css: 'var(--warning-ink)', glyph: '\u25d0', label: 'degraded' },
  battery:  { css: 'var(--critical)',    glyph: '\u25ae', label: 'battery low' },
  offline:  { css: 'var(--axis)',        glyph: '\u2715', label: 'offline in service' },
};
const HOLD_MS = 10 * 60_000;
const BOOT_GAP_MS = 15 * 60_000;

export function statusTimeline({ points, now = Date.now(), width = 640, tz, dev }) {
  const H = 26, W = Math.max(280, width), PAD = { l: 28, r: 44 };
  const iw = W - PAD.l - PAD.r;

  const wrap = h('div', { class: 'chart-card' });
  wrap.append(h('div', { class: 'chart-title' }, 'Reading status'));
  if (!points.length) {
    wrap.append(h('div', { class: 'empty' }, 'No recorded changes in this window.'));
    return wrap;
  }

  const t0 = points[0].t;
  const t1 = Math.max(now, points[points.length - 1].t);
  const span = Math.max(1, t1 - t0);
  const x = t => PAD.l + ((t - t0) / span) * iw;

  const keyOf = p =>
    p.status === 'discontiguous' ? 'fault'
    : p.status === 'degraded' ? 'degraded'
    : (p.battery === 'low' || p.battery === 'critical') ? 'battery'
    : 'ok';

  const segs = [];
  points.forEach((p, i) => {
    const next = points[i + 1];
    if (next) {
      const wasOff = next.reason === 'boot' && next.t - p.t > BOOT_GAP_MS;
      segs.push({ from: p.t, to: wasOff ? Math.min(next.t, p.t + HOLD_MS) : next.t, key: keyOf(p) });
    } else {
      const alive = Math.min(t1, Math.max(p.t,
        dev?.updated_at ? new Date(dev.updated_at).getTime() : p.t));
      segs.push({ from: p.t, to: Math.min(t1, Math.max(alive, p.t + 60_000)), key: keyOf(p) });
      if (alive < t1 && dev?.offline) segs.push({ from: alive, to: t1, key: 'offline' });
    }
  });

  const svg = s('svg', { width: W, height: H, viewBox: `0 0 ${W} ${H}`, role: 'img',
                         'aria-label': 'Reading status over time' });
  // The track: blank time shows as empty rail, not as nothing.
  svg.append(s('rect', { x: PAD.l, y: 4, width: iw, height: 14, rx: 3, fill: 'var(--sunken)' }));

  for (const seg of segs) {
    const st = TIMELINE_STYLE[seg.key];
    const w = Math.max(1.5, x(seg.to) - x(seg.from));
    svg.append(s('rect', {
      x: x(seg.from), y: 4, width: w, height: 14, rx: 3, fill: st.css,
    }, s('title', {}, `${st.label} \u00b7 ${fmtDateTime(new Date(seg.from).toISOString(), tz)} \u2192 ${fmtDateTime(new Date(seg.to).toISOString(), tz)}`)));
  }

  wrap.append(h('div', { class: 'chart-wrap' }, svg));

  const seen = new Set(segs.map(g => g.key));
  const legend = h('div', { class: 'legend' });
  for (const key of ['ok', 'fault', 'degraded', 'battery', 'offline']) {
    if (!seen.has(key)) continue;
    const st = TIMELINE_STYLE[key];
    legend.append(h('span', {},
      h('i', { style: `background:${st.css}` }),
      `${st.glyph} ${st.label}`));
  }
  legend.append(h('span', { class: 'dim' }, 'blank = powered off / not reporting'));
  wrap.append(legend);
  return wrap;
}

export { STATUS_STYLE };

/**
 * Weight over time — the load cell's counterpart to stepChart().
 *
 * DELIBERATELY NOT stepChart WITH A DIFFERENT UNIT, for two reasons that both
 * matter:
 *
 *   * stepChart draws one gridline PER INTEGER (`for v = 0; v <= top; v++`).
 *     That is right for a bowl count, which tops out at 4. For grams it would
 *     draw twenty thousand gridlines.
 *
 *   * A bowl count is a STEP: it holds a value until a sensor says otherwise,
 *     so the tread between changes is real and drawing it flat is honest. A
 *     weight is CONTINUOUS and was sampled — the line between two samples is
 *     interpolation, and a step would claim the counter sat still and then
 *     jumped, which is not what happened.
 *
 * Same visual language otherwise: hairline grid, ticks at the ends, and the
 * line BROKEN wherever the reading cannot be trusted. `weight_state != 'ok'`
 * is not a low weight, it is not a weight at all, so the line must not bridge
 * it — exactly as stepChart refuses to bridge `discontiguous`.
 */
export function weightChart({ points, series, now = Date.now(), width = 640, tz,
                              title, subtitle, height = 260 }) {
  // SEVERAL LINES ON ONE AXIS (Statistics: the D+M+T total and each area) when
  // `series` is given: [{ points, label, color, width, cls }]. The first series
  // is the one whose last value is called out. Without it this is the original
  // single-series chart, unchanged for its callers.
  const list = series || [{ points, color: 'var(--series-1)', width: 2 }];
  const all = list.flatMap(l => l.points);
  // t: 20 rather than 12, to clear the unit label. At 12 the "kg" sat exactly
  // on y(top) -- the topmost gridline's own number -- and the two overprinted.
  //
  // Taller than the bowl chart by 20 px: a weight is continuous and its
  // interesting feature is the SLOPE, which a squat plot flattens toward
  // horizontal whatever the data does.
  const H = height, PAD = { t: 20, r: 44, b: 24, l: 46 };
  const W = Math.max(280, width);
  const iw = W - PAD.l - PAD.r;
  const ih = H - PAD.t - PAD.b;

  const wrap = h('div', { class: 'chart-card' });
  if (title) wrap.append(h('div', { class: 'chart-title' }, title));
  if (subtitle) wrap.append(h('div', { class: 'chart-sub' }, subtitle));

  const usable = all.filter(p => p.v != null);
  if (!usable.length) {
    wrap.append(h('div', { class: 'empty' },
      'No weight recorded in this window.'));
    return wrap;
  }

  const t0 = Math.min(...all.map(p => p.t));
  const t1 = Math.max(now, ...all.map(p => p.t));
  const span = Math.max(1, t1 - t0);

  // A NICE ceiling, not the maximum. An axis topping out at 1,213 g puts every
  // tick on a number nobody can hold in their head; rounding up to the next
  // 0.5 kg makes the gridlines readable and costs a little headroom.
  //
  // Tiers up to a 200 kg buffer platform. With 10 kg as the last step, a full
  // buffer drew twenty gridlines -- a page of ruled paper with a line on it.
  const peak = Math.max(...usable.map(p => p.v), 0);
  const stepG = peak <= 2000 ? 500 : peak <= 10000 ? 2000 : peak <= 40000 ? 5000
              : peak <= 100000 ? 10000 : peak <= 200000 ? 25000 : 50000;
  const top = Math.max(stepG, Math.ceil(peak / stepG) * stepG);

  const x = t => PAD.l + ((t - t0) / span) * iw;
  const y = v => PAD.t + ih - (v / (top || 1)) * ih;

  const svg = s('svg', {
    width: W, height: H, viewBox: `0 0 ${W} ${H}`,
    role: 'img', 'aria-label': `${title || 'Weight'} over time`,
  });

  for (let v = 0; v <= top; v += stepG) {
    svg.append(s('line', {
      x1: PAD.l, x2: W - PAD.r, y1: y(v), y2: y(v),
      class: v === 0 ? 'axisline' : 'gridline',
    }));
    svg.append(s('text', {
      x: PAD.l - 6, y: y(v) + 3.5, 'text-anchor': 'end', class: 'tick',
    }, `${(v / 1000).toFixed(v % 1000 === 0 ? 0 : 1)}`));
  }
  svg.append(s('text', {
    x: PAD.l - 6, y: PAD.t - 9, 'text-anchor': 'end', class: 'tick',
  }, 'kg'));

  const TICKS = W > 520 ? 5 : 3;
  for (let i = 0; i < TICKS; i++) {
    const t = t0 + (span * i) / (TICKS - 1);
    svg.append(s('text', {
      x: x(t), y: H - 8,
      'text-anchor': i === 0 ? 'start' : i === TICKS - 1 ? 'end' : 'middle',
      class: 'tick',
    }, fmtClock(new Date(t).toISOString(), tz)));
  }

  // Drawn last-first, so the first series (the total) lies on top.
  for (const l of [...list].reverse()) {
    // Break the line wherever the scale had no trustworthy figure.
    const runs = [];
    let run = [];
    for (const p of l.points) {
      if (p.v == null) { if (run.length) runs.push(run); run = []; }
      else run.push(p);
    }
    if (run.length) runs.push(run);

    for (const r of runs) {
      if (r.length === 1) {
        // A lone sample is a dot, not a line. Drawing a zero-length path would
        // render nothing and read as missing data.
        svg.append(s('circle', {
          cx: x(r[0].t), cy: y(r[0].v), r: 2.5, fill: l.color, class: l.cls || null,
        }));
        continue;
      }
      // ATTRIBUTES, NOT A CLASS, and this is not a style preference -- it was a
      // bug. There is no `.series` rule anywhere in app.css; stepChart above sets
      // fill/stroke directly on the element, and only the grid lines are styled
      // by class. A path with a class nobody defines gets SVG's DEFAULT fill of
      // BLACK, so the chart rendered as a solid black region under the curve
      // rather than as a line -- which reads as a broken chart, not as a
      // stylesheet miss. (`cls` only supplies --area-accent for `color`.)
      svg.append(s('path', {
        d: r.map((p, i) => `${i ? 'L' : 'M'}${x(p.t)},${y(p.v)}`).join(''),
        fill: 'none', stroke: l.color, 'stroke-width': l.width || 2, class: l.cls || null,
        'stroke-linejoin': 'round', 'stroke-linecap': 'round',
      }));
    }
  }

  // The last known value of the first series, called out at the right edge the
  // way stepChart does.
  const firstUsable = list[0].points.filter(p => p.v != null);
  const last = (firstUsable.length ? firstUsable : usable).slice(-1)[0];
  svg.append(s('text', {
    x: W - PAD.r + 6, y: y(last.v) + 3.5, class: 'tick', 'text-anchor': 'start',
  }, `${(last.v / 1000).toFixed(2)}`));

  wrap.append(svg);
  if (series && series.some(l => l.label)) {
    wrap.append(h('div', { class: 'legend-line' },
      ...series.filter(l => l.label).map(l => h('span', { class: l.cls || null },
        h('i', { style: `background:${l.color}` }), ` ${l.label} `))));
  }
  return wrap;
}
