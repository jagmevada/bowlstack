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

import { h, banner, toast, confirmAction } from '../ui.js';
import { unwrap, describeError } from '../supa.js';
import {
  LOCATION_NAMES, SERVING_LOCATIONS, MEAL_TYPES, FOOD_SLOTS,
  serviceDate, addDays, mealAtLocalClock, fmtDay, serviceState,
} from '../domain.js';

/** Slots an admin opened by hand that have neither a stack nor a saved dish.
 *  Session-scoped: they exist only until something is saved against them. */
const manuallyAdded = new Map();

export function renderMenu(state, params, ctx) {
  const { tz } = serviceState(state.devices);
  const loc = SERVING_LOCATIONS.includes(params.get('loc')) ? params.get('loc') : 'D';
  const meal = MEAL_TYPES.includes(params.get('meal')) ? params.get('meal') : mealAtLocalClock(tz);
  const date = /^\d{4}-\d{2}-\d{2}$/.test(params.get('date') || '') ? params.get('date') : serviceDate(tz);

  // Navigating to the hash you are already on fires no event, so a save that
  // "reloads by navigating to itself" would silently do nothing. Fall back to
  // an explicit re-render in that case.
  const go = (patch) => {
    const next = `#/menu?${new URLSearchParams({ loc, meal, date, ...patch })}`;
    if (location.hash === next) ctx.rerender();
    else location.hash = next;
  };

  const frag = document.createDocumentFragment();

  const bar = h('div', { class: 'toolbar' },
    h('select', { 'aria-label': 'Area', onchange: e => go({ loc: e.target.value }) },
      ...SERVING_LOCATIONS.map(l =>
        h('option', { value: l, selected: l === loc }, LOCATION_NAMES[l]))),
    h('select', { 'aria-label': 'Meal', onchange: e => go({ meal: e.target.value }) },
      ...MEAL_TYPES.map(m => h('option', { value: m, selected: m === meal }, m))),
    h('button', { class: 'ghost', onclick: () => go({ date: addDays(date, -1) }), title: 'Previous day' }, '‹'),
    h('input', { type: 'date', value: date, 'aria-label': 'Service date',
                 onchange: e => e.target.value && go({ date: e.target.value }) }),
    h('button', { class: 'ghost', onclick: () => go({ date: addDays(date, 1) }), title: 'Next day' }, '›'),
    h('button', { class: 'ghost', onclick: () => go({ date: serviceDate(tz) }) }, 'Today'));
  frag.append(bar);

  const box = h('div', {}, h('div', { class: 'empty' }, 'Loading menu…'));
  frag.append(box);

  const deployedSlots = [...new Set(state.devices
    .filter(d => d.location === loc && d.food_slot != null)
    .map(d => Number(d.food_slot)))].sort((a, b) => a - b);

  load(ctx.client, loc, meal, date)
    .then(rows => box.replaceChildren(form(ctx, { loc, meal, date, tz, rows, deployedSlots, go })))
    .catch(err => box.replaceChildren(banner('critical', '✕', describeError(err))));

  return frag;
}

async function load(client, loc, meal, date) {
  return unwrap(await client.rpc('meal_mapping_preload', {
    p_location: loc, p_meal_type: meal, p_meal_date: date,
  })) || [];
}

function form(ctx, { loc, meal, date, tz, rows, deployedSlots, go }) {
  const frag = document.createDocumentFragment();
  const isDraft = rows.length > 0 && rows.every(r => !r.is_saved);
  const sourceDate = rows.length ? rows[0].source_date : null;

  if (!rows.length) {
    frag.append(banner('info', '○',
      h('b', {}, 'No menu yet. '),
      `Nothing has been entered for ${LOCATION_NAMES[loc]} ${meal} on any earlier date, `,
      'so this form starts blank.'));
  } else if (isDraft) {
    frag.append(banner('warning', '◐',
      h('b', {}, 'Not saved for this date. '),
      `Carried over from ${fmtDay(sourceDate)}. Review the dishes and press Save — `,
      'until you do, no menu exists for ', fmtDay(date), ' and the dashboard will show ',
      'no dish name.'));
  } else {
    frag.append(banner('info', '✓',
      h('b', {}, 'Saved. '), `This is the recorded ${meal} menu for ${fmtDay(date)}.`));
  }

  const byNumber = new Map(rows.map(r => [Number(r.food_slot), r]));
  const extraKey = `${loc}|${meal}|${date}`;
  const extra = manuallyAdded.get(extraKey) || new Set();
  const slots = [...new Set([...deployedSlots, ...byNumber.keys(), ...extra])].sort((a, b) => a - b);
  if (!slots.length) slots.push(1, 2, 3, 4, 5);

  const list = h('div', { class: 'card section' });
  list.append(h('div', { class: 'chart-sub' },
    `${LOCATION_NAMES[loc]} · ${meal} · ${fmtDay(date)}. `,
    'Slot numbers are the painted positions on the station, not dishes.'));

  const inputs = new Map();
  for (const slot of slots) {
    const row = byNumber.get(slot);
    const deployed = deployedSlots.includes(slot);
    const input = h('input', {
      type: 'text', value: row?.food_name || '', placeholder: 'No dish',
      'aria-label': `Slot ${slot} dish`, autocomplete: 'off',
      maxlength: 60,
    });
    inputs.set(slot, input);

    list.append(h('div', { class: `row-form${row && !row.is_saved ? ' is-draft' : ''}` },
      h('span', { class: 'slotno' }, `${slot}${deployed ? '' : ' ·'}`),
      input,
      row?.is_saved
        ? h('button', {
            class: 'danger-text', type: 'button',
            title: 'Remove this dish from the menu',
            onclick: () => clearSlot(ctx, { loc, meal, date, slot, go }),
          }, 'Clear')
        : h('span', { class: 'dim', style: 'font-size:.75rem' }, deployed ? '' : 'no stack')));
  }

  const remaining = FOOD_SLOTS.filter(n => !slots.includes(n));
  if (remaining.length) {
    list.append(h('div', { class: 'row-form', style: 'border-bottom:0' },
      h('span', {}),
      h('select', {
        'aria-label': 'Add a slot',
        onchange: e => {
          const n = Number(e.target.value);
          if (!n) return;
          e.target.value = '';
          // Held outside the DOM so it survives the re-render below. A slot with
          // no stack behind it is legal — the position may exist before a
          // counter is installed on it.
          if (!manuallyAdded.has(extraKey)) manuallyAdded.set(extraKey, new Set());
          manuallyAdded.get(extraKey).add(n);
          go({});
          toast(`Slot ${n} added — enter a dish and save`);
        },
      }, h('option', { value: '' }, 'Add slot…'),
         ...remaining.map(n => h('option', { value: n }, `Slot ${n}`))),
      h('span', {})));
  }

  frag.append(list);

  const status = h('span', { class: 'dim', style: 'font-size:.8rem' });
  const saveBtn = h('button', { class: 'primary', type: 'button' }, 'Save menu');
  saveBtn.addEventListener('click', async () => {
    saveBtn.disabled = true;
    status.textContent = 'Saving…';
    try {
      await save(ctx, { loc, meal, date, inputs });
      toast('Menu saved');
      go({});
    } catch (err) {
      status.textContent = '';
      toast(describeError(err), true);
    } finally {
      saveBtn.disabled = false;
    }
  });

  frag.append(h('div', { class: 'sticky-actions' },
    saveBtn,
    h('button', { class: 'ghost', type: 'button', onclick: () => go({}) }, 'Reload'),
    status));

  frag.append(copyDayCard(ctx, { loc, date, tz }));
  return frag;
}

async function save(ctx, { loc, meal, date, inputs }) {
  const payload = [];
  const toDelete = [];
  for (const [slot, input] of inputs) {
    const name = input.value.trim();
    if (name) payload.push({ location: loc, meal_type: meal, meal_date: date, food_slot: slot, food_name: name });
    else toDelete.push(slot);
  }

  if (payload.length) {
    // Blanks are filtered above: a CHECK rejects an empty food_name, so
    // submitting untouched slots would fail the whole batch.
    unwrap(await ctx.client.from('meal_food_mapping')
      .upsert(payload, { onConflict: 'location,meal_date,meal_type,food_slot' }));
  }
  if (toDelete.length) {
    // Emptying a field means "no dish here", which is a DELETE — not a row
    // holding an empty name.
    unwrap(await ctx.client.from('meal_food_mapping').delete()
      .eq('location', loc).eq('meal_type', meal).eq('meal_date', date)
      .in('food_slot', toDelete));
  }
  if (!payload.length && !toDelete.length) throw new Error('Nothing to save');
}

async function clearSlot(ctx, { loc, meal, date, slot, go }) {
  if (!confirmAction(`Remove the dish in slot ${slot} from this menu?`)) return;
  try {
    unwrap(await ctx.client.from('meal_food_mapping').delete()
      .eq('location', loc).eq('meal_type', meal)
      .eq('meal_date', date).eq('food_slot', slot));
    toast(`Slot ${slot} cleared`);
    go({});
  } catch (err) {
    toast(describeError(err), true);
  }
}

/** Copy a whole day's three menus to another date — the tedium the preload
 *  does not cover, since preload only reaches back, never forward. */
function copyDayCard(ctx, { loc, date, tz }) {
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
        .select('meal_type, food_slot, food_name')
        .eq('location', loc).eq('meal_date', date)) || [];
      if (!rows.length) { note.textContent = 'Nothing saved on this day to copy.'; return; }
      unwrap(await ctx.client.from('meal_food_mapping').upsert(
        rows.map(r => ({ location: loc, meal_date: to, meal_type: r.meal_type,
                         food_slot: r.food_slot, food_name: r.food_name })),
        { onConflict: 'location,meal_date,meal_type,food_slot' }));
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
      `All three meals for ${LOCATION_NAMES[loc]} on ${fmtDay(date)}, written to another date. `,
      'Existing entries on the target date are overwritten.'),
    h('div', { class: 'toolbar' }, target, btn, note));
}
