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
import {
  LOCATION_NAMES, FOOD_SLOTS, fmtRelative, assignmentError, isHalfAssigned,
} from '../domain.js';

const LOCATIONS = ['D', 'M', 'T', 'R'];

export function renderAssign(state, params, ctx) {
  const frag = document.createDocumentFragment();
  const devices = [...state.devices].sort((a, b) => a.device_id.localeCompare(b.device_id));

  frag.append(banner('info', 'ℹ',
    h('b', {}, 'Assignment is permanent; the menu is not. '),
    'Several stacks sharing one position is normal, not a conflict — remaining stock ',
    'for a dish is the sum across them. Labels name the physical position; dish names ',
    'belong on the Menu tab.'));

  // Existing damage, named. Until these are given a slot they contribute
  // nothing to any dish position and the stock screen quietly under-reports.
  const broken = devices.filter(isHalfAssigned);
  if (broken.length) {
    frag.append(banner('critical', '⚠',
      h('b', {}, `${broken.length} device${broken.length === 1 ? ' is' : 's are'} in an area with no slot. `),
      `Stock totals are wrong until this is fixed: `,
      h('b', {}, broken.map(d => d.device_id).join(', ')),
      ` contribute${broken.length === 1 ? 's' : ''} nothing to `,
      broken.length === 1 ? 'its dish position' : 'their dish positions',
      ', because the stock view can only sum devices that have both an area and a slot.'));
  }

  const shared = new Map();
  for (const d of devices) {
    if (d.location == null || d.food_slot == null) continue;
    const k = `${d.location}|${d.food_slot}`;
    shared.set(k, (shared.get(k) || 0) + 1);
  }

  const dirty = new Map();    // device_id -> patch
  const invalid = new Map();  // device_id -> why the pair is not saveable
  const status = h('span', { class: 'dim', style: 'font-size:.8rem' });
  const saveBtn = h('button', { class: 'primary', type: 'button', disabled: true }, 'Save changes');

  // Only a row you have TOUCHED can block the save. Rows already broken in the
  // database are shown in red, but they must not hold the page hostage —
  // otherwise one bad row from before this validation existed would stop you
  // relabelling an unrelated device.
  const refreshSaveBtn = () => {
    const blocking = [...dirty.keys()].filter(id => invalid.has(id));
    saveBtn.disabled = dirty.size === 0 || blocking.length > 0;
    saveBtn.textContent = blocking.length
      ? `Fix ${blocking.length} row${blocking.length === 1 ? '' : 's'} first`
      : dirty.size
        ? `Save ${dirty.size} change${dirty.size === 1 ? '' : 's'}`
        : 'Save changes';
  };

  const markDirty = (id, field, value, original) => {
    const patch = dirty.get(id) || {};
    if (value === original) delete patch[field];
    else patch[field] = value;
    if (Object.keys(patch).length) dirty.set(id, patch); else dirty.delete(id);
    refreshSaveBtn();
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

    const slotSel = h('select', { 'aria-label': `${d.device_id} slot` },
      h('option', { value: '', selected: d.food_slot == null }, '—'),
      ...FOOD_SLOTS.map(n => h('option', { value: n, selected: Number(d.food_slot) === n }, n)));

    const rowError = h('div', {
      class: 'row-error', role: 'alert', hidden: true,
    });

    const origSlot = d.food_slot == null ? null : Number(d.food_slot);

    /**
     * The two dropdowns are one decision, not two. Left independent, they let
     * you save an area with no slot — which slot_overview drops, so the device
     * silently stops counting toward its dish position. Validate the PAIR on
     * every change and refuse to save until it is coherent.
     */
    const syncPair = () => {
      const loc = locSel.value || null;
      let slot = slotSel.value ? Number(slotSel.value) : null;

      // A reserved or unassigned unit occupies no serving position, so the
      // slot is cleared for you rather than reported as an error.
      if ((loc === 'R' || loc == null) && slot != null) {
        slotSel.value = '';
        slot = null;
      }
      slotSel.disabled = loc === 'R' || loc == null;

      markDirty(d.device_id, 'location', loc, d.location);
      markDirty(d.device_id, 'food_slot', slot, origSlot);

      const err = assignmentError(loc, slot);
      if (err) invalid.set(d.device_id, err); else invalid.delete(d.device_id);
      rowError.textContent = err || '';
      rowError.hidden = !err;
      row.classList.toggle('invalid', !!err);
      refreshSaveBtn();
      flag();
    };

    locSel.addEventListener('change', syncPair);
    slotSel.addEventListener('change', syncPair);

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
        isHalfAssigned(d) ? badge('critical', '⚠', 'No slot')
          : count > 1 ? badge('idle', '⧉', `${count} stacks here`) : ''),
      rowError);

    // Surface rows that are already broken in the database, so they are red on
    // arrival rather than only after someone touches them.
    slotSel.disabled = d.location === 'R' || d.location == null;
    const initialErr = assignmentError(d.location ?? null, origSlot);
    if (initialErr) {
      invalid.set(d.device_id, initialErr);
      rowError.textContent = initialErr;
      rowError.hidden = false;
      row.classList.add('invalid');
    }

    list.append(row);
  }
  refreshSaveBtn();

  frag.append(list);

  saveBtn.addEventListener('click', async () => {
    saveBtn.disabled = true;
    status.textContent = 'Saving…';
    const failures = [];
    let ok = 0;
    for (const [deviceId, patch] of dirty) {
      // Belt and braces. The form cannot reach this state, but a half-assigned
      // write is silent and permanent-looking -- the device just stops
      // appearing in its slot total -- so it is checked once more against the
      // values actually about to be sent.
      const before = devices.find(x => x.device_id === deviceId) || {};
      const loc = 'location' in patch ? patch.location : (before.location ?? null);
      const slot = 'food_slot' in patch
        ? patch.food_slot
        : (before.food_slot == null ? null : Number(before.food_slot));
      const err = assignmentError(loc, slot);
      if (err) { failures.push(`${deviceId}: ${err}`); continue; }

      try {
        unwrap(await ctx.client.from('devices').update(patch).eq('device_id', deviceId));
        ok++;
      } catch (err2) {
        failures.push(`${deviceId}: ${describeError(err2)}`);
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
