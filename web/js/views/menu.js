// ====================================================================
//  Menu — what each slot serves, per meal per day.
//
//  A food slot is a fixed physical position painted on the station; the
//  dish in it changes three times a day. The mapping is keyed by date so a
//  past bowl count stays joinable to the dish that was actually there.
//
//  meal_mapping_preload() inherits the most recent previous menu for the
//  same location and meal, and flags whether what you are looking at is
//  SAVED or merely INHERITED. That flag is not decoration: a preloaded form
//  is pixel-identical to a saved one, so without acting on it an admin who
//  agrees with every inherited dish and walks away leaves no row behind.
// ====================================================================

import { h, badge, banner, toast, confirmAction, fillSlot } from '../ui.js';
import { unwrap, describeError } from '../supa.js';
import {
  LOCATION_NAMES, SERVING_LOCATIONS, MEAL_TYPES, FOOD_SLOTS, WEEKDAYS,
  weekdayOf, serviceDate, addDays, mealAtLocalClock, fmtDay, serviceState,
} from '../domain.js';

/** Last preload per (location, meal, date).
 *  A redraw renders straight from this, so navigating or refreshing does not
 *  drop the form back to "Loading menu…" and lose what is on screen. */
const preloadCache = new Map();
const SLOT_ID = 'menu-body';

/** Areas selected in the daily editor. Multi-select; at least one. The old
 *  single `loc` param is honoured so old links and habits keep working. */
function parseLocs(params) {
  const raw = params.get('locs') ?? params.get('loc') ?? '';
  const picked = raw.split(',').map(x => x.trim()).filter(l => SERVING_LOCATIONS.includes(l));
  return picked.length ? [...new Set(picked)] : [...SERVING_LOCATIONS];
}

export function renderMenu(state, params, ctx) {
  const { tz } = serviceState(state.devices);
  const mode = params.get('mode') === 'week' ? 'week' : 'date';
  const meal = MEAL_TYPES.includes(params.get('meal')) ? params.get('meal') : mealAtLocalClock(tz);
  const date = /^\d{4}-\d{2}-\d{2}$/.test(params.get('date') || '') ? params.get('date') : serviceDate(tz);
  const day = /^[0-6]$/.test(params.get('day') || '')
    ? Number(params.get('day'))
    : weekdayOf(serviceDate(tz));
  const loc = SERVING_LOCATIONS.includes(params.get('loc')) ? params.get('loc') : 'D';  // weekly mode
  const locs = parseLocs(params);                                                       // daily mode

  // Navigating to the hash you are already on fires no event, so a save that
  // "reloads by navigating to itself" would silently do nothing — fall back
  // to an explicit re-render. Null patch values DELETE their key, or the URL
  // would collect literal "null"s.
  const go = (patch) => {
    const q = mode === 'week'
      ? { loc, meal, day, mode }
      : { locs: locs.join(','), meal, date };
    Object.assign(q, patch);
    for (const k of Object.keys(q)) if (q[k] == null) delete q[k];
    const next = `#/menu?${new URLSearchParams(q)}`;
    if (location.hash === next) ctx.rerender();
    else location.hash = next;
  };

  const frag = document.createDocumentFragment();

  // Two modes of one tab, not a fifth tab: the tab bar is the triage strip
  // someone watches through a service, and staying on the `menu` route keeps
  // the poll-suppression that stops a background refresh wiping typing.
  frag.append(h('div', { class: 'toolbar' },
    h('button', {
      class: 'ghost', 'aria-pressed': String(mode === 'date'),
      onclick: () => go({ mode: null }),
    }, 'Daily menu'),
    h('button', {
      class: 'ghost', 'aria-pressed': String(mode === 'week'),
      onclick: () => go({ mode: 'week' }),
    }, 'Weekly template')));

  if (mode === 'week') {
    frag.append(renderWeekMode(state, ctx, { loc, meal, day, tz, go }));
    return frag;
  }

  // The daily page is a VERIFICATION surface: with the morning cron (or a
  // weekly Apply) filling menus, staff come here to eyeball the current meal
  // across every area at once. So: meals as one-tap capsules (one at a time),
  // areas as multi-select capsules, and one column in the grid per area.
  frag.append(h('div', { class: 'toolbar' },
    ...MEAL_TYPES.map(m => h('button', {
      class: 'ghost', 'aria-pressed': String(m === meal),
      onclick: () => go({ meal: m }),
    }, m))));

  frag.append(h('div', { class: 'toolbar' },
    ...SERVING_LOCATIONS.map(l => {
      const on = locs.includes(l);
      return h('button', {
        class: 'ghost', 'aria-pressed': String(on),
        onclick: () => {
          const next = on ? locs.filter(x => x !== l) : [...locs, l];
          if (!next.length) { toast('Keep at least one area selected'); return; }
          go({ locs: SERVING_LOCATIONS.filter(x => next.includes(x)).join(',') });
        },
      }, LOCATION_NAMES[l]);
    }),
    h('span', { class: 'grow' }),
    h('button', { class: 'ghost', onclick: () => go({ date: addDays(date, -1) }), title: 'Previous day' }, '‹'),
    h('input', { type: 'date', value: date, 'aria-label': 'Service date', style: 'max-width:11rem',
                 onchange: e => e.target.value && go({ date: e.target.value }) }),
    h('button', { class: 'ghost', onclick: () => go({ date: addDays(date, 1) }), title: 'Next day' }, '›'),
    h('button', { class: 'ghost', onclick: () => go({ date: serviceDate(tz) }) }, 'Today')));

  const allCached = locs.every(l => preloadCache.get(`${l}|${meal}|${date}`));
  const build = () => dailyGrid(ctx, state, { locs, meal, date, tz, go });
  const box = h('div', { id: SLOT_ID },
    allCached ? build() : h('div', { class: 'empty' }, 'Loading menu…'));
  frag.append(box);

  // Guard the async fill both ways: a response for a hash that has moved on
  // would paint the wrong areas under the current capsules, and a response
  // identical to the cached render would wipe in-progress typing for nothing.
  const issuedFor = location.hash;
  Promise.all(locs.map(l =>
    load(ctx.client, l, meal, date).then(r => ({ l, r }))))
    .then(results => {
      let changed = !allCached;
      for (const { l, r } of results) {
        const k = `${l}|${meal}|${date}`;
        const prev = preloadCache.get(k);
        if (!prev
            || JSON.stringify(prev.rows) !== JSON.stringify(r.rows)
            || JSON.stringify([...(prev.tpl || [])]) !== JSON.stringify([...(r.tpl || [])])) {
          changed = true;
        }
        preloadCache.set(k, r);
      }
      if (location.hash !== issuedFor || !changed) return;
      fillSlot(SLOT_ID, box, build());
    })
    .catch(err => {
      if (location.hash !== issuedFor) return;
      fillSlot(SLOT_ID, box, banner('critical', '✕', describeError(err)));
    });

  return frag;
}

async function load(client, loc, meal, date) {
  const rows = unwrap(await client.rpc('meal_mapping_preload', {
    p_location: loc, p_meal_type: meal, p_meal_date: date,
  })) || [];

  // The weekly template for this date's weekday — used to annotate overrides,
  // computed live rather than stamped, because a stored provenance flag lies
  // the moment the template is edited. Tolerates the table not existing.
  let tpl = null;
  try {
    const t = await client.from('meal_menu_template')
      .select('food_slot, food_name')
      .eq('location', loc).eq('meal_type', meal)
      .eq('weekday', weekdayOf(date));
    if (!t.error) tpl = new Map((t.data || []).map(r => [Number(r.food_slot), r.food_name]));
  } catch { /* pre-migration database */ }
  return { rows, tpl };
}

function dailyGrid(ctx, state, { locs, meal, date, tz, go }) {
  const frag = document.createDocumentFragment();
  const inputsByLoc = new Map();

  frag.append(h('div', { class: 'chart-sub', style: 'margin:.5rem 0 0' },
    `${meal} · ${fmtDay(date)}. Slot numbers are the painted positions on the `,
    'stations. Blank a field and Save to remove that dish.'));

  const grid = h('div', { class: 'menu-grid' });
  for (const l of locs) {
    const { rows = [], tpl = null } = preloadCache.get(`${l}|${meal}|${date}`) || {};
    grid.append(menuColumn(state, { loc: l, meal, date, rows, tpl, inputsByLoc }));
  }
  frag.append(grid);

  const status = h('span', { class: 'dim', style: 'font-size:.8rem' });
  const saveBtn = h('button', { class: 'primary', type: 'button' },
    locs.length === 1 ? 'Save menu' : `Save all ${locs.length} areas`);
  saveBtn.addEventListener('click', async () => {
    saveBtn.disabled = true;
    status.textContent = 'Saving…';
    const failures = [];
    let dishes = 0;
    for (const l of locs) {
      try {
        dishes += await saveArea(ctx, {
          loc: l, meal, date, inputs: inputsByLoc.get(l)?.inputs || new Map(),
        });
        preloadCache.delete(`${l}|${meal}|${date}`);
      } catch (err) {
        failures.push(`${LOCATION_NAMES[l]}: ${describeError(err)}`);
      }
    }
    if (failures.length) {
      status.textContent = failures.join(' · ');
      toast(`Saved with errors — ${failures.length} area(s) failed`, true);
    } else {
      status.textContent = '';
      toast(`Saved ${dishes} dish${dishes === 1 ? '' : 'es'} across ${locs.length} area${locs.length === 1 ? '' : 's'}`);
    }
    saveBtn.disabled = false;
    go({});
  });

  frag.append(h('div', { class: 'sticky-actions' },
    saveBtn,
    h('button', { class: 'ghost', type: 'button', onclick: () => go({}) }, 'Reload'),
    status));

  frag.append(copyDayCard(ctx, { locs, date, tz }));
  return frag;
}

function menuColumn(state, { loc, meal, date, rows, tpl, inputsByLoc }) {
  const byNumber = new Map(rows.map(r => [Number(r.food_slot), r]));
  const deployedSlots = [...new Set(state.devices
    .filter(d => d.location === loc && d.food_slot != null)
    .map(d => Number(d.food_slot)))].sort((a, b) => a - b);
  const slots = [...new Set([...deployedSlots, ...byNumber.keys()])].sort((a, b) => a - b);
  if (!slots.length) slots.push(1, 2, 3, 4, 5);

  const isDraft = rows.length > 0 && rows.every(r => !r.is_saved);
  const sourceDate = rows.length ? rows[0].source_date : null;
  const fromTemplate = isDraft && sourceDate === date;

  // The column's one-line truth: recorded, or a draft (and from where), or
  // nothing. A draft is pixel-identical to a saved menu, so this line is what
  // stops an admin believing an unrecorded menu was recorded.
  const statusChip = !rows.length
    ? badge('idle', '○', 'nothing entered yet')
    : fromTemplate
      ? badge('warning', '◐', 'From the weekly template — not recorded yet',
          'These dishes are the template. Press Save to record them for this date.')
      : isDraft
        ? badge('warning', '◐', `Not saved for this date — carried over from ${fmtDay(sourceDate)}`)
        : badge('good', '✓', 'Saved');

  const col = h('div', { class: 'card menu-col' },
    h('div', { class: 'menu-col-head' },
      h('h3', {}, LOCATION_NAMES[loc]),
      statusChip));

  const inputs = new Map();
  inputsByLoc.set(loc, { inputs });
  for (const slot of slots) {
    const row = byNumber.get(slot);
    const tplName = tpl?.get(slot);
    const overridden = row?.is_saved && tplName != null && tplName !== row.food_name;
    const input = h('input', {
      type: 'text', value: row?.food_name || '', placeholder: 'No dish',
      'aria-label': `${LOCATION_NAMES[loc]} slot ${slot} dish`,
      autocomplete: 'off', maxlength: 60,
    });
    inputs.set(slot, input);
    col.append(h('div', { class: `row-form${row && !row.is_saved ? ' is-draft' : ''}` },
      h('span', { class: 'slotno' }, `${slot}${deployedSlots.includes(slot) ? '' : ' ·'}`),
      input,
      overridden ? badge('warning', '◆', 'override', `Template: ${tplName}`) : h('span', {})));
  }
  return col;
}

/** One area's save: upsert the filled slots, delete the blanked ones.
 *  Blanks are filtered because a CHECK rejects an empty food_name; an emptied
 *  field means "no dish here", which is a DELETE, not a blank row. */
async function saveArea(ctx, { loc, meal, date, inputs }) {
  const payload = [];
  const toDelete = [];
  for (const [slot, input] of inputs) {
    const name = input.value.trim();
    if (name) payload.push({ location: loc, meal_type: meal, meal_date: date, food_slot: slot, food_name: name });
    else toDelete.push(slot);
  }
  if (payload.length) {
    unwrap(await ctx.client.from('meal_food_mapping')
      .upsert(payload, { onConflict: 'location,meal_date,meal_type,food_slot' }));
  }
  if (toDelete.length) {
    unwrap(await ctx.client.from('meal_food_mapping').delete()
      .eq('location', loc).eq('meal_type', meal).eq('meal_date', date)
      .in('food_slot', toDelete));
  }
  return payload.length;
}

/** Copy the selected areas' whole day (all three meals) to another date. */
function copyDayCard(ctx, { locs, date, tz }) {
  const target = h('input', { type: 'date', value: addDays(date, 1), 'aria-label': 'Copy to date' });
  const btn = h('button', { class: 'ghost', type: 'button' }, 'Copy day');
  const note = h('span', { class: 'dim', style: 'font-size:.8rem' });

  btn.addEventListener('click', async () => {
    const to = target.value;
    if (!to || to === date) { toast('Pick a different date', true); return; }
    btn.disabled = true;
    note.textContent = 'Copying…';
    try {
      const rows = unwrap(await ctx.client.from('meal_food_mapping')
        .select('location, meal_type, food_slot, food_name')
        .in('location', locs).eq('meal_date', date)) || [];
      if (!rows.length) { note.textContent = 'Nothing saved on this day to copy.'; return; }
      unwrap(await ctx.client.from('meal_food_mapping').upsert(
        rows.map(r => ({ location: r.location, meal_date: to, meal_type: r.meal_type,
                         food_slot: r.food_slot, food_name: r.food_name })),
        { onConflict: 'location,meal_date,meal_type,food_slot' }));
      for (const l of locs) {
        for (const m of MEAL_TYPES) preloadCache.delete(`${l}|${m}|${to}`);
      }
      note.textContent = `Copied ${rows.length} dishes to ${fmtDay(to)}.`;
      toast(`Copied ${rows.length} dishes to ${fmtDay(to)}`);
    } catch (err) {
      note.textContent = '';
      toast(describeError(err), true);
    } finally {
      btn.disabled = false;
    }
  });

  return h('div', { class: 'card section' },
    h('div', { class: 'chart-title' }, 'Copy this day'),
    h('div', { class: 'chart-sub' },
      `All three meals for ${locs.map(l => LOCATION_NAMES[l]).join(', ')} on `,
      `${fmtDay(date)}, written to another date. Existing entries on the target `,
      'date are overwritten.'),
    h('div', { class: 'toolbar' }, target, btn, note));
}

// ====================================================================
//  Weekly template mode.
//
//  The template is CONFIGURATION that produces menu rows; it is never
//  itself the menu. The dashboard keeps reading meal_food_mapping and
//  nothing else — a weekday-keyed template has no date, so resolving
//  dishes from it directly would let a whole service pass with a dish
//  name on screen and no dated row behind it, permanently breaking the
//  "what was served last Tuesday" join. The template reaches the
//  dashboard two ways only: the daily editor's Save (which preloads from
//  it), and the Apply action below, which writes real dated rows.
// ====================================================================

const WEEK_SLOT_ID = 'menu-week-body';

/** All template rows for a location, cached per location. */
const templateCache = new Map();

async function loadTemplate(client, loc) {
  const res = await client.from('meal_menu_template')
    .select('weekday, meal_type, food_slot, food_name')
    .eq('location', loc);
  if (res.error) throw res.error;
  return res.data || [];
}

function renderWeekMode(state, ctx, { loc, meal, day, tz, go }) {
  const frag = document.createDocumentFragment();

  // Capsules, no dropdowns — same grammar as the daily page. The template is
  // edited one area at a time (it writes per-area rows), so area is
  // single-select here; Copy-area spreads it.
  frag.append(h('div', { class: 'toolbar' },
    ...SERVING_LOCATIONS.map(l => h('button', {
      class: 'ghost', 'aria-pressed': String(l === loc),
      onclick: () => go({ loc: l }),
    }, LOCATION_NAMES[l]))));
  frag.append(h('div', { class: 'toolbar' },
    ...MEAL_TYPES.map(m => h('button', {
      class: 'ghost', 'aria-pressed': String(m === meal),
      onclick: () => go({ meal: m }),
    }, m))));

  // Day chips. Sunday-first to match weekday 0 = Sunday end to end.
  const chips = h('div', { class: 'toolbar' });
  WEEKDAYS.forEach((name, i) => {
    chips.append(h('button', {
      class: 'ghost', 'aria-pressed': String(i === day),
      onclick: () => go({ day: i }),
    }, name.slice(0, 3)));
  });
  frag.append(chips);

  const cached = templateCache.get(loc);
  const box = h('div', { id: WEEK_SLOT_ID }, cached
    ? weekBody(state, ctx, { loc, meal, day, tz, go, all: cached })
    : h('div', { class: 'empty' }, 'Loading template…'));
  frag.append(box);

  // Same two guards as the daily editor: a fill for a hash that has moved on
  // would paint the wrong (loc, meal, day) under the current chips, and a fill
  // identical to the cached render would wipe in-progress typing for nothing.
  const issuedFor = location.hash;
  loadTemplate(ctx.client, loc)
    .then(all => {
      const unchanged = cached && JSON.stringify(cached) === JSON.stringify(all);
      templateCache.set(loc, all);
      if (location.hash !== issuedFor || unchanged) return;
      fillSlot(WEEK_SLOT_ID, box, weekBody(state, ctx, { loc, meal, day, tz, go, all }));
    })
    .catch(err => {
      if (location.hash !== issuedFor) return;
      fillSlot(WEEK_SLOT_ID, box, banner('critical', '✕',
        h('b', {}, 'Could not load the weekly template. '),
        `${describeError(err)} — if the table does not exist yet, run `,
        h('code', {}, 'supabase/weekly_menu_and_offline.sql'),
        ' in the Supabase SQL editor.'));
    });

  return frag;
}

function weekBody(state, ctx, { loc, meal, day, tz, go, all }) {
  const frag = document.createDocumentFragment();

  // Coverage map: 7 days x 3 meals, each cell the number of dishes entered.
  // Navigation and completeness check in one glance — the answer to "which
  // meals have I not typed in yet" without clicking through 21 cells.
  const counts = new Map();
  for (const r of all) {
    const k = `${r.weekday}|${r.meal_type}`;
    counts.set(k, (counts.get(k) || 0) + 1);
  }
  const map = h('div', { class: 'table-wrap card', style: 'padding:.5rem' });
  const tbl = h('table', {},
    h('thead', {}, h('tr', {},
      h('th', {}, ''),
      ...WEEKDAYS.map(w => h('th', {}, w.slice(0, 3))))),
    h('tbody', {}, ...MEAL_TYPES.map(m =>
      h('tr', {},
        h('td', {}, m),
        ...WEEKDAYS.map((w, i) => {
          const n = counts.get(`${i}|${m}`) || 0;
          return h('td', { class: 'num' },
            h('button', {
              class: 'ghost',
              style: 'min-height:32px;padding:.2rem .5rem;font-size:.78rem;'
                + (i === day && m === meal ? 'font-weight:700;' : '')
                + (n ? '' : 'opacity:.45;'),
              title: `${WEEKDAYS[i]} ${m}: ${n ? `${n} dishes` : 'nothing entered'}`,
              onclick: () => go({ day: i, meal: m }),
            }, n || '·'));
        })))));
  map.append(h('div', { class: 'chart-sub', style: 'margin-bottom:.4rem' },
    `${LOCATION_NAMES[loc]} — dishes entered per meal. Click a cell to edit it.`),
    tbl);
  frag.append(map);

  // The editor for one (day, meal): the same slot-row form as the daily
  // editor, writing to the template table instead.
  const mine = new Map(all
    .filter(r => r.weekday === day && r.meal_type === meal)
    .map(r => [Number(r.food_slot), r.food_name]));
  const deployedSlots = [...new Set(state.devices
    .filter(d => d.location === loc && d.food_slot != null)
    .map(d => Number(d.food_slot)))].sort((a, b) => a - b);
  const slots = [...new Set([...deployedSlots, ...mine.keys()])].sort((a, b) => a - b);
  if (!slots.length) slots.push(1, 2, 3, 4, 5);

  const list = h('div', { class: 'card section' });
  list.append(h('div', { class: 'chart-sub' },
    `${LOCATION_NAMES[loc]} · every ${WEEKDAYS[day]} · ${meal}. `,
    'This is the plan, not any particular date — the daily editor and Apply ',
    'turn it into recorded menus.'));

  const inputs = new Map();
  for (const slot of slots) {
    const input = h('input', {
      type: 'text', value: mine.get(slot) || '', placeholder: 'No dish',
      'aria-label': `Slot ${slot} dish`, autocomplete: 'off', maxlength: 60,
    });
    inputs.set(slot, input);
    list.append(h('div', { class: 'row-form' },
      h('span', { class: 'slotno' },
        `${slot}${deployedSlots.includes(slot) ? '' : ' ·'}`),
      input,
      h('span', {})));
  }
  frag.append(list);

  const status = h('span', { class: 'dim', style: 'font-size:.8rem' });
  const saveBtn = h('button', { class: 'primary', type: 'button' }, 'Save template');
  saveBtn.addEventListener('click', async () => {
    saveBtn.disabled = true;
    status.textContent = 'Saving…';
    try {
      const payload = [];
      const toDelete = [];
      for (const [slot, input] of inputs) {
        const name = input.value.trim();
        if (name) payload.push({ location: loc, weekday: day, meal_type: meal,
                                 food_slot: slot, food_name: name });
        else if (mine.has(slot)) toDelete.push(slot);
      }
      if (payload.length) {
        unwrap(await ctx.client.from('meal_menu_template')
          .upsert(payload, { onConflict: 'location,weekday,meal_type,food_slot' }));
      }
      if (toDelete.length) {
        // Same rule as the daily editor: an emptied field is a DELETE, never
        // a row holding a blank name.
        unwrap(await ctx.client.from('meal_menu_template').delete()
          .eq('location', loc).eq('weekday', day).eq('meal_type', meal)
          .in('food_slot', toDelete));
      }
      templateCache.delete(loc);
      toast('Template saved');
      go({});
    } catch (err) {
      status.textContent = '';
      toast(describeError(err), true);
    } finally {
      saveBtn.disabled = false;
    }
  });
  frag.append(h('div', { class: 'sticky-actions' }, saveBtn,
    h('button', { class: 'ghost', type: 'button', onclick: () => go({}) }, 'Reload'),
    status));

  frag.append(copyWeekdayCard(ctx, { loc, day, go }));
  frag.append(copyAreaCard(ctx, { loc, go }));
  frag.append(applyTemplateCard(ctx, { loc, tz, go }));
  return frag;
}

/** Copy this location's ENTIRE template (all days, all meals) to the other
 *  areas. The three areas serve an identical menu here, so without this a
 *  shared week must be typed three times — measured at ~3x the effort the
 *  template exists to remove. */
function copyAreaCard(ctx, { loc, go }) {
  const others = SERVING_LOCATIONS.filter(l => l !== loc);
  const target = h('select', { 'aria-label': 'Copy template to' },
    h('option', { value: 'all' },
      `both (${others.map(l => LOCATION_NAMES[l]).join(' + ')})`),
    ...others.map(l => h('option', { value: l }, LOCATION_NAMES[l])));
  const btn = h('button', { class: 'ghost', type: 'button' }, 'Copy area');
  const note = h('span', { class: 'dim', style: 'font-size:.8rem' });

  btn.addEventListener('click', async () => {
    const dests = target.value === 'all' ? others : [target.value];
    if (!confirmAction(
      `Replace the ENTIRE weekly template of ${dests.map(l => LOCATION_NAMES[l]).join(' and ')} `
      + `with ${LOCATION_NAMES[loc]}'s — every day, every meal?`)) return;
    btn.disabled = true;
    note.textContent = 'Copying…';
    try {
      const src = unwrap(await ctx.client.from('meal_menu_template')
        .select('weekday, meal_type, food_slot, food_name')
        .eq('location', loc)) || [];
      if (!src.length) { note.textContent = `${LOCATION_NAMES[loc]} has no template to copy yet.`; return; }
      // Delete-then-insert: "replace" means replace, not merge — a merge
      // would leave the target serving last week's leftovers in any slot the
      // source does not fill.
      unwrap(await ctx.client.from('meal_menu_template').delete()
        .in('location', dests));
      unwrap(await ctx.client.from('meal_menu_template').upsert(
        dests.flatMap(l => src.map(r => ({
          location: l, weekday: r.weekday, meal_type: r.meal_type,
          food_slot: r.food_slot, food_name: r.food_name,
        }))),
        { onConflict: 'location,weekday,meal_type,food_slot' }));
      for (const l of dests) templateCache.delete(l);
      note.textContent = `Copied ${src.length} dishes to ${dests.map(l => LOCATION_NAMES[l]).join(' and ')}.`;
      toast(`Template copied to ${dests.length} area${dests.length === 1 ? '' : 's'}`);
    } catch (err) {
      note.textContent = '';
      toast(describeError(err), true);
    } finally {
      btn.disabled = false;
    }
  });

  return h('div', { class: 'card section' },
    h('div', { class: 'chart-title' }, `Copy ${LOCATION_NAMES[loc]}'s template to another area`),
    h('div', { class: 'chart-sub' },
      'The whole week — all days and meals. The target area\'s template is ',
      'replaced, not merged. Their already-recorded dated menus are untouched.'),
    h('div', { class: 'toolbar' }, target, btn, note));
}

/** Copy one weekday's whole template (all three meals) to other weekdays —
 *  how a "same menu most days" week gets entered with one day's typing. */
function copyWeekdayCard(ctx, { loc, day, go }) {
  const target = h('select', { 'aria-label': 'Copy to' },
    h('option', { value: 'all' }, 'every other day'),
    ...WEEKDAYS.map((w, i) =>
      i === day ? null : h('option', { value: i }, w)));
  const btn = h('button', { class: 'ghost', type: 'button' }, 'Copy');
  const note = h('span', { class: 'dim', style: 'font-size:.8rem' });

  btn.addEventListener('click', async () => {
    btn.disabled = true;
    note.textContent = 'Copying…';
    try {
      const src = unwrap(await ctx.client.from('meal_menu_template')
        .select('meal_type, food_slot, food_name')
        .eq('location', loc).eq('weekday', day)) || [];
      if (!src.length) { note.textContent = `Nothing entered for ${WEEKDAYS[day]} yet.`; return; }
      const days = target.value === 'all'
        ? [0, 1, 2, 3, 4, 5, 6].filter(i => i !== day)
        : [Number(target.value)];
      // Delete-then-insert, because the card says "overwritten" and means it:
      // a bare upsert would MERGE, leaving target-day slots the source day
      // lacks — a copy that looks complete and serves last week's leftovers.
      unwrap(await ctx.client.from('meal_menu_template').delete()
        .eq('location', loc).in('weekday', days));
      const payload = days.flatMap(d => src.map(r => ({
        location: loc, weekday: d, meal_type: r.meal_type,
        food_slot: r.food_slot, food_name: r.food_name,
      })));
      unwrap(await ctx.client.from('meal_menu_template')
        .upsert(payload, { onConflict: 'location,weekday,meal_type,food_slot' }));
      templateCache.delete(loc);
      toast(`Copied ${WEEKDAYS[day]} to ${days.length} day${days.length === 1 ? '' : 's'}`);
      go({});
    } catch (err) {
      note.textContent = '';
      toast(describeError(err), true);
    } finally {
      btn.disabled = false;
    }
  });

  return h('div', { class: 'card section' },
    h('div', { class: 'chart-title' }, `Copy ${WEEKDAYS[day]}`),
    h('div', { class: 'chart-sub' },
      'All three meals of this weekday, copied within the template. ',
      'Existing template entries on the target days are overwritten.'),
    h('div', { class: 'toolbar' }, target, btn, note));
}

/** Freeze the template into real dated menus. This is what makes the
 *  dashboard show dish names: the views only ever read dated rows. */
function applyTemplateCard(ctx, { loc, tz, go }) {
  const today = serviceDate(tz);
  const from = h('input', { type: 'date', value: today, 'aria-label': 'From date' });
  const to = h('input', { type: 'date', value: addDays(today, 6), 'aria-label': 'To date' });
  const overwrite = h('input', { type: 'checkbox', id: 'tpl-overwrite',
                                 style: 'width:auto;min-height:0' });
  const btn = h('button', { class: 'primary', type: 'button' }, 'Apply to dates');
  const report = h('div', {});

  btn.addEventListener('click', async () => {
    if (overwrite.checked && !confirmAction(
      'Overwrite mode REPLACES every already-entered meal in this range with '
      + 'the template, including deliberate per-date changes. Continue?')) return;
    btn.disabled = true;
    report.replaceChildren(h('span', { class: 'dim' }, 'Applying…'));
    try {
      const rows = unwrap(await ctx.client.rpc('meal_template_apply', {
        p_location: loc, p_from: from.value, p_to: to.value,
        p_overwrite: overwrite.checked,
      })) || [];
      const written = rows.filter(r => !r.skipped);
      const skipped = rows.filter(r => r.skipped);
      const total = written.reduce((n, r) => n + r.written, 0);
      report.replaceChildren(
        banner(total ? 'info' : 'warning', total ? '✓' : '○',
          h('b', {}, `${total} dishes written across ${written.length} meal${written.length === 1 ? '' : 's'}. `),
          skipped.length
            ? `${skipped.length} meal${skipped.length === 1 ? '' : 's'} skipped — already entered for `
              + 'their dates (deliberate changes are never overwritten unless you tick overwrite).'
            : 'Nothing needed skipping.'));
      preloadCache.clear();   // dated menus just changed under the daily editor
      toast(`Template applied: ${total} dishes`);
    } catch (err) {
      report.replaceChildren(banner('critical', '✕',
        describeError(err).includes('function')
          ? `The apply function is missing — run supabase/weekly_menu_and_offline.sql first. (${describeError(err)})`
          : describeError(err)));
    } finally {
      btn.disabled = false;
    }
  });

  return h('div', { class: 'card section' },
    h('div', { class: 'chart-title' }, 'Apply the template to real dates'),
    h('div', { class: 'chart-sub' },
      `Writes ${LOCATION_NAMES[loc]}'s template into the recorded menus for a date `,
      'range — the dashboard only ever shows recorded menus. Meals already ',
      'entered for a date are left alone, so one-off changes survive. Past ',
      'dates are refused: the template must never rewrite history.'),
    h('div', { class: 'toolbar' },
      from, h('span', { class: 'dim' }, 'to'), to, btn),
    h('label', { style: 'display:flex;gap:.4rem;align-items:center;font-size:.8rem;color:var(--ink-2)' },
      overwrite, 'Overwrite meals that are already entered'),
    report);
}
