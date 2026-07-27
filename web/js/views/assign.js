// ====================================================================
//  Devices — the permanent assignment.
//
//  Rare: location and food_slot change only when hardware is physically
//  moved or replaced. `label` names the POSITION, never the dish — what
//  sits in slot 3 changes with the meal and lives on the Menu tab.
//
//  (location, food_slot) is deliberately NOT unique. Darshanarthi runs
//  three counters per dish position, so a shared position is normal and is
//  shown as a count, never flagged as a conflict.
//
//  device_id is not editable: it is the installation's identity and the key
//  every history row hangs off.
// ====================================================================

import { h, badge, banner, toast } from '../ui.js';
import { unwrap, describeError } from '../supa.js';
import { LOCATION_NAMES, FOOD_SLOTS, fmtRelative } from '../domain.js';

const LOCATIONS = ['D', 'M', 'T', 'R'];

export function renderAssign(state, params, ctx) {
  const frag = document.createDocumentFragment();
  const devices = [...state.devices].sort((a, b) => a.device_id.localeCompare(b.device_id));

  frag.append(banner('info', 'ℹ',
    h('b', {}, 'Assignment is permanent; the menu is not. '),
    'Several stacks sharing one position is normal, not a conflict — remaining stock ',
    'for a dish is the sum across them. Labels name the physical position; dish names ',
    'belong on the Menu tab.'));

  const shared = new Map();
  for (const d of devices) {
    if (d.location == null || d.food_slot == null) continue;
    const k = `${d.location}|${d.food_slot}`;
    shared.set(k, (shared.get(k) || 0) + 1);
  }

  const dirty = new Map();   // device_id -> patch
  const status = h('span', { class: 'dim', style: 'font-size:.8rem' });
  const saveBtn = h('button', { class: 'primary', type: 'button', disabled: true }, 'Save changes');

  const markDirty = (id, field, value, original) => {
    const patch = dirty.get(id) || {};
    if (value === original) delete patch[field];
    else patch[field] = value;
    if (Object.keys(patch).length) dirty.set(id, patch); else dirty.delete(id);
    saveBtn.disabled = dirty.size === 0;
    saveBtn.textContent = dirty.size ? `Save ${dirty.size} change${dirty.size === 1 ? '' : 's'}` : 'Save changes';
    status.textContent = '';
  };

  const list = h('div', { class: 'card section' });
  list.append(h('div', { class: 'assign-row', style: 'font-size:.75rem;color:var(--ink-3)' },
    h('span', {}, 'Device'), h('span', {}, 'Area'), h('span', {}, 'Slot'),
    h('span', { class: 'lbl' }, 'Label'), h('span', { class: 'act' }, '')));

  for (const d of devices) {
    const row = h('div', { class: 'assign-row' });
    const flag = () => row.classList.toggle('dirty', dirty.has(d.device_id));

    const locSel = h('select', { 'aria-label': `${d.device_id} area` },
      h('option', { value: '', selected: d.location == null }, '—'),
      ...LOCATIONS.map(l => h('option', { value: l, selected: d.location === l },
        `${l} ${LOCATION_NAMES[l]}`)));
    locSel.addEventListener('change', () => {
      markDirty(d.device_id, 'location', locSel.value || null, d.location);
      flag();
    });

    const slotSel = h('select', { 'aria-label': `${d.device_id} slot` },
      h('option', { value: '', selected: d.food_slot == null }, '—'),
      ...FOOD_SLOTS.map(n => h('option', { value: n, selected: Number(d.food_slot) === n }, n)));
    slotSel.addEventListener('change', () => {
      markDirty(d.device_id, 'food_slot', slotSel.value ? Number(slotSel.value) : null,
        d.food_slot == null ? null : Number(d.food_slot));
      flag();
    });

    const labelInput = h('input', {
      type: 'text', value: d.label || '', placeholder: 'e.g. Darshanarthi slot 3',
      'aria-label': `${d.device_id} label`, maxlength: 80,
    });
    labelInput.addEventListener('input', () => {
      markDirty(d.device_id, 'label', labelInput.value.trim() || null, d.label || null);
      flag();
    });

    const count = shared.get(`${d.location}|${d.food_slot}`) || 0;

    row.append(
      h('div', { class: 'did' },
        d.device_id,
        h('div', { class: 'dev-where' },
          d.awaiting_deployment ? 'never reported' : `updated ${fmtRelative(d.updated_at)}`)),
      locSel,
      slotSel,
      h('div', { class: 'lbl' }, labelInput),
      h('div', { class: 'act' },
        count > 1 ? badge('idle', '⧉', `${count} stacks here`) : ''));

    list.append(row);
  }

  frag.append(list);

  saveBtn.addEventListener('click', async () => {
    saveBtn.disabled = true;
    status.textContent = 'Saving…';
    const failures = [];
    let ok = 0;
    for (const [deviceId, patch] of dirty) {
      try {
        unwrap(await ctx.client.from('devices').update(patch).eq('device_id', deviceId));
        ok++;
      } catch (err) {
        failures.push(`${deviceId}: ${describeError(err)}`);
      }
    }
    if (failures.length) {
      status.textContent = failures.join(' · ');
      toast(`${ok} saved, ${failures.length} failed`, true);
    } else {
      toast(`${ok} device${ok === 1 ? '' : 's'} updated`);
    }
    dirty.clear();
    await ctx.refresh();
  });

  frag.append(h('div', { class: 'sticky-actions' }, saveBtn, status));
  return frag;
}
