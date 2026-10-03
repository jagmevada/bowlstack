// ====================================================================
//  Render smoke test.
//
//    cd web/test && npm install && node smoke.mjs
//
//  Loads the real index.html into jsdom, stubs PostgREST with fixtures
//  shaped exactly like device_overview / slot_overview / status_events,
//  and drives every screen. It exists to protect the semantic rules — the
//  ones that are easy to break by a reasonable-looking edit and that no
//  reviewer would catch by reading a diff:
//
//    - discontiguous never renders as a count
//    - bowls_trusted NULL is "no data", not zero
//    - capacity comes from the view, never a hardcoded 4 or 12
//    - offline sorts above everything else
//    - blank dish names are never submitted; clearing is a DELETE
//    - device_id is never written
//    - a background poll never wipes a half-typed form
// ====================================================================

import { JSDOM } from 'jsdom';
import fs from 'node:fs';

const here = new URL('.', import.meta.url);
const webDir = new URL('..', here);
const html = fs.readFileSync(new URL('index.html', webDir), 'utf8');

const dom = new JSDOM(html, { url: 'https://x.test/', pretendToBeVisual: true });
const { window } = dom;

for (const k of ['document', 'navigator', 'HTMLElement', 'Node', 'Event', 'CustomEvent',
                 'URLSearchParams', 'getComputedStyle', 'requestAnimationFrame', 'DocumentFragment']) {
  Object.defineProperty(globalThis, k, { value: window[k], configurable: true, writable: true });
}
globalThis.window = window;
globalThis.location = window.location;
globalThis.localStorage = window.localStorage;
globalThis.confirm = () => true;
window.confirm = () => true;
window.alert = () => {};
window.scrollTo = () => {};

// ---- fixtures: the real assignment from supabase/assign_devices.sql ----
const ASSIGN = [
  ...['001', '002', '003'].map(n => ['D', 1, n]), ...['004', '005', '006'].map(n => ['D', 2, n]),
  ...['007', '008', '009'].map(n => ['D', 3, n]), ...['010', '011', '012'].map(n => ['D', 4, n]),
  ...['021', '022'].map(n => ['D', 5, n]),
  ['M', 1, '013'], ['M', 2, '014'], ['M', 3, '015'], ['M', 4, '016'], ['M', 5, '023'],
  ['T', 1, '017'], ['T', 2, '018'], ['T', 3, '019'], ['T', 4, '020'], ['T', 5, '024'],
];
const AREA = { D: 'Darshanarthi', M: 'Mahatma', T: 'Tiffin' };
const MENU = {
  D: { 1: 'Rice', 2: 'Dal', 3: 'Curry', 4: 'Roti', 5: 'Khichdi' },
  M: { 1: 'Rice', 2: 'Dal', 3: 'Sabzi', 4: 'Roti', 5: 'Salad' },
  T: { 1: 'Rice', 2: 'Dal', 3: 'Bhaji', 4: 'Roti', 5: 'Chaas' },
};

// Per-bowl mass in GRAMS, keyed the same way. Slot 1 and 2 agree across all
// three areas — the normal case. Slot 3 deliberately does NOT: the areas
// serve different dishes at different weights, and TIFFIN IS UNWEIGHED. That
// one gap is the fixture's most important property: it is what proves the
// Master row degrades to a lower bound instead of quietly pricing Tiffin's
// Bhaji at zero, and what proves the naive "total bowls x one weight"
// formula is not what shipped.
const WEIGHT = {
  D: { 1: 5000, 2: 3500, 3: 4500, 4: 2000, 5: 4000 },
  M: { 1: 5000, 2: 3500, 3: 4000, 4: 2000, 5: 4000 },
  T: { 1: 5000, 2: 3500, 3: null, 4: 2000, 5: 4000 },
};
const now = Date.now();
const iso = ms => new Date(ms).toISOString();

const devices = [];
ASSIGN.forEach(([loc, slot, n], i) => {
  const id = `BWL-${n}`;
  // One device seeded into each awkward state the UI has a rule for.
  const fault = id === 'BWL-002';
  const degr = id === 'BWL-008';
  const degrFull = id === 'BWL-022';   // degraded but the stack is FULL: >=4 of 4 is exactly 4
  const noSense = id === 'BWL-023';    // zero sensors online; the count in the payload is a leftover
  const off = id === 'BWL-014';
  const missed = id === 'BWL-021';   // slept through the last completed window
  const noBatt = id === 'BWL-019';
  const critB = id === 'BWL-005';
  const lowB = id === 'BWL-011';
  const medB = id === 'BWL-012';
  const never = id === 'BWL-024';
  const levels = fault ? ['absent', 'present', 'absent', 'absent']
    : degr ? ['present', 'present', 'unknown', 'absent']
    : degrFull ? ['present', 'present', 'present', 'present']
    : noSense ? ['unknown', 'unknown', 'unknown', 'unknown']
    : [['absent', 'absent', 'absent', 'absent'],
       ['present', 'absent', 'absent', 'absent'],
       ['present', 'present', 'absent', 'absent'],
       ['present', 'present', 'present', 'absent'],
       ['present', 'present', 'present', 'present']][i % 5];
  devices.push({
    device_id: id, location: loc, food_slot: slot,
    label: `${AREA[loc]} slot ${slot}`, timezone: 'Asia/Kolkata',
    current_food: MENU[loc][slot], current_meal: 'Lunch',
    reported: !never,
    updated_at: never ? null : iso(now - (off ? 20 * 60_000 : missed ? 30 * 3600_000 : 25_000)),
    stale_for: '00:00:25', in_service: true,
    offline: off, awaiting_deployment: never, data_is_stale: false,
    missed_last_service: missed,
    stack_count: never ? null : noSense ? 4 : levels.filter(l => l === 'present').length,
    stack_status: never ? null : fault ? 'discontiguous'
      : (degr || degrFull || noSense) ? 'degraded' : 'ok',
    levels: never ? null : levels,
    sensors_online: never ? null : noSense ? 0 : (degr || degrFull) ? 3 : 4,
    battery_mv: never ? null : noBatt ? null : critB ? 3320 : lowB ? 3560 : medB ? 3720 : 4020,
    battery_level: never ? null : noBatt ? null : critB ? 'critical' : lowB ? 'low' : medB ? 'medium' : 'good',
    charging: never ? null : id === 'BWL-001',
    uptime_s: never ? null : 7412, firmware: never ? null : '0.2.0',
    mac: never ? null : 'A0:B1:C2:D3:E4:F5',
  });
});
for (let n = 25; n <= 32; n++) {
  devices.push({
    // BWL-030 reproduces the state that emptied Darshanarthi slot 1: parked in
    // a serving area with no slot, so slot_overview drops it entirely.
    device_id: `BWL-0${n}`,
    location: n === 30 ? 'D' : 'R', food_slot: null,
    label: n === 30 ? 'Darshanarthi slot 1' : 'Reserved',
    timezone: 'Asia/Kolkata', current_food: null, current_meal: 'Lunch',
    reported: false, updated_at: null, stale_for: null, in_service: true,
    offline: false, awaiting_deployment: true, data_is_stale: false,
    stack_count: null, stack_status: null, levels: null, sensors_online: null,
    battery_mv: null, battery_level: null, charging: null, uptime_s: null,
    firmware: null, mac: null, missed_last_service: false,
  });
}

// slot_overview, aggregated the way the view does it.
const slots = [];
for (const loc of ['D', 'M', 'T']) {
  for (let s = 1; s <= 5; s++) {
    const mine = devices.filter(d => d.location === loc && d.food_slot === s);
    const ok = mine.filter(d => d.stack_status === 'ok');
    const rep = mine.filter(d => d.reported);
    slots.push({
      location: loc, food_slot: s,
      current_food: MENU[loc][s], current_meal: 'Lunch',
      devices: mine.length, devices_reported: rep.length,
      bowls_capacity: mine.length * 4,
      // sum() FILTER returns NULL, not 0, when nothing matches.
      bowls_trusted: ok.length ? ok.reduce((n, d) => n + d.stack_count, 0) : null,
      bowls_reported: rep.length ? rep.reduce((n, d) => n + (d.stack_count ?? 0), 0) : null,
      any_fault: mine.some(d => d.stack_status === 'discontiguous'),
      any_degraded: mine.some(d => d.stack_status === 'degraded'),
      any_battery_warn: mine.some(d => ['low', 'critical'].includes(d.battery_level)),
      any_offline: mine.some(d => d.offline),
      oldest_update: rep.map(d => d.updated_at).sort()[0] ?? null,
      any_missed_service: mine.some(d => d.missed_last_service),
    });
  }
}
slots.push({
  location: 'M', food_slot: 6, current_food: null, current_meal: 'Lunch',
  devices: 1, devices_reported: 0, bowls_capacity: 4, bowls_trusted: null,
  bowls_reported: null, any_fault: false, any_degraded: false,
  any_battery_warn: false, any_offline: false, oldest_update: null,
  any_missed_service: false,
});

// slot_quantity, aggregated the way THAT view does it: grouped by food_slot
// across every area, summing each area's bowls against its own weight, and
// excluding any slot whose devices have never reported.
const quantity = [];
for (let sl = 1; sl <= 6; sl++) {
  const areas = ['D', 'M', 'T'].map(loc => {
    const row = slots.find(x => x.location === loc && x.food_slot === sl);
    return {
      location: loc, food_name: MENU[loc][sl], bowl_weight_g: WEIGHT[loc][sl],
      bowls_trusted: row ? row.bowls_trusted : null,
      bowls_capacity: row ? row.bowls_capacity : 0,
      devices: row ? row.devices : 0,
      // Each hall's own mass, computed by the view so no screen re-derives it.
      weight_g: WEIGHT[loc][sl] == null || !row || row.bowls_trusted == null
        ? null : row.bowls_trusted * WEIGHT[loc][sl],
      capacity_weight_g: WEIGHT[loc][sl] == null || !row ? null
        : row.bowls_capacity * WEIGHT[loc][sl],
    };
  });
  const mine = devices.filter(d => d.food_slot === sl && ['D', 'M', 'T'].includes(d.location));
  if (!mine.some(d => d.reported)) continue;           // the has-ever-reported gate
  const weighed = areas.filter(a => a.bowl_weight_g != null);
  const distinctW = [...new Set(weighed.map(a => a.bowl_weight_g))];
  const trusted = areas.filter(a => a.bowls_trusted != null);
  quantity.push({
    food_slot: sl, current_meal: 'Lunch',
    // The menu resolved from the meal that is running, so it is live. The
    // not-live case is exercised below.
    menu_meal_type: 'Lunch', menu_meal_date: '2026-08-20', menu_is_live: true,
    dishes: [...new Set(areas.map(a => a.food_name).filter(Boolean))].sort(),
    bowl_weight_g: distinctW.length === 1 ? distinctW[0] : null,
    devices: areas.reduce((n, a) => n + a.devices, 0),
    bowls_capacity: areas.reduce((n, a) => n + a.bowls_capacity, 0),
    bowls_trusted: trusted.length ? trusted.reduce((n, a) => n + a.bowls_trusted, 0) : null,
    est_weight_g: weighed.length
      ? weighed.reduce((n, a) => n + (a.bowls_trusted || 0) * a.bowl_weight_g, 0) : null,
    capacity_weight_g: weighed.length
      ? weighed.reduce((n, a) => n + a.bowls_capacity * a.bowl_weight_g, 0) : null,
    est_is_partial: areas.some(a => a.bowl_weight_g == null && (a.bowls_trusted || 0) > 0),
    areas_without_weight: areas
      .filter(a => a.bowl_weight_g == null && (a.bowls_trusted || 0) > 0).map(a => a.location),
    areas,
    any_fault: mine.some(d => d.stack_status === 'discontiguous'),
    any_degraded: mine.some(d => d.stack_status === 'degraded'),
    any_battery_warn: mine.some(d => ['low', 'critical'].includes(d.battery_level)),
    any_offline: mine.some(d => d.offline),
    any_missed_service: mine.some(d => d.missed_last_service),
    oldest_update: mine.filter(d => d.reported).map(d => d.updated_at).sort()[0] ?? null,
  });
}

// status_events, with a seq gap, a second boot_id and one backdated row.
const events = [];
let seq = 0;
for (let i = 30; i >= 0; i--) {
  const t = now - i * 8 * 60_000;
  events.push({
    recorded_at: iso(t),
    received_at: iso(t + (i === 12 ? 315_000 : 300)),
    reason: i === 30 ? 'boot' : i % 9 === 0 ? 'periodic' : 'change',
    seq: (seq += (i === 7 ? 3 : 1)), boot_id: i > 20 ? 41 : 42,
    stack_count: i % 5,
    stack_status: i === 4 ? 'discontiguous' : i === 9 ? 'degraded' : 'ok',
    levels: ['present', 'present', 'absent', 'absent'],
    sensors_online: 4, battery_level: i > 15 ? 'good' : 'medium',
    charging: false, firmware: '0.2.0',
  });
}
events.reverse();   // the query orders recorded_at descending

// weight_samples for the buffer block near the end, newest first. The oldest
// row is the boot after a power cycle, so its bowl count is unconfirmed.
const samples = [0, 1, 2].map(k => ({
  device_id: 'BWL-101', recorded_at: iso(now - k * 120_000),
  received_at: iso(now - k * 120_000 + 300),
  reason: k === 2 ? 'boot' : 'periodic', seq: 3 - k, boot_id: 7,
  weight_state: 'ok', weight_g: 42300 + k * 500, gross_g: 49800 + k * 500,
  bowls: 3, bowls_confirmed: k !== 2, cells_online: 1,
  net_counts: Math.round((49800 + k * 500) * 20.7), counts_per_gram: 20.7,
  battery_level: 'good', firmware: 'V1.20 261003',
}));

const preload = [1, 2, 3, 4, 5].map(s => ({
  food_slot: s, food_name: MENU.D[s], bowl_weight_g: WEIGHT.D[s],
  source_date: '2026-07-26', is_saved: false,
}));

// Weekly template fixture: D has a full Lunch row set for every weekday,
// and M slot 6 has a plan — the position whose dated menu is deliberately
// missing, so the "menu not applied" hint has something to point at.
const templateRows = [];
for (let wd = 0; wd <= 6; wd++) {
  for (let slot = 1; slot <= 5; slot++) {
    templateRows.push({ location: 'D', weekday: wd, meal_type: 'Lunch',
                        food_slot: slot, food_name: `T-${MENU.D[slot]}`,
                        bowl_weight_g: WEIGHT.D[slot] });
  }
  templateRows.push({ location: 'M', weekday: wd, meal_type: 'Lunch',
                      food_slot: 6, food_name: 'T-PlannedDish' });
}

// ---- fake PostgREST ---------------------------------------------------
const calls = [];
let preloadSourceDate = null;   // set per-test to mark a template draft
// Flip to simulate a database that has not run migrate_bowl_weight.sql: the
// view does not exist, so PostgREST answers 42P01 rather than an empty set.
let quantityMissing = false;
let authCallback = null;        // captured so tests can fire auth events
function builder(table) {
  const rec = { table, filters: [] };
  calls.push(rec);
  const api = {};
  for (const m of ['select', 'order', 'eq', 'gte', 'in', 'limit', 'upsert', 'update', 'delete']) {
    api[m] = (...args) => { rec.filters.push([m, ...args]); return api; };
  }
  api.then = (res, rej) => {
    if (table === 'slot_quantity' && quantityMissing) {
      return Promise.resolve({ data: null, error: {
        message: 'relation "public.slot_quantity" does not exist', code: '42P01',
      } }).then(res, rej);
    }
    let data = table === 'device_overview' ? devices
      : table === 'slot_overview' ? slots
      : table === 'slot_quantity' ? quantity
      : table === 'status_events' ? events
      : table === 'weight_samples' ? samples
      : table === 'meal_food_mapping' ? [{ meal_type: 'Lunch', food_slot: 1, food_name: 'Rice' }]
      : table === 'meal_menu_template' ? templateRows
      : [];
    // Honour eq filters on the template table — the week editor queries per
    // location, and handing it every location's rows corrupted its counts.
    if (table === 'meal_menu_template') {
      for (const f of rec.filters) {
        if (f[0] === 'eq') data = data.filter(r => String(r[f[1]]) === String(f[2]));
      }
    }
    return Promise.resolve({ data, error: null }).then(res, rej);
  };
  return api;
}

// Start with NO session, so boot has to authenticate itself the way a kitchen
// tablet does: silently, with nobody typing anything.
let session = null;
let anonSignIns = 0;

window.supabase = {
  createClient: () => ({
    from: builder,
    rpc: (name, args) => {
      calls.push({ rpc: name, args });
      if (name === 'meal_template_apply') {
        return Promise.resolve({ data: [
          { meal_date: args.p_from, meal_type: 'Lunch', written: 5, skipped: false },
          { meal_date: args.p_from, meal_type: 'Dinner', written: 0, skipped: true },
        ], error: null });
      }
      // Tiffin disagrees in slot 2, so the All-areas view has one genuinely
      // ambiguous slot to protect.
      if (name === 'meal_mapping_preload' && args.p_location === 'T') {
        const rows = (preloadSourceDate
          ? preload.map(r => ({ ...r, source_date: preloadSourceDate }))
          : preload).map(r => Number(r.food_slot) === 2 ? { ...r, food_name: 'T-Special' } : r);
        return Promise.resolve({ data: rows, error: null });
      }
      return Promise.resolve({
        data: preloadSourceDate
          ? preload.map(r => ({ ...r, source_date: preloadSourceDate }))
          : preload,
        error: null,
      });
    },
    auth: {
      getSession: async () => ({ data: { session }, error: null }),
      getUser: async () => ({ data: { user: session?.user ?? null }, error: null }),
      onAuthStateChange: (cb) => {
        authCallback = cb;
        return { data: { subscription: { unsubscribe() {} } } };
      },
      signOut: async () => { session = null; return { error: null }; },
      signInWithPassword: async () => {
        session = { user: { email: 'kitchen@example.test' } };
        return { error: null };
      },
      // First attempt fails the way a fresh Supabase project does — the
      // feature is off by default. That exercises the remedy card AND the
      // retry, and everything downstream still runs.
      signInAnonymously: async () => {
        if (++anonSignIns === 1) {
          return { error: { message: 'Anonymous sign-ins are disabled' } };
        }
        session = { user: { id: 'anon-1', is_anonymous: true } };
        return { error: null };
      },
    },
  }),
};

window.BOWLSTACK_CONFIG = {
  url: 'https://x.supabase.co', anonKey: 'anon', autoLogin: 'anonymous',
};

// ---- run --------------------------------------------------------------
const fails = [];
const ok = (name, cond, extra = '') => {
  if (cond) console.log(`  PASS  ${name}`);
  else { console.log(`  FAIL  ${name} ${extra}`); fails.push(name); }
};
const go = async hash => {
  window.location.hash = hash;
  window.dispatchEvent(new window.Event('hashchange'));
  await new Promise(r => setTimeout(r, 120));
};

await import(new URL('js/app.js', webDir).href);
await new Promise(r => setTimeout(r, 150));

const view = window.document.getElementById('view');
const text = () => view.textContent.replace(/\s+/g, ' ');

const hidden = id => window.document.getElementById(id).hasAttribute('hidden');

console.log('\n[auto-login: anonymous sign-ins still off]');
ok('attempted sign-in on boot', anonSignIns === 1);
ok('shows the remedy, not a raw error', !hidden('connecting'));
{
  const box = window.document.getElementById('connecting-error');
  ok('names the setting to turn on', /Allow anonymous sign-ins/.test(box.textContent));
  ok('links to the right dashboard page',
    box.querySelector('a')?.getAttribute('href')
      === 'https://supabase.com/dashboard/project/x/auth/providers');
  ok('offers the account route as a way out',
    !window.document.getElementById('connecting-manual').hidden);
}

// Flip the switch and press Try again.
window.document.getElementById('connecting-retry').dispatchEvent(new window.Event('click'));
await new Promise(r => setTimeout(r, 150));

console.log('\n[auto-login: after retry]');
ok('signed in without a prompt', anonSignIns === 2);
ok('login form never shown', hidden('login'));
ok('setup form never shown', hidden('setup'));
ok('connecting gate cleared', hidden('connecting'));
ok('sign-out hidden when auto-login is on',
  window.document.querySelector('[data-act="signout"]').hidden === true);
ok('anonymous user labelled',
  /anonymously/i.test(window.document.getElementById('menu-user').textContent));

console.log('\n[hand-configured hosts: no login form ever]');
{
  // A Vercel-style host has no generated config.js; the first-run screen
  // stores the connection per-browser. That stored connection must imply
  // anonymous auto-login — the field hit a staff-account form for which no
  // account has ever existed.
  const { saveConfig, readAutoLogin, clearConfig } = await import('../js/supa.js');
  const savedBaked = window.BOWLSTACK_CONFIG;
  window.BOWLSTACK_CONFIG = undefined;          // no deployment config at all
  saveConfig('https://x.supabase.co/', 'somekey');
  ok('a pasted connection auto-logs-in anonymously',
    readAutoLogin()?.mode === 'anonymous');
  clearConfig();
  window.BOWLSTACK_CONFIG = savedBaked;
  ok('deployment config still honoured afterwards',
    readAutoLogin()?.mode === 'anonymous');
}

console.log('\n[shell]');
ok('app pane visible', !hidden('app'));
ok('queried device_overview', calls.some(c => c.table === 'device_overview'));
ok('queried slot_overview', calls.some(c => c.table === 'slot_overview'));
ok('fleet chips rendered', window.document.getElementById('fleet-chips').children.length >= 5);
// 2 = BWL-014 (offline mid-window) + BWL-021 (missed the last window): the
// chip counts everything not talking when it should be.
ok('offline chip counts both silent devices',
  /2offline/.test(window.document.getElementById('fleet-chips').textContent));
ok('meal indicator live', /Lunch/.test(window.document.getElementById('meal-indicator').textContent));
{
  const v = window.document.getElementById('app-version');
  ok('version is shown', /^v\d+\.\d+/.test(v.textContent), v.textContent);
  ok('version sits just before the theme button',
    v.nextElementSibling?.id === 'theme-btn', v.nextElementSibling?.id);
}

console.log('\n[stock]');
ok('renders three areas', ['Darshanarthi', 'Mahatma', 'Tiffin'].every(a => text().includes(a)));
ok('each area sits on its own wash',
  ['D', 'M', 'T'].every(l => view.querySelector(`.area.area-${l}`) !== null));
ok('dish names resolved', text().includes('Khichdi') && text().includes('Chaas'));
ok('fault slot shows no count', text().includes('Check station'));
ok('no-data slot says No data', text().includes('No data'));
ok('degraded slot headline is a lower bound', text().includes('\u2265'));
ok('offline stack surfaced as a \u2715 line', view.querySelector('.dev-line .st-off') !== null);
ok('stack pills link to devices', view.querySelector('a[href^="#/device/"]') !== null);
ok('capacity comes from the view', text().includes('of 4 bowls') && text().includes('of 12 bowls'));

console.log('\n[offline: last value kept, in red]');
{
  // D5 holds BWL-021 (missed the last completed window) and BWL-022 (fine).
  const cards = [...view.querySelectorAll('.slot')];
  const d5 = cards.find(c => c.textContent.includes('Khichdi'));
  ok('missed-service slot keeps its number', d5?.querySelector('.slot-count') !== null);
  ok('and the number is red (compromised)', d5?.querySelector('.slot-count.is-alert') !== null);
  ok('with no decorative underline',
    !/underline/.test(getComputedStyle ? '' : '')  // css-level; the class carries colour only
    && d5?.querySelector('.slot-count.is-offline') === null);
  // Minimal by request: the card never prints a state word. The silent
  // stack is a \u2715 line, the degraded one a \u25d0 line, and the words live in
  // tooltips, the page legend and the Health page.
  ok('no state words printed on the card',
    !/last known|offline|Stack/i.test(d5?.textContent || ''));
  ok('one symbolic line per stack (\u2715 + \u25d0)',
    (d5?.querySelectorAll('.dev-line') || []).length === 2
    && d5?.querySelector('.dev-line .st-off') !== null
    && d5?.querySelector('.dev-line .st-deg') !== null);
  // Pinned to an actual numeral: "non-empty" would pass on any junk.
  const offLine = [...(d5?.querySelectorAll('.dev-line') || [])]
    .find(l => l.querySelector('.st-off'));
  ok('the \u2715 line pins its last-known numeral',
    /\d/.test(offLine?.querySelector('.ct')?.textContent || ''));
  ok('the red number explains itself on hover',
    /last known/.test(d5?.querySelector('.slot-count')?.title || ''));

  // The capsule bar now carries data confidence: D5 has BWL-021 (missed) and
  // BWL-022 (degraded) -- NO healthy stack -- so the whole bar is striped and
  // none of it is solid.
  {
    const fill = d5?.querySelector('.meter .fill');
    const bad = d5?.querySelector('.meter .invalid');
    ok('confidence bar present', !!fill && !!bad);
    ok('no solid fill when no stack is healthy', fill?.style.width === '0%');
    ok('the whole position is striped invalid', bad?.style.width === '100%');
  }
  // T1 (Rice, Tiffin slot 1: BWL-017 healthy, 1 bowl of 4): solid fill only.
  {
    const t1 = cards.filter(c => c.textContent.includes('Rice'))
      .find(c => c.querySelector('.meter .invalid')?.style.width === '0%'
              && c.querySelector('.meter .fill')?.style.width !== '0%');
    ok('a healthy position shows solid fill and zero stripes', !!t1);
  }
  // A healthy low count is INK, not red: quantity never colours the number.
  {
    const healthyLow = cards.find(c => !c.querySelector('.slot-count.is-alert')
      && c.querySelector('.slot-count')?.textContent === '1');
    ok('a nearly-empty healthy slot keeps a plain ink number', !!healthyLow);
  }
  // M2 is BWL-014: offline at 0 bowls -- red for the OFFLINE, not the zero.
  //
  // Found by the thing being asserted, not by the absence of "BWL" in the
  // card text. That old finder worked only because the device rows stripped
  // their prefix and printed "014"; the moment full ids came back -- which
  // they had to, once a scale could share the position -- every card contained
  // "BWL" and this silently matched nothing.
  const m2 = cards.find(c => c.querySelector('.slot-count.is-alert')?.textContent === '0');
  ok('an offline zero is red for the silence, not the quantity', m2 != null);

  // The fault and no-data treatments are not regressed: neither renders a
  // numeral, so neither can have gained a red one.
  const faultCard = cards.find(c => /Check station/.test(c.textContent));
  ok('fault slot still shows no count at all', faultCard?.querySelector('.slot-count') == null);
  const noData = cards.find(c => /No data/.test(c.textContent));
  ok('no-data slot stays grey', noData?.querySelector('.slot-count') == null
    && noData?.querySelector('.slot-nodata') !== null);

  // The thin problem strip: ONE deduplicated count — the header capsules
  // already itemize offline/fault/degraded/battery, and showing the same
  // numbers twice was the bug. Union of problem devices in the fixtures:
  // 002 fault, 008/022/023 degraded, 014 offline, 021 missed, 005 critical
  // cell, 011 low cell = 8 stations.
  const strip = view.querySelector('.alert-strip');
  ok('the problem strip is present', strip != null);
  ok('it carries one deduplicated count', /8 stations need attention/.test(strip?.textContent || ''));
  ok('it does not re-itemize the capsules',
    !/offline|fault|degraded|battery/.test(strip?.textContent || ''));
  ok('the message is wrap-proof', strip?.querySelector('.msg') !== null);
  ok('it points at Health', /Diagnose in Health/.test(strip?.textContent || ''));
  strip.dispatchEvent(new window.Event('click'));
  ok('clicking it opens Health filtered to problems',
    location.hash === '#/health?f=problems', location.hash);
  await go('#/stock');
}

console.log('\n[stock: symbolic cards & battery bars]');
{
  const line = id => [...view.querySelectorAll('.dev-line')]
    .find(l => l.getAttribute('href')?.includes(id));
  ok('cards carry no badge chips at all',
    view.querySelectorAll('.slot .badge').length === 0);
  ok('the legend pairs every glyph with its word once',
    ['reporting', 'offline', 'fault', 'degraded', 'no reading']
      .every(w => view.querySelector('.legend-line')?.textContent.includes(w)));
  ok('every device line wears a battery glyph',
    [...view.querySelectorAll('.dev-line')].length > 0
    && [...view.querySelectorAll('.dev-line')].every(l => l.querySelector('.batt')));
  ok('a healthy cell shows 4 bars', line('BWL-001')?.querySelector('.batt.lvl-good') !== null);
  ok('the charger overlays a bolt', line('BWL-001')?.querySelector('.batt .bolt') !== null);
  ok('a critical cell shows red', line('BWL-005')?.querySelector('.batt.lvl-critical') !== null);
  ok('no cell detected = dashed empty case',
    line('BWL-019')?.querySelector('.batt.lvl-none') !== null);
  ok('an unplugged good cell has no bolt',
    line('BWL-019')?.querySelector('.batt .bolt') === null);
  ok('the fault stack is a \u25b2 line', view.querySelector('.dev-line .st-fault') !== null);
  ok('a healthy stack is a \u25cf line', view.querySelector('.dev-line .st-ok') !== null);
  ok('a low cell shows amber', line('BWL-011')?.querySelector('.batt.lvl-low') !== null);
  ok('a medium cell shows 3 bars', line('BWL-012')?.querySelector('.batt.lvl-medium') !== null);
  ok('the legend carries the glyphs themselves, in order',
    [...view.querySelectorAll('.legend-line b')].map(b => b.textContent).join('')
      === '\u25cf\u2715\u25b2\u25d0\u25cc');
  const faultCard = [...view.querySelectorAll('.slot')]
    .find(c => c.querySelector('.st-fault'));
  ok('the fault card keeps its one sentence',
    /still reading correctly\./.test(faultCard?.textContent || ''));
}

// ====================================================================
//  Master — the site total in kilograms.
//
//  The expected figures below are worked out by hand, not recomputed from
//  the fixtures. A test that derived its expectation the same way the code
//  does would pass on a shared mistake, which is the one thing this block
//  exists to catch.
// ====================================================================
// ====================================================================
//  Menu — the per-bowl weight field.
//
//  Grams on the wire, kilograms on screen, and a blank field is NULL rather
//  than zero. That last rule is the one worth a test: zero would render a
//  full counter as "0.0 kg remaining" on Master and send someone to refill
//  something that is full.
// ====================================================================
console.log('');
console.log('[menu: per-bowl weight]');
await go('#/menu?locs=D&meal=Lunch&date=2026-09-01');
{
  const weights = [...view.querySelectorAll('.row-form .w-input')];
  ok('every slot gets a weight field', weights.length >= 5, `got ${weights.length}`);
  ok('the two fields are labelled', /Dish/.test(text()) && /kg \/ bowl/.test(text()));

  // The preload carries grams; the field shows kilograms. 5000 → "5.0".
  ok('grams arrive as kilograms', weights.map(i => i.value).includes('5.0'),
    weights.map(i => i.value).join(','));
  ok('and every seeded weight is converted',
    ['5.0', '3.5', '4.5', '2.0', '4.0'].every(v => weights.map(i => i.value).includes(v)),
    weights.map(i => i.value).join(','));

  // A number input, so a phone raises the numeric keypad rather than a
  // full keyboard for a field that only ever holds digits and a point.
  ok('it is a numeric field', weights[0]?.getAttribute('type') === 'number');
  ok('bounded to a weight a bowl can plausibly be',
    weights[0]?.getAttribute('min') === '0.1' && weights[0]?.getAttribute('max') === '50');

  const dishes = [...view.querySelectorAll('.row-form input[type=text]')];
  // Slot 1: change the weight only. Slot 2: clear the weight entirely.
  // Slot 3: clear the DISH, which must delete the row weight and all.
  weights[0].value = '6.25';
  weights[1].value = '';
  dishes[2].value = '';

  const saveBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Save menu');
  ok('save button present', !!saveBtn);
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 120));

  const up = [...calls].reverse().find(c => c.table === 'meal_food_mapping'
    && c.filters.some(f => f[0] === 'upsert'));
  const payload = up?.filters.find(f => f[0] === 'upsert')?.[1] || [];
  const bySlot = new Map(payload.map(r => [Number(r.food_slot), r]));

  ok('the weight is written as whole grams', bySlot.get(1)?.bowl_weight_g === 6250,
    String(bySlot.get(1)?.bowl_weight_g));
  ok('a blank weight is NULL, never zero', bySlot.get(2)?.bowl_weight_g === null,
    String(bySlot.get(2)?.bowl_weight_g));
  ok('an untouched weight is preserved', bySlot.get(4)?.bowl_weight_g === 2000,
    String(bySlot.get(4)?.bowl_weight_g));
  ok('a blanked dish is not upserted at all', !bySlot.has(3));

  const del = [...calls].reverse().find(c => c.table === 'meal_food_mapping'
    && c.filters.some(f => f[0] === 'delete'));
  ok('a blanked dish deletes the row, weight and all',
    (del?.filters.find(f => f[0] === 'in')?.[2] || []).includes(3),
    JSON.stringify(del?.filters.find(f => f[0] === 'in')?.[2]));
}

// The weekly template carries the weight too, or a materialised week would
// arrive named but unweighed and every Master row would read "set a weight".
console.log('');
console.log('[menu: the weekly template carries weight]');
await go('#/menu?locs=D&meal=Lunch&mode=week&day=3');
{
  const weights = [...view.querySelectorAll('.row-form .w-input')];
  ok('the template editor has weight fields too', weights.length >= 5,
    `got ${weights.length}`);
  ok('prefilled from the stored template',
    weights.map(i => i.value).includes('5.0'), weights.map(i => i.value).join(','));

  weights[0].value = '7.5';
  const saveBtn = [...view.querySelectorAll('button')]
    .find(b => b.textContent === 'Save template');
  ok('template save present', !!saveBtn);
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 120));

  const up = [...calls].reverse().find(c => c.table === 'meal_menu_template'
    && c.filters.some(f => f[0] === 'upsert'));
  const payload = up?.filters.find(f => f[0] === 'upsert')?.[1] || [];
  ok('the template stores grams as well as a name',
    payload.some(r => r.bowl_weight_g === 7500),
    JSON.stringify(payload.slice(0, 2)));
  ok('and still writes the dish name',
    payload.every(r => typeof r.food_name === 'string' && r.food_name.trim()));
}

// ====================================================================
//  Master — quantity per dish position, itemised by serving hall.
//
//  Expected figures are worked out by hand below, not recomputed from the
//  fixtures. A test that derived its expectation the same way the code does
//  would pass on a shared mistake, which is the one thing this block exists
//  to catch.
//
//  There is deliberately NO site-wide total to assert: rice + dal + curry
//  added together is a number nobody can act on.
// ====================================================================
console.log('');
console.log('[master: quantity per slot, per hall]');
await go('#/master');
{
  const t = text().replace(/\s+/g, ' ');
  const cards = [...view.querySelectorAll('.mslot')];
  const card = nth => cards.find(c =>
    c.querySelector('.mrow-slot')?.textContent === `Slot ${nth}`);
  const kgOf = nth => card(nth)?.querySelector('.mrow-kg')?.textContent;
  // One line per hall, in SERVING_LOCATIONS order: D, M, T.
  const hall = (nth, i) => card(nth)?.querySelectorAll('.marea')[i];
  const hallKg = (nth, i) => hall(nth, i)?.querySelector('.marea-kg')?.textContent;
  const hallBowls = (nth, i) => hall(nth, i)?.querySelector('.marea-bowls')?.textContent;

  ok('one card per reporting slot', cards.length === 5, `got ${cards.length}`);
  ok('queried slot_quantity', calls.some(c => c.table === 'slot_quantity'));

  // The site-wide total is gone on purpose — adding rice to dal to curry
  // produces a figure nobody cooks, orders or refills against.
  ok('no cross-slot grand total', view.querySelector('.master-total') === null
    && !/Total remaining/i.test(t));

  // --- slot 1: Rice at 5.0 kg everywhere, with a FAULTED stack ----------
  // D1 holds 0 + (BWL-002 faulted, excluded) + 2 = 2 trusted bowls; M1 4; T1 4.
  //   D 2 x 5.0 = 10.0    M 4 x 5.0 = 20.0    T 4 x 5.0 = 20.0   -> 50.0
  ok('each hall is itemised', card(1)?.querySelectorAll('.marea').length === 3,
    String(card(1)?.querySelectorAll('.marea').length));
  // A hall line is a THREE-column grid and must emit exactly three children.
  // A fourth — an empty placeholder for a dish that is not shown — wraps the
  // kilograms onto their own row and doubles the height of every card. That
  // shipped once; this is the guard.
  ok('every hall line fills exactly three grid cells',
    [...view.querySelectorAll('.marea')].every(a => a.children.length === 3),
    [...new Set([...view.querySelectorAll('.marea')].map(a => a.children.length))].join(','));
  ok('the hall name and its dish share one cell',
    card(3)?.querySelector('.marea-name .marea-hall') !== null
    && card(3)?.querySelector('.marea-name .marea-dish') !== null);
  ok('halls are named', /Darshanarthi/.test(card(1)?.textContent || '')
    && /Mahatma/.test(card(1)?.textContent || '')
    && /Tiffin/.test(card(1)?.textContent || ''));
  ok('Darshanarthi share of slot 1', hallKg(1, 0) === '10.0 kg', hallKg(1, 0));
  ok('Mahatma share of slot 1', hallKg(1, 1) === '20.0 kg', hallKg(1, 1));
  ok('Tiffin share of slot 1', hallKg(1, 2) === '20.0 kg', hallKg(1, 2));
  ok('the slot total is the sum of its halls', kgOf(1) === '50.0 kg', kgOf(1));
  ok('bowls are shown per hall too', hallBowls(1, 0) === '2/12',
    hallBowls(1, 0));

  // A fault no longer hides the figure — bowls_trusted already excluded the
  // faulted stack, so the number is what the healthy stacks hold. It is red,
  // and the offending hall carries the glyph.
  ok('a faulted slot still states its figure', /kg/.test(kgOf(1) || ''), kgOf(1));
  ok('and states it in red', card(1)?.querySelector('.mrow-kg.is-alert') !== null);
  ok('the fault is pinned to the hall it is in',
    hall(1, 0)?.querySelector('.st-fault') !== null
    && hall(1, 1)?.querySelector('.st-fault') === null,
    'expected the glyph on Darshanarthi only');

  // --- slot 2: Dal at 3.5 kg. M2's stack is OFFLINE ---------------------
  // D2 3+4+0 = 7 -> 24.5 kg;  M2 0 -> 0.0;  T2 0 -> 0.0
  ok('slot 2 totals 24.5 kg', kgOf(2) === '24.5 kg', kgOf(2));
  ok('Darshanarthi carries all of it', hallKg(2, 0) === '24.5 kg', hallKg(2, 0));
  ok('a hall reporting zero bowls shows 0.0 kg', hallKg(2, 1) === '0.0 kg',
    hallKg(2, 1));
  ok('the offline hall is marked', hall(2, 1)?.querySelector('.st-off') !== null);

  // --- slot 3: three dishes, three weights, Tiffin unweighed ------------
  //   D 4 x 4.5 = 18.0    M 1 x 4.0 = 4.0    T 1 x (unset)
  // Total is a lower bound; the naive "6 bowls x one weight" is wrong.
  ok('a slot with different dishes lists them all',
    /Bhaji \/ Curry \/ Sabzi/.test(card(3)?.textContent || ''),
    card(3)?.querySelector('.mrow-dish')?.textContent);
  ok('each hall is weighed against its own dish',
    hallKg(3, 0) === '18.0 kg' && hallKg(3, 1) === '4.0 kg',
    `${hallKg(3, 0)} / ${hallKg(3, 1)}`);
  ok('an unweighed hall says so rather than showing zero',
    /no weight set/.test(hallKg(3, 2) || ''), hallKg(3, 2));
  ok('and the slot total is a lower bound', kgOf(3) === '≥22.0 kg', kgOf(3));
  ok('marked as a bound', card(3)?.querySelector('.mrow-kg.is-bound') !== null);
  ok('with a link that fixes it', /Set weight/.test(card(3)?.textContent || ''));
  ok('pointed at the hall actually missing a weight',
    /locs=T/.test(card(3)?.querySelector('.mrow-fix')?.getAttribute('href') || ''),
    card(3)?.querySelector('.mrow-fix')?.getAttribute('href'));
  ok('the differing dishes are named per hall',
    /Curry/.test(hall(3, 0)?.textContent || '')
    && /Bhaji/.test(hall(3, 2)?.textContent || ''));

  // --- slot 4: clean. D 5x2.0=10.0, M 2x2.0=4.0, T 2x2.0=4.0 -> 18.0 ----
  ok('a clean slot states a plain figure', kgOf(4) === '18.0 kg', kgOf(4));
  ok('and is not marked compromised',
    card(4)?.querySelector('.mrow-kg.is-alert') === null);
  ok('its halls add up',
    hallKg(4, 0) === '10.0 kg' && hallKg(4, 1) === '4.0 kg' && hallKg(4, 2) === '4.0 kg',
    `${hallKg(4, 0)} / ${hallKg(4, 1)} / ${hallKg(4, 2)}`);
  ok('the slot carries its bowl count', /9 of 20 bowls/.test(card(4)?.textContent || ''),
    card(4)?.querySelector('.mslot-of')?.textContent);

  // --- slot 5: only Darshanarthi has a reading --------------------------
  // D5 2 x 4.0 = 8.0; M5 and T5 have no trusted stack at all.
  ok('a hall with no reading shows a dash, never 0.0 kg',
    hallKg(5, 1) === '—', hallKg(5, 1));
  ok('and its bowl count is a dash too', hallBowls(5, 1) === '—', hallBowls(5, 1));
  // The terse form must not lose the meaning — it rides in the tooltip.
  ok('the terse bowl count spells itself out on hover',
    hall(1, 0)?.querySelector('.marea-bowls')?.getAttribute('title') === '2 of 12 bowls',
    hall(1, 0)?.querySelector('.marea-bowls')?.getAttribute('title'));

  // --- the header names the meal ---------------------------------------
  ok('the header names the meal on screen', /Lunch service is on/.test(t), t.slice(0, 120));

  // The gate: M slot 6 exists in slot_overview but no device there has ever
  // reported, so it must not appear — this is what keeps the undeployed
  // backup units off the screen.
  ok('a slot no device has ever reported is hidden', !/Slot 6/.test(t));
}

// Between meals the figures come from the meal that just finished. That food
// is still on the counters so the numbers are real — but the header must say
// which meal it is, or last night's dinner reads as tonight's.
{
  for (const r of quantity) {
    r.menu_is_live = false;
    r.menu_meal_type = 'Dinner';
    r.menu_meal_date = '2026-08-19';
  }
  await go('#/stock');
  await go('#/master');
  const t2 = text();
  ok('a between-meals header names its meal', /From Dinner/.test(t2), t2.slice(0, 200));
  ok('and dates it', /19 Aug/.test(t2), t2.slice(0, 200));
  ok('and says the readings are not live', /last known/.test(t2));
  ok('the kilograms are still stated', /kg/.test(t2));
  for (const r of quantity) {
    r.menu_is_live = true;
    r.menu_meal_type = 'Lunch';
    r.menu_meal_date = '2026-08-20';
  }
  await go('#/stock');
  await go('#/master');
  ok('a live header says service is on', /Lunch service is on/.test(text()));
}

// An un-migrated database and a fleet that has never reported look identical
// on screen unless the view says which it is -- and they send someone to
// opposite places.
{
  const saved = quantity.slice();
  quantity.length = 0;
  const app = await import('../js/app.js');
  await go('#/stock');
  await go('#/master');
  ok('an empty view blames the fleet, not the database',
    /never reported|has reported yet/.test(text()) && !/migration/i.test(text()), text().slice(0, 120));
  quantity.push(...saved);
  await go('#/stock');
  await go('#/master');
  ok('and the rows come back', view.querySelectorAll('.mslot').length === 5);
}

{
  // This one needs a real REFETCH, not just a re-render: quantityError is set
  // by refresh(), and the empty-array case above only worked because
  // state.quantity holds the very array the stub hands back.
  quantityMissing = true;
  window.document.getElementById('refresh-btn').dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  await go('#/stock');
  await go('#/master');
  const t2 = text();
  ok('an un-migrated database says so instead',
    /needs one migration/i.test(t2), t2.slice(0, 160));
  ok('and names the file to run',
    /migrate_bowl_weight\.sql/.test(t2));
  ok('and does not blame the fleet', !/never reported|has reported yet/.test(t2));
  ok('the underlying error is still shown', /42P01|does not exist/.test(t2));
  quantityMissing = false;
  window.document.getElementById('refresh-btn').dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  await go('#/stock');
  await go('#/master');
  ok('and Master recovers once the view exists',
    view.querySelectorAll('.mslot').length === 5);
}

console.log('\n[swipe navigation]');
{
  const { swipeTarget } = await import('../js/app.js');
  // Master sits between Stock and Health in the tab bar, so it is what a
  // left swipe off Stock now reaches. The order under test is the tab-bar
  // order, not an arbitrary list.
  // Order is master, stock, health, menu, assign -- the tab bar's order, which
  // Master now leads. These assert the ARGUMENTS as well as the names: an
  // earlier pass renamed them to match and left the arguments describing the
  // old order, so they still passed while testing the opposite thing.
  ok('left swipe on Master lands on Stock', swipeTarget('master', -120, 10, 200, false) === 'stock');
  ok('right swipe on Stock returns to Master', swipeTarget('stock', 120, -8, 200, false) === 'master');
  ok('left swipe on Stock lands on Health', swipeTarget('stock', -120, 10, 200, false) === 'health');
  ok('right swipe on Health returns to Stock', swipeTarget('health', 120, -8, 200, false) === 'stock');
  ok('left swipe on Devices has nowhere to go', swipeTarget('assign', -120, 0, 200, false) === null);
  ok('right swipe on Master has nowhere to go', swipeTarget('master', 120, 0, 200, false) === null);
  ok('a mostly-vertical drag is a scroll, not a swipe', swipeTarget('stock', -70, 60, 200, false) === null);
  ok('a slow drag never navigates', swipeTarget('stock', -120, 0, 900, false) === null);
  ok('a short nudge never navigates', swipeTarget('stock', -30, 0, 150, false) === null);
  ok('the device page ignores swipes', swipeTarget('device', -120, 0, 200, false) === null);
  ok('an unsaved menu draft blocks the swipe', swipeTarget('menu', -120, 0, 200, true) === null);
  ok('menu swipes on to Devices when clean', swipeTarget('menu', -120, 0, 200, false) === 'assign');
}

console.log('\n[stock: outside service hours]');
{
  // The As-of line and the outside-service banner only exist when the WHOLE
  // fleet is out of window — flip the fixtures, render, flip back.
  for (const d of devices) d.in_service = false;
  await go('#/stock');
  ok('banner says these are last-known values', text().includes('Outside service hours'));
  ok('cards date their figures', /As of /.test(text()));
  // Egress guard: out of window the poll idles and the freshness line says so.
  const { pollDelay } = await import('../js/app.js');
  ok('poll runs at 15 s during service', pollDelay(true) === 15_000);
  ok('poll idles at 10 min outside service', pollDelay(false) === 600_000);
  // The note is painted by renderChrome, which runs on refresh — not on
  // navigation. Press the app's own refresh button, as a person would.
  window.document.getElementById('refresh-btn').click();
  await new Promise(r => setTimeout(r, 150));
  ok('the freshness line admits the idling',
    window.document.getElementById('freshness').textContent.includes('idles outside service'));
  for (const d of devices) d.in_service = true;
  await go('#/stock');
  ok('banner returns to live on restore', !text().includes('Outside service hours'));
  window.document.getElementById('refresh-btn').click();
  await new Promise(r => setTimeout(r, 150));
  ok('and the idle note clears',
    !window.document.getElementById('freshness').textContent.includes('idles'));
}

console.log('\n[stock: template vs daily priority]');
{
  // The dish on a card comes ONLY from the dated daily menu. The template is
  // a plan: when it covers a slot whose daily menu is missing, the card says
  // "apply it" — it never shows the planned dish as if it were recorded.
  const cards = [...view.querySelectorAll('.slot')];
  const m6 = cards.find(c => /Menu not applied/.test(c.textContent));
  ok('an unapplied plan says so instead of "No menu entered"', !!m6);
  ok('the planned dish never renders as the menu', !m6?.textContent.includes('T-PlannedDish'));
  ok('but the tooltip names it for the curious',
    /T-PlannedDish/.test(m6?.querySelector('.slot-food')?.title || ''));
  const dailyCard = cards.find(c => c.textContent.includes('Khichdi'));
  ok('a recorded daily menu shows its dish, template or not', !!dailyCard);
}

console.log('\n[health]');
await go('#/health');
const rows = [...view.querySelectorAll('.dev')];
ok('lists devices', rows.length >= 24);
ok('offline device sorts first', rows[0].textContent.includes('BWL-014'), rows[0]?.textContent.slice(0, 40));
ok('fault device near top', rows.slice(0, 3).some(r => r.textContent.includes('BWL-002')));
ok('awaiting section present', text().includes('Awaiting deployment'));
// Rows are symbolic now — the words ride tooltips and the device page.
ok('health rows carry no badge chips',
  view.querySelectorAll('.dev .badge').length === 0);
ok('the health legend pairs glyphs with words', view.querySelector('.legend-line') !== null);
ok('no-battery device shows the dashed empty case',
  [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-019'))
    ?.querySelector('.batt.lvl-none') !== null);
ok('every deployed row wears a battery glyph',
  [...view.querySelectorAll('.section')].shift()
  && [...view.querySelectorAll('.section')][0].querySelectorAll('.dev').length ===
     [...view.querySelectorAll('.section')][0].querySelectorAll('.dev .batt').length);
ok('never-reported shows no count',
  [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-025'))
    ?.querySelector('.dev-count.na')?.textContent === '—');
{
  const row21 = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-021'));
  ok('missed-service device wears the ✕ glyph', row21?.querySelector('.st-off') !== null);
  ok('and its tooltip states the silence', /last service/.test(row21?.title || ''));
  ok('its last count is red', row21?.querySelector('.dev-count.is-offline') !== null);
  ok('missed-service sorts into the top three',
    [...view.querySelectorAll('.dev')].slice(0, 3).some(r => r.textContent.includes('BWL-021')));
  ok('the row itself prints no state words', !/OFFLINE|offline/.test(row21?.textContent || ''));
  const row14 = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-014'));
  ok('offline device count is red too', row14?.querySelector('.dev-count.is-offline') !== null);
  ok('the in-window offline row wears ✕ too', row14?.querySelector('.st-off') !== null);
  const healthy = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-001'));
  ok('a healthy row never says OFFLINE', !/OFFLINE/.test(healthy?.textContent || ''));
  const faultRow = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-002'));
  ok('a fault row never gains the red count', faultRow?.querySelector('.dev-count.is-offline') == null
    && faultRow?.querySelector('.dev-count.na') !== null);
}
await go('#/health?f=offline');
ok('offline filter includes the missed-service device',
  view.querySelectorAll('.dev').length === 2
  && ['BWL-014', 'BWL-021'].every(id => text().includes(id)));

// Fault and degraded are DIFFERENT failures and must filter apart: fault is
// an impossible reading from working sensors (f2 without f1 — bowls stack),
// degraded is a sensor itself being down. They shared one filter once, so
// the "1 faults" chip opened a 3-row list.
await go('#/health?f=fault');
ok('fault filter shows exactly the impossible-reading device',
  view.querySelectorAll('.dev').length === 1 && text().includes('BWL-002'));
await go('#/health?f=degraded');
ok('degraded filter shows exactly the degraded devices',
  view.querySelectorAll('.dev').length === 3
  && ['BWL-008', 'BWL-022', 'BWL-023'].every(id => text().includes(id)));
{
  // A bound saturated at the 4-bowl ceiling is the exact answer, not ">=4".
  const full = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-022'));
  ok('full degraded stack shows plain 4, never >=4',
    full?.querySelector('.dev-count')?.textContent === '4');
  ok('and the dead sensor rides its tooltip', /Sensor down/.test(full?.title || '')
    && full?.querySelector('.st-deg') !== null);
  // Zero sensors online = zero detection: no number at all, however loudly
  // the payload claims one.
  const blind = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-023'));
  ok('zero-sensor device shows no count at all',
    blind?.querySelector('.dev-count')?.textContent === '\u2014'
    && blind?.querySelector('.dev-count.na') !== null);
  ok('its tooltip tells the real sensor story', /0 of 4 sensors/.test(blind?.title || ''));
  ok('no lower-bound claim on a blind device', !/lower bound/i.test(blind?.textContent || ''));
}
{
  const chips = [...window.document.getElementById('fleet-chips').children];
  const faultChip = chips.find(c => /faults/.test(c.textContent));
  const degrChip = chips.find(c => /degraded/.test(c.textContent));
  faultChip.dispatchEvent(new window.Event('click'));
  ok('faults chip routes to its own filter', location.hash === '#/health?f=fault');
  degrChip.dispatchEvent(new window.Event('click'));
  ok('degraded chip routes to its own filter', location.hash === '#/health?f=degraded');
}

// The not-deployed chip is GONE — static configuration, not status, and
// removing it is what fits the capsules on one phone row. The section is
// still reachable through Health's own filter.
await go('#/stock');
{
  const chips = [...window.document.getElementById('fleet-chips').children];
  ok('no not-deployed chip in the header', !chips.some(c => /not deployed/.test(c.textContent)));
  await go('#/health?f=awaiting');
  await new Promise(r => setTimeout(r, 60));
  const rows = [...view.querySelectorAll('.dev')];
  ok('shows exactly the never-reported devices', rows.length === 9,
    String(rows.length));
  ok('every row is an awaiting one',
    rows.every(r => r.querySelector('.st-none') && r.querySelector('.dev-count.na')));
  ok('no deployed device leaks in', !rows.some(r => r.textContent.includes('BWL-001')));
  ok('the hidden-count banner stays truthful', /23 of 32 devices hidden/.test(text()));
}

console.log('\n[device]');
await go('#/device/BWL-008?h=24');
ok('queried status_events', calls.some(c => c.table === 'status_events'));
ok('ordered by recorded_at',
  calls.find(c => c.table === 'status_events')?.filters.some(f => f[0] === 'order' && f[1] === 'recorded_at'));
ok('degraded banner shown', text().includes('lower bound'));
ok('level rows f1..f4', text().includes('f1') && text().includes('f4'));
ok('step chart drawn', view.querySelector('svg path[stroke="var(--series-1)"]') !== null);
ok('status timeline drawn', view.querySelectorAll('svg rect').length > 3);
ok('seq gap detected', /2 events dropped/.test(text()), text().match(/\d+ events dropped/)?.[0]);
ok('boots counted', /2 boots/.test(text()));
ok('buffered event detected', /1 buffered offline/.test(text()));
ok('event table present', view.querySelector('table tbody tr') !== null);
ok('fault row shows no count in table',
  [...view.querySelectorAll('table tbody tr')].some(r => /discontiguous/.test(r.textContent) && /—/.test(r.textContent)));
ok('five history ranges offered',
  ['2 h', '6 h', '12 h', '1 d', '2 d'].every(l =>
    [...view.querySelectorAll('.toolbar .ghost')].some(b => b.textContent === l)));
ok('stack-now draws the levels exactly once',
  view.querySelectorAll('.level-row').length === 4 && view.querySelector('.levels') === null);
ok('fault paints blue on the timeline, never red',
  view.querySelector('svg rect[fill="var(--series-1)"]') !== null);
ok('the timeline explains blank', /blank = powered off/.test(text()));
await go('#/device/BWL-021');
ok('the offline words live on the device page', /Went dark during service/.test(text()));
await go('#/device/BWL-008?h=24');
await new Promise(r => setTimeout(r, 60));

const queriesBefore = calls.filter(c => c.table === 'status_events').length;
window.dispatchEvent(new window.Event('hashchange'));
await new Promise(r => setTimeout(r, 60));
ok('history is cached across redraws',
  calls.filter(c => c.table === 'status_events').length === queriesBefore);

console.log('\n[menu]');
await go('#/menu?loc=D&meal=Lunch&date=2026-07-27');
ok('called preload rpc', calls.some(c => c.rpc === 'meal_mapping_preload'));
ok('carried draft chip shown, compact', /Carried from 26 Jul — not saved/.test(text()));
ok('source date named', text().includes('26 Jul'));
ok('five slot inputs', view.querySelectorAll('.row-form input[type=text]').length >= 5);
ok('prefilled from preload',
  [...view.querySelectorAll('.row-form input[type=text]')].map(i => i.value).includes('Khichdi'));
ok('copy-day control present', text().includes('Copy this day'));

// A background poll must not throw away what someone is typing.
const typed = view.querySelector('.row-form input[type=text]');
typed.value = 'Half-typed dish';
window.document.dispatchEvent(new window.Event('visibilitychange'));
await new Promise(r => setTimeout(r, 120));
ok('a background poll does not wipe the form',
  view.querySelector('.row-form input[type=text]')?.value === 'Half-typed dish');
typed.value = '';

const saveBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Save menu');
ok('save button present', !!saveBtn);
if (saveBtn) {
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 100));
  const up = calls.find(c => c.table === 'meal_food_mapping' && c.filters.some(f => f[0] === 'upsert'));
  ok('save upserts on the natural key',
    up?.filters.find(f => f[0] === 'upsert')?.[2]?.onConflict === 'location,meal_date,meal_type,food_slot');
  const payload = up?.filters.find(f => f[0] === 'upsert')?.[1] || [];
  ok('no blank food_name submitted', payload.length > 0 && payload.every(r => r.food_name?.trim()));
  ok('an emptied slot is a DELETE, not a blank row',
    calls.some(c => c.table === 'meal_food_mapping' && c.filters.some(f => f[0] === 'delete')));
  ok('the form redraws after saving to the same route',
    view.querySelectorAll('.row-form input[type=text]').length >= 5);
}

// The screen is watched continuously through a service, so a refresh must not
// blink, jump, or throw away what the user is doing.
console.log('\n[refresh keeps the page intact]');
await go('#/health');
{
  const firstRow = view.querySelector('.dev');
  const rowCount = view.querySelectorAll('.dev').length;
  const search = view.querySelector('input[type=search]');
  search.value = 'BWL-0';                    // typing, not yet committed
  search.focus();

  // Stamped on the live element. It survives only if that exact object is
  // reused; a rebuild would hand back a different node without it.
  firstRow.__probe = 'kept';

  // A poll, exactly as the timer fires it.
  const before = devices.find(d => d.device_id === 'BWL-001');
  before.stack_count = 3;
  before.levels = ['present', 'present', 'present', 'absent'];
  window.document.dispatchEvent(new window.Event('visibilitychange'));
  await new Promise(r => setTimeout(r, 200));

  ok('the view is not rebuilt', view.querySelector('.dev') === firstRow);
  ok('no rows are lost or duplicated', view.querySelectorAll('.dev').length === rowCount);
  ok('the same element object is reused', view.querySelector('.dev').__probe === 'kept');
  ok('focus stays in the search box', window.document.activeElement === search);
  ok('a half-typed query is not overwritten', search.value === 'BWL-0');
  ok('changed data still reaches the screen',
    view.textContent.includes('BWL-001') && /3/.test(
      [...view.querySelectorAll('.dev')]
        .find(r => r.textContent.includes('BWL-001'))?.querySelector('.dev-count')?.textContent || ''),
    [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-001'))
      ?.querySelector('.dev-count')?.textContent);
  ok('no dimming class is left behind', !view.classList.contains('is-refetching'));
  search.blur();

}

console.log('\n[menu: a touched form survives the refetch]');
// Field report: open the menu, start typing, and the background refetch lands
// half a second later — rewriting the inputs with the fetched values, so Save
// then saves the OLD menu. Once the user has typed anything, the fill must
// keep its hands off.
preloadSourceDate = '2026-08-09';        // makes the refetch differ from cache
await go('#/menu?locs=D&meal=Lunch&date=2026-08-09');
await new Promise(r => setTimeout(r, 150));
preloadSourceDate = null;                 // next fetch differs from cache again
await go('#/menu?locs=D&meal=Dinner&date=2026-08-09');
{
  // Type immediately — before the async refetch resolves.
  const input = view.querySelector('.menu-col input');
  input.value = 'My New Dish';
  input.dispatchEvent(new window.Event('input', { bubbles: true }));
  await new Promise(r => setTimeout(r, 250));   // let the refetch land
  ok('typed value survives a differing refetch',
    view.querySelector('.menu-col input')?.value === 'My New Dish',
    view.querySelector('.menu-col input')?.value);

  const saveBtn = [...view.querySelectorAll('button')].find(b => /^Save/.test(b.textContent));
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  const up = [...calls].reverse().find(c => c.table === 'meal_food_mapping'
    && c.filters.some(f => f[0] === 'upsert'));
  const payload = up?.filters.find(f => f[0] === 'upsert')?.[1] || [];
  ok('and Save writes what was typed, not the fetched old menu',
    payload.some(r => r.food_name === 'My New Dish'),
    JSON.stringify(payload.map(r => r.food_name)));
}

console.log('\n[menu: multi-area verification grid]');
await go('#/menu?meal=Lunch&date=2026-08-06');
{
  ok('all three areas selected by default', view.querySelectorAll('.menu-col').length === 3);
  ok('a column per area, five slots each',
    view.querySelectorAll('.menu-col .row-form input[type=text]').length === 15);
  ok('meal capsules, exactly one pressed',
    [...view.querySelectorAll('button')].filter(b =>
      ['Breakfast', 'Lunch', 'Dinner'].includes(b.textContent)
      && b.getAttribute('aria-pressed') === 'true').length === 1);
  ok('every column carries its own status chip',
    [...view.querySelectorAll('.menu-col-head .badge')].length === 3);
  ok('save button covers all areas',
    [...view.querySelectorAll('button')].some(b => b.textContent === 'Save all 3 areas'));

  const mahatma = [...view.querySelectorAll('button')].find(b => b.textContent === 'Mahatma');
  mahatma.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 150));
  ok('deselecting an area drops its column', view.querySelectorAll('.menu-col').length === 2);
  ok('and the url says which remain', /locs=D(%2C|,)T/.test(location.hash), location.hash);

  const before = calls.filter(c => c.table === 'meal_food_mapping'
    && c.filters.some(f => f[0] === 'upsert')).length;
  const saveBtn = [...view.querySelectorAll('button')].find(b => /^Save all 2 areas$/.test(b.textContent));
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  const after = calls.filter(c => c.table === 'meal_food_mapping'
    && c.filters.some(f => f[0] === 'upsert')).length;
  ok('save-all writes each selected area', after - before === 2, String(after - before));
}

console.log('\n[menu: weekly template]');
await go('#/menu?loc=D&meal=Lunch&mode=week');
{
  ok('mode toggle present and pressed',
    [...view.querySelectorAll('button')].some(b =>
      b.textContent === 'Weekly template' && b.getAttribute('aria-pressed') === 'true'));
  ok('seven day chips', ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat']
    .every(d => [...view.querySelectorAll('button')].some(b => b.textContent === d)));
  ok('queried the template table', calls.some(c => c.table === 'meal_menu_template'));
  ok('coverage map shows entered counts', view.querySelector('table') !== null
    && /5/.test(view.querySelector('table')?.textContent || ''));
  ok('slot inputs prefilled from the template',
    [...view.querySelectorAll('.row-form input[type=text]')].some(i => i.value === 'T-Rice'));
  ok('no literal null leaked into the url', !location.hash.includes('null'));

  const saveBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Save template');
  ok('save button present', !!saveBtn);
  if (saveBtn) {
    saveBtn.dispatchEvent(new window.Event('click'));
    await new Promise(r => setTimeout(r, 120));
    const up = [...calls].reverse().find(c =>
      c.table === 'meal_menu_template' && c.filters.some(f => f[0] === 'upsert'));
    ok('template save upserts on its natural key',
      up?.filters.find(f => f[0] === 'upsert')?.[2]?.onConflict
        === 'location,weekday,meal_type,food_slot');
    const payload = up?.filters.find(f => f[0] === 'upsert')?.[1] || [];
    ok('no blank names, weekday attached',
      payload.length > 0 && payload.every(r =>
        r.food_name?.trim() && r.weekday >= 0 && r.weekday <= 6));
  }
}

console.log('\n[menu: the day survives every action]');
// The bug all three reviewers found independently: go() rebuilt the query
// without `day`, so Save/Reload/Copy and the Area/Meal selects snapped the
// editor back to today's weekday — an admin editing Wednesday was silently
// looking at (and then overwriting) Saturday.
await go('#/menu?loc=D&meal=Lunch&mode=week&day=3');
{
  ok('day chip 3 (Wednesday) is pressed',
    [...view.querySelectorAll('button')].some(b =>
      b.textContent === 'Wed' && b.getAttribute('aria-pressed') === 'true'));
  const saveBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Save template');
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 120));
  ok('day survives Save', /day=3/.test(location.hash), location.hash);
  ok('still on Wednesday after Save',
    [...view.querySelectorAll('button')].some(b =>
      b.textContent === 'Wed' && b.getAttribute('aria-pressed') === 'true'));

  // Meals are capsules now, not a dropdown.
  const dinnerBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Dinner');
  dinnerBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 120));
  ok('day survives a meal switch', /day=3/.test(location.hash), location.hash);
  ok('no dropdowns anywhere in week mode',
    [...view.querySelectorAll('select')].every(sel =>
      ![...sel.options].some(o => ['Dinner', 'Darshanarthi'].includes(o.textContent))));
}

console.log('\n[menu: copy-weekday really overwrites]');
await go('#/menu?loc=D&meal=Lunch&mode=week&day=3');
{
  const copyBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Copy');
  copyBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 150));
  const del = [...calls].reverse().find(c =>
    c.table === 'meal_menu_template' && c.filters.some(f => f[0] === 'delete'));
  ok('copy deletes the target days before writing',
    !!del && del.filters.some(f => f[0] === 'in' && f[1] === 'weekday'));
}

console.log('\n[menu: All areas — type once, save everywhere]');
await go('#/menu?locs=all&meal=Lunch&date=2026-08-06');
{
  ok('All-areas capsule pressed',
    [...view.querySelectorAll('button')].some(b =>
      b.textContent === 'All areas' && b.getAttribute('aria-pressed') === 'true'));
  ok('one combined column', view.querySelectorAll('.menu-col').length === 1
    && /All areas/.test(view.querySelector('.menu-col h3')?.textContent || ''));
  ok('slots prefill where the areas agree',
    [...view.querySelectorAll('.menu-col input')].some(i => i.value === 'Khichdi'));
  // Tiffin's slot 2 differs, and the view must both say so and protect it.
  ok('the disagreement is named on the chip', /differs in slot/.test(text()));
  ok('the differing slot stays blank with the marker',
    [...view.querySelectorAll('.menu-col input')].some(i =>
      i.value === '' && i.placeholder.includes('differs')));

  // Field report: a blanked slot that silently came back read as "delete is
  // broken". A blank now DELETES where every area agreed (unambiguous), and
  // still writes nothing where they differ.
  const inputs = [...view.querySelectorAll('.menu-col input')];
  inputs[0].value = '';                 // slot 1: areas agreed -> delete it
  const saveBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Save to all 3 areas');
  ok('save button says all 3 areas', !!saveBtn);
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  const up = [...calls].reverse().find(c => c.table === 'meal_food_mapping'
    && c.filters.some(f => f[0] === 'upsert'));
  const payload = up?.filters.find(f => f[0] === 'upsert')?.[1] || [];
  ok('one upsert fans out to D, M and T',
    ['D', 'M', 'T'].every(l => payload.some(r => r.location === l)));
  ok('three filled slots x three areas = 9 rows (blank + differing skipped)',
    payload.length === 9, String(payload.length));
  const del = [...calls].reverse().find(c => c.table === 'meal_food_mapping'
    && c.filters.some(f => f[0] === 'delete')
    && c.filters.some(f => f[0] === 'in' && f[1] === 'location'));
  const delSlots = del?.filters.find(f => f[0] === 'in' && f[1] === 'food_slot')?.[2] || [];
  ok('the agreed blank deletes across every area', !!del && delSlots.includes(1));
  ok('the differing slot is never deleted', !delSlots.includes(2), JSON.stringify(delSlots));

  // Clicking a single area while in All narrows to that area.
  const tiffin = [...view.querySelectorAll('button')].find(b => b.textContent === 'Tiffin');
  tiffin.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 150));
  ok('clicking one area exits All into that area',
    /locs=T(&|$)/.test(location.hash) && view.querySelectorAll('.menu-col').length === 1
    && /Tiffin/.test(view.querySelector('.menu-col h3')?.textContent || ''));
}

console.log('\n[menu: All areas in the weekly template]');
await go('#/menu?mode=week&locs=all&meal=Lunch');
{
  ok('one combined template column', view.querySelectorAll('.menu-col').length === 1
    && /All areas/.test(view.querySelector('.menu-col h3')?.textContent || ''));
  // Template fixture: D has Lunch slots 1-5, M only slot 6, T nothing — the
  // areas disagree, so the chip must say so and the differing slots stay blank.
  ok('disagreement is named', /differs in slot/.test(text()));
  ok('differing slots stay blank with the marker',
    [...view.querySelectorAll('.menu-col input')].some(i =>
      i.value === '' && i.placeholder.includes('differs')));
  ok('save to all 3 areas offered',
    [...view.querySelectorAll('button')].some(b => b.textContent === 'Save to all 3 areas'));
}

console.log('\n[menu: weekly template is multi-area too]');
await go('#/menu?mode=week&meal=Lunch');
{
  ok('three template columns by default', view.querySelectorAll('.menu-col').length === 3);
  ok('area capsules multi-pressed',
    [...view.querySelectorAll('button')].filter(b =>
      ['Darshanarthi', 'Mahatma', 'Tiffin'].includes(b.textContent)
      && b.getAttribute('aria-pressed') === 'true').length === 3);
  ok('one coverage map per area', [...view.querySelectorAll('.table-wrap table')].length === 3);
  ok('save covers all areas',
    [...view.querySelectorAll('button')].some(b => b.textContent === 'Save template — 3 areas'));
  ok('copy-area hides when the source is ambiguous',
    ![...view.querySelectorAll('button')].some(b => b.textContent === 'Copy area'));

  const tiffin = [...view.querySelectorAll('button')].find(b => b.textContent === 'Tiffin');
  tiffin.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 150));
  ok('deselecting drops to two columns', view.querySelectorAll('.menu-col').length === 2);

  const saveBtn = [...view.querySelectorAll('button')].find(b => /Save template — 2 areas/.test(b.textContent));
  const before = calls.filter(c => c.table === 'meal_menu_template'
    && c.filters.some(f => f[0] === 'upsert')).length;
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  const after = calls.filter(c => c.table === 'meal_menu_template'
    && c.filters.some(f => f[0] === 'upsert')).length;
  ok('template save-all writes each selected area', after - before === 2, String(after - before));
}

console.log('\n[menu: weekly per-area delete works]');
await go('#/menu?mode=week&locs=D&meal=Lunch&day=3');
{
  const input = [...view.querySelectorAll('.menu-col input')].find(i => i.value === 'T-Rice');
  ok('slot prefilled from the template', !!input);
  input.value = '';
  const saveBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Save template');
  saveBtn.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  const del = [...calls].reverse().find(c => c.table === 'meal_menu_template'
    && c.filters.some(f => f[0] === 'delete')
    && c.filters.some(f => f[0] === 'eq' && f[1] === 'weekday'));
  ok('blanking a planned dish deletes it from the template', !!del);
  const delSlots = del?.filters.find(f => f[0] === 'in' && f[1] === 'food_slot')?.[2] || [];
  ok('and exactly that slot', delSlots.length === 1 && delSlots.includes(1),
    JSON.stringify(delSlots));
}

console.log('\n[menu: apply template to dates]');
await go('#/menu?loc=D&meal=Lunch&mode=week');
{
  const applyBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Apply to dates');
  ok('apply card present', !!applyBtn);
  if (applyBtn) {
    applyBtn.dispatchEvent(new window.Event('click'));
    await new Promise(r => setTimeout(r, 120));
    const call = [...calls].reverse().find(c => c.rpc === 'meal_template_apply');
    ok('calls the apply RPC', !!call);
    ok('defaults to fill-gaps, never overwrite', call?.args?.p_overwrite === false);
    ok('scoped to the visible location', call?.args?.p_location === 'D');
    ok('reports written and skipped meals',
      /5 dishes written/.test(text()) && /1 meal skipped/.test(text()));
  }
}

console.log('\n[menu: template draft in the daily editor]');
// A preload whose source_date equals the requested date is the template-draft
// marker; the daily editor must say so rather than claiming a carry-over.
preloadSourceDate = '2026-08-05';
await go('#/menu?loc=D&meal=Lunch&date=2026-08-05');
{
  ok('template draft chip shown', /From template — save to record/.test(text()));
  ok('and it does not claim a carry-over', !/Carried from/.test(text()));
}
preloadSourceDate = null;

console.log('\n[session drop mid-edit: recovered silently]');
// The v1.2 field report: the page "flickers (blackout) once" and settings are
// uneditable for 10-15 s. Cause: a session hiccup swapped in the full-screen
// connecting gate and force-rendered on the way back. Recovery must now be
// invisible: no gate, no wiped form, a fresh sign-in underneath.
await go('#/menu?loc=D&meal=Lunch&date=2026-08-06');
{
  const input = view.querySelector('.row-form input[type=text]');
  input.value = 'Half-typed while session dies';
  const before = anonSignIns;

  session = null;                          // the session evaporates
  authCallback('SIGNED_OUT', null);        // supabase-js notices
  await new Promise(r => setTimeout(r, 200));

  ok('app pane never hidden', !hidden('app'));
  ok('connecting gate never shown', hidden('connecting'));
  ok('login form never shown', hidden('login'));
  ok('a new session was minted underneath', anonSignIns === before + 1);
  ok('the half-typed dish survived',
    view.querySelector('.row-form input[type=text]')?.value === 'Half-typed while session dies');
}

console.log('\n[menu: copy template across areas]');
await go('#/menu?loc=D&meal=Lunch&mode=week&day=3');
{
  const btn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Copy area');
  ok('copy-area card present', !!btn);
  if (btn) {
    btn.dispatchEvent(new window.Event('click'));
    await new Promise(r => setTimeout(r, 150));
    const del = [...calls].reverse().find(c =>
      c.table === 'meal_menu_template'
      && c.filters.some(f => f[0] === 'delete')
      && c.filters.some(f => f[0] === 'in' && f[1] === 'location'));
    ok('replaces the target areas (delete first)', !!del);
    const up = [...calls].reverse().find(c =>
      c.table === 'meal_menu_template' && c.filters.some(f => f[0] === 'upsert'));
    const payload = up?.filters.find(f => f[0] === 'upsert')?.[1] || [];
    ok('writes both other areas, never the source',
      payload.length > 0
      && payload.every(r => ['M', 'T'].includes(r.location))
      && ['M', 'T'].every(l => payload.some(r => r.location === l)));
  }
}

console.log('\n[health: finding a specific device]');
await go('#/health?f=offline');
{
  const shown = [...view.querySelectorAll('.dev')].map(a => a.textContent);
  ok('a filter hides healthy devices', !shown.some(t => t.includes('BWL-001')));
  ok('and says so loudly, with a count',
    /\d+ of \d+ devices hidden/.test(text()), text().match(/\d+ of \d+ devices hidden/)?.[0]);
  ok('offers one click back to everything', /Show all devices/.test(text()));
}
await go('#/health?f=offline&q=BWL-001');
{
  const shown = [...view.querySelectorAll('.dev')].map(a => a.getAttribute('href'));
  ok('search finds a healthy device despite the filter',
    shown.some(hh => hh.includes('BWL-001')), shown.join(' '));
  ok('search narrows to just it', shown.length === 1);
  ok('search says it overrode the filters', /Search ignores the filters/.test(text()));
}
await go('#/health?q=Tiffin');
ok('search also matches the label', view.querySelectorAll('.dev').length >= 5);
await go('#/health?q=zzzz');
ok('a search with no hits says so', /No device matches/.test(text()));

console.log('\n[devices]');
await go('#/assign');
ok('lists all 32 plus a header', view.querySelectorAll('.assign-row').length === 33);
ok('shared position shown as count, not conflict', text().includes('3 stacks here'));
ok('save disabled until edited',
  [...view.querySelectorAll('button')].find(b => /Save changes/.test(b.textContent))?.disabled === true);

const locSel = view.querySelectorAll('.assign-row select')[0];
locSel.value = 'M';
locSel.dispatchEvent(new window.Event('change'));
const saveDev = [...view.querySelectorAll('button')].find(b => /^Save 1 change$/.test(b.textContent));
ok('edit enables save', !!saveDev && !saveDev.disabled);
if (saveDev) {
  saveDev.dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 100));
  const upd = calls.find(c => c.table === 'devices' && c.filters.some(f => f[0] === 'update'));
  const patch = upd?.filters.find(f => f[0] === 'update')?.[1];
  ok('updates devices row', patch?.location === 'M');
  ok('never writes device_id', !patch || !('device_id' in patch));
  ok('writes only grantable columns',
    !patch || Object.keys(patch).every(k => ['location', 'food_slot', 'label', 'timezone'].includes(k)));
}

// An area with no slot is a legitimate configuration, not an error: plenty of
// units sit in an area without being tied to a serving position, so the form
// saves it as given rather than arguing with it.
console.log('\n[devices: an unassigned slot is allowed]');
await go('#/assign');
{
  const row = [...view.querySelectorAll('.assign-row')]
    .find(r => r.querySelector('.did')?.textContent.startsWith('BWL-004'));
  const [locSel, slotSel] = row.querySelectorAll('select');
  const saveOf = () => [...view.querySelectorAll('button')]
    .find(b => /^Save/.test(b.textContent));

  slotSel.value = '';
  slotSel.dispatchEvent(new window.Event('change'));
  ok('clearing the slot is accepted', saveOf()?.disabled === false, saveOf()?.textContent);
  ok('no error is shown on the row',
    !row.classList.contains('invalid') && row.querySelector('.row-error') === null);
  ok('both dropdowns stay usable', locSel.disabled === false && slotSel.disabled === false);

  saveOf().dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 100));
  const upd = [...calls].reverse()
    .find(c => c.table === 'devices' && c.filters.some(f => f[0] === 'update'));
  const patch = upd?.filters.find(f => f[0] === 'update')?.[1];
  ok('a null slot is written through', !!patch && patch.food_slot === null, JSON.stringify(patch));
}

// =======================================================================
//  Buffer platforms -- every BWL-xxx after supabase/cutover_buffers.sql.
//
//  Everything above ran against kind-less stacks and slot rows with no
//  buffer columns: the database BEFORE migrate_buffer.sql, and those
//  assertions passing untouched is the proof the dashboard still reads one.
//  This block splices a post-cut-over set of positions in -- D/7, D/8, M/7,
//  M/8, none of which the fixtures above use -- asserts, and takes it back
//  out, so no count above moves. Expected figures are worked by hand.
//
//    D/7  BWL-101 ok 42.3 kg food, 3 bowls, gross 49.8 kg
//         BWL-102 settling                -> the position is >= 50.4 kg
//         LDC-107 ok 8.1 kg on the counter
//    D/8  BWL-103 ok, tared and empty     -> 0.0 kg, never "No data"
//    M/7  BWL-104 ok 30.0 kg, 3 bowls UNCONFIRMED
//    M/8  BWL-105 no_cells                -> NULL -> "No data", never >=
// =======================================================================
console.log('\n[buffers: one 200 kg cell, food in kilograms]');
{
  const live = { reported: true, updated_at: iso(now - 20_000), stale_for: '00:00:20',
    in_service: true, offline: false, awaiting_deployment: false, data_is_stale: false,
    missed_last_service: false, stack_count: null, stack_status: null, levels: null,
    sensors_online: null, battery_mv: 4020, battery_level: 'good', charging: null,
    uptime_s: 3120, firmware: 'V1.20 261003', mac: '28:84:85:47:AB:01',
    timezone: 'Asia/Kolkata', current_meal: 'Lunch' };
  const none = { weight_g: null, gross_g: null, bowls: null, bowls_confirmed: null };
  const buf = (id, loc, slot, w) => ({ ...live, device_id: id, kind: 'buffer',
    location: loc, food_slot: slot, label: `${AREA[loc]} buffer ${slot}`,
    current_food: 'Pulao', cells_online: 1, counts_per_gram: 20.7,
    net_counts: w.gross_g == null ? null : Math.round(w.gross_g * 20.7), ...w });
  const extraDevices = [
    buf('BWL-101', 'D', 7, { weight_state: 'ok', weight_g: 42300, gross_g: 49800,
                             bowls: 3, bowls_confirmed: true }),
    buf('BWL-102', 'D', 7, { ...none, weight_state: 'settling' }),
    { ...live, device_id: 'LDC-107', kind: 'scale', location: 'D', food_slot: 7,
      label: 'Darshanarthi slot 7 scale', current_food: 'Pulao', ...none,
      weight_state: 'ok', weight_g: 8100, cells_online: 3, counts_per_gram: 106.857,
      net_counts: 865542 },
    buf('BWL-103', 'D', 8, { weight_state: 'ok', weight_g: 0, gross_g: 0,
                             bowls: 0, bowls_confirmed: true }),
    buf('BWL-104', 'M', 7, { weight_state: 'ok', weight_g: 30000, gross_g: 37500,
                             bowls: 3, bowls_confirmed: false }),
    buf('BWL-105', 'M', 8, { ...none, weight_state: 'no_cells', cells_online: 0 }),
  ];
  const slotRow = (loc, n, o) => ({ location: loc, food_slot: n, current_food: 'Pulao',
    current_meal: 'Lunch', devices: 0, devices_reported: 0, bowls_capacity: 0,
    bowls_trusted: null, bowls_reported: null, any_fault: false, any_degraded: false,
    any_battery_warn: false, any_offline: false, any_missed_service: false,
    oldest_update: iso(now - 20_000), scales: 0, scales_ok: 0, measured_weight_g: null,
    scale_issues: [], bowl_weight_g: null, buffer_issues: 0, buffer_unconfirmed: null,
    ...o });
  const extraSlots = [
    slotRow('D', 7, { scales: 1, scales_ok: 1, measured_weight_g: 8100,
      buffers: 2, buffers_ok: 1, buffer_issues: 1, buffer_measured_g: 42300,
      buffer_g: 42300, buffer_bowls: 3, buffer_unconfirmed: false, weight_g: 50400 }),
    slotRow('D', 8, { buffers: 1, buffers_ok: 1, buffer_measured_g: 0, buffer_g: 0,
      buffer_bowls: 0, buffer_unconfirmed: false, weight_g: 0 }),
    slotRow('M', 7, { buffers: 1, buffers_ok: 1, buffer_measured_g: 30000,
      buffer_g: 30000, buffer_bowls: 3, buffer_unconfirmed: true, weight_g: 30000 }),
    slotRow('M', 8, { buffers: 1, buffers_ok: 0, buffer_issues: 1,
      buffer_measured_g: null, buffer_g: null, buffer_bowls: null, weight_g: null }),
  ];
  const area = (loc, o) => ({ location: loc, food_name: 'Pulao', bowl_weight_g: null,
    bowls_trusted: null, bowls_capacity: 0, devices: 0, est_weight_g: null,
    capacity_weight_g: null, scales: 0, measured_weight_g: null, ...o });
  const qRow = (n, o) => ({ food_slot: n, current_meal: 'Lunch', menu_meal_type: 'Lunch',
    menu_meal_date: '2026-08-20', menu_is_live: true, dishes: ['Pulao'],
    bowl_weight_g: null, devices: 0, bowls_capacity: 0, bowls_trusted: null,
    est_weight_g: null, capacity_weight_g: null, est_is_partial: false,
    areas_without_weight: [], any_fault: false, any_degraded: false,
    any_battery_warn: false, any_offline: false, any_missed_service: false,
    oldest_update: iso(now - 20_000), ...o });
  const extraQuantity = [
    // 42.3 + 8.1 at D, 30.0 at M = 80.4, a bound because BWL-102 is settling.
    qRow(7, { areas: [
        area('D', { weight_g: 50400, measured_weight_g: 8100, scales: 1, buffers: 2,
                    buffers_ok: 1, buffer_g: 42300, buffer_bowls: 3, buffer_unconfirmed: false }),
        area('M', { weight_g: 30000, buffers: 1, buffers_ok: 1, buffer_g: 30000,
                    buffer_bowls: 3, buffer_unconfirmed: true })],
      scales: 1, scales_ok: 1, measured_weight_g: 8100, buffer_g: 72300, counter_g: 8100,
      weight_g: 80400, buffers: 3, buffers_ok: 2, buffer_measured_g: 72300,
      buffer_bowls: 6, buffer_unconfirmed: true, weight_is_partial: true }),
    // 0 at D (empty) and nothing known at M: a bound on ZERO, which is a figure.
    qRow(8, { areas: [
        area('D', { weight_g: 0, buffers: 1, buffers_ok: 1, buffer_g: 0,
                    buffer_bowls: 0, buffer_unconfirmed: false }),
        area('M', { weight_g: null, buffers: 1, buffers_ok: 0, buffer_g: null,
                    buffer_bowls: null, buffer_unconfirmed: null })],
      measured_weight_g: null, buffer_g: 0, counter_g: null, weight_g: 0,
      buffers: 2, buffers_ok: 1, buffer_measured_g: 0, buffer_bowls: 0,
      buffer_unconfirmed: false, weight_is_partial: true }),
  ];
  devices.push(...extraDevices);
  slots.push(...extraSlots);
  quantity.push(...extraQuantity);
  window.document.getElementById('refresh-btn').dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));

  // --- Stock ------------------------------------------------------------
  await go('#/stock');
  const sCard = (loc, n) => [...(view.querySelector(`.area-${loc}`)?.querySelectorAll('.slot') || [])]
    .find(c => c.querySelector('.slot-pos')?.textContent === `Slot ${n}`);
  const kgOn = c => c?.querySelector('.slot-kg')?.textContent;
  ok('a buffer position leads with kilograms of food', kgOn(sCard('D', 7)) === '≥50.4 kg',
    kgOn(sCard('D', 7)));
  ok('...a lower bound, because one buffer there is settling',
    /lower bound/.test(sCard('D', 7)?.querySelector('.slot-kg')?.title || ''));
  ok('...and the tooltip splits buffer from counter',
    /42\.3 kg in the buffer \+ 8\.1 kg on the counter/.test(
      sCard('D', 7)?.querySelector('.slot-kg')?.title || ''),
    sCard('D', 7)?.querySelector('.slot-kg')?.title);
  ok('the line under it is the bowls on the buffer',
    sCard('D', 7)?.querySelector('.slot-of')?.textContent === '3 bowls buffered',
    sCard('D', 7)?.querySelector('.slot-of')?.textContent);
  ok('a position with no stack draws no bowl meter', sCard('D', 7)?.querySelector('.meter') == null);
  ok('a tared, empty buffer reads 0.0 kg, not "No data"', kgOn(sCard('D', 8)) === '0.0 kg',
    kgOn(sCard('D', 8)));
  ok('an unconfirmed count wears a ?',
    /^3\? bowls/.test(sCard('M', 7)?.querySelector('.slot-of')?.textContent || ''),
    sCard('M', 7)?.querySelector('.slot-of')?.textContent);
  ok('...but the kilograms are not a bound for it', kgOn(sCard('M', 7)) === '30.0 kg',
    kgOn(sCard('M', 7)));
  ok('a buffer with no weight is "No data"',
    sCard('M', 8)?.querySelector('.slot-nodata')?.textContent === 'No data');
  ok('...and never a bound', !/≥/.test(sCard('M', 8)?.querySelector('.slot-figure')?.textContent || 'x'));
  ok('the device line shows the food, not a bowl count',
    [...(sCard('D', 7)?.querySelectorAll('.dev-line') || [])]
      .find(l => /BWL-101/.test(l.textContent))?.querySelector('.ct')?.textContent === '42.3 kg');

  // --- Health -----------------------------------------------------------
  await go('#/health');
  const hRow = id => [...view.querySelectorAll('.dev')].find(r => r.textContent.includes(id));
  ok('a buffer draws ONE cell, not three',
    hRow('BWL-101')?.querySelectorAll('.cells i').length === 1,
    String(hRow('BWL-101')?.querySelectorAll('.cells i').length));
  ok('...while the counter still draws three',
    hRow('LDC-107')?.querySelectorAll('.cells i').length === 3);
  ok('a buffer row reads in kilograms',
    hRow('BWL-101')?.querySelector('.dev-count.is-weight')?.textContent === '42.3 kg');
  ok('its tooltip carries the bowls and the gross',
    /3 bowls · gross 49\.8 kg/.test(hRow('BWL-101')?.querySelector('.dev-count')?.title || ''),
    hRow('BWL-101')?.querySelector('.dev-count')?.title);
  ok('an unconfirmed count says so',
    /3 bowls \(unconfirmed\)/.test(hRow('BWL-104')?.querySelector('.dev-count')?.title || ''));
  ok('a buffer with no cell answering is a fault', hRow('BWL-105')?.querySelector('.st-fault') != null);
  ok('a healthy buffer is not "no reading"', hRow('BWL-101')?.querySelector('.st-ok') != null);
  ok('no level ladder is drawn for a buffer', hRow('BWL-101')?.querySelector('.levels') == null);
  await go('#/health?f=fault');
  ok('...and the Faults filter lists it', text().includes('BWL-105'));

  // --- Device -----------------------------------------------------------
  await go('#/device/BWL-101?h=24');
  await new Promise(r => setTimeout(r, 80));
  const hist = calls.filter(c => c.table === 'weight_samples');
  ok('buffer history queries weight_samples',
    hist.some(c => c.filters.some(f => f[0] === 'eq' && f[1] === 'device_id' && f[2] === 'BWL-101')));
  ok("...with select('*'), so it works before and after migrate_buffer.sql",
    hist.every(c => c.filters.some(f => f[0] === 'select' && f[1] === '*')));
  ok('...and never status_events',
    !calls.some(c => c.table === 'status_events'
      && c.filters.some(f => f[0] === 'eq' && f[2] === 'BWL-101')));
  ok('the card is titled for a buffer', /Buffer now/.test(text()) && !/Counter now/.test(text()));
  ok('the card draws one cell', view.querySelectorAll('.cells.lg i').length === 1,
    String(view.querySelectorAll('.cells.lg i').length));
  ok('the device line says 1 of 1', /1 of 1 converting/.test(text()));
  const kvOf = k => [...view.querySelectorAll('dl.kv dt')]
    .find(dt => dt.textContent === k)?.nextElementSibling?.textContent;
  ok('the bowls have a row', kvOf('Bowls') === '3 bowls', kvOf('Bowls'));
  ok('...and the gross, in kilograms', /^49\.8 kg/.test(kvOf('Gross') || ''), kvOf('Gross'));
  ok('the history chart is food on this buffer', /Food on this buffer/.test(text()));
  {
    const heads = [...view.querySelectorAll('table thead th')].map(t => t.textContent);
    ok('the history table has Bowls and Gross columns',
      heads.includes('Bowls') && heads.includes('Gross'), heads.join(','));
    ok('an unconfirmed sample shows its ?',
      [...view.querySelectorAll('table tbody tr')].some(r => /3\?/.test(r.textContent)));
  }
  await go('#/device/BWL-104');
  ok('the device page spells out unconfirmed',
    /unconfirmed: remembered from before a power cycle/.test(text()));
  await go('#/device/LDC-107');
  ok('a counter keeps its own title and three cells',
    /Counter now/.test(text()) && view.querySelectorAll('.cells.lg i').length === 3);

  // --- Master -----------------------------------------------------------
  await go('#/master');
  {
    const mCard = n => [...view.querySelectorAll('.mslot')]
      .find(c => c.querySelector('.mrow-slot')?.textContent === `Slot ${n}`);
    const hall = (n, i) => mCard(n)?.querySelectorAll('.marea')[i];
    ok('a buffer slot states buffer + counter as a bound',
      mCard(7)?.querySelector('.mrow-kg')?.textContent === '≥80.4 kg',
      mCard(7)?.querySelector('.mrow-kg')?.textContent);
    ok('...marked as one', mCard(7)?.querySelector('.mrow-kg.is-bound') != null);
    ok('...with no "Set weight" link, which could not fix a buffer that is down',
      mCard(7)?.querySelector('.mrow-fix') == null);
    ok('the slot carries its buffered bowls, ? when unconfirmed',
      mCard(7)?.querySelector('.mslot-of')?.textContent === '6? bowls buffered',
      mCard(7)?.querySelector('.mslot-of')?.textContent);
    ok("each hall's bowls stand alone",
      hall(7, 0)?.querySelector('.marea-bowls')?.textContent === '3'
      && hall(7, 1)?.querySelector('.marea-bowls')?.textContent === '3?',
      `${hall(7, 0)?.querySelector('.marea-bowls')?.textContent} / ${hall(7, 1)?.querySelector('.marea-bowls')?.textContent}`);
    ok("each hall's kilograms are its buffer + counter",
      hall(7, 0)?.querySelector('.marea-kg')?.textContent === '50.4 kg'
      && hall(7, 1)?.querySelector('.marea-kg')?.textContent === '30.0 kg');
    ok('the hall tooltip says the buffer was weighed',
      /42\.3 kg buffered \(3 bowls, weighed on the buffer\) \+ 8\.1 kg weighed at the counter/
        .test(hall(7, 0)?.querySelector('.marea-kg')?.title || ''),
      hall(7, 0)?.querySelector('.marea-kg')?.title);
    ok('a settling buffer is not a fault on its hall -- only the slot is a bound',
      hall(7, 0)?.querySelector('.st-fault') == null);
    ok('zero is a figure even beside a dead buffer: >= 0.0 kg, not No data',
      mCard(8)?.querySelector('.mrow-kg')?.textContent === '≥0.0 kg',
      mCard(8)?.querySelector('.mrow-kg')?.textContent);
    ok('a buffer hall with no weight is a dash, never "no weight set"',
      hall(8, 1)?.querySelector('.marea-kg')?.textContent === '—'
      && !/no weight set/.test(mCard(8)?.textContent || ''),
      hall(8, 1)?.querySelector('.marea-kg')?.textContent);
    ok('the dead buffer marks its hall as a fault', hall(8, 1)?.querySelector('.st-fault') != null);
  }

  // --- Menu: kg / bowl only where a stack can use it --------------------
  // Stacks are still in the fleet, so the field shows...
  await go('#/menu?locs=D&meal=Lunch&date=2026-09-01');
  ok('kg / bowl is visible while a stack exists',
    [...view.querySelectorAll('.row-form .w-input')].length >= 5
    && [...view.querySelectorAll('.row-form .w-input')].every(i => !i.hasAttribute('hidden')));
  // ...and with only buffers and scales left it hides -- but stays in the form.
  const stacks = devices.filter(d => d.kind == null);
  devices.splice(0, devices.length, ...devices.filter(d => d.kind != null));
  window.document.getElementById('refresh-btn').dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  await go('#/stock');
  await go('#/menu?locs=D&meal=Lunch&date=2026-09-01');
  {
    const w = [...view.querySelectorAll('.row-form .w-input')];
    ok('with no stack left, kg / bowl is hidden', w.length >= 5 && w.every(i => i.hasAttribute('hidden')),
      `${w.length} inputs, ${w.filter(i => i.hasAttribute('hidden')).length} hidden`);
    ok('...its header too',
      [...view.querySelectorAll('.row-head span')].find(s => s.textContent === 'kg / bowl')
        ?.hasAttribute('hidden') === true);
    const saveBtn = [...view.querySelectorAll('button')].find(b => b.textContent === 'Save menu');
    saveBtn?.dispatchEvent(new window.Event('click'));
    await new Promise(r => setTimeout(r, 150));
    const up = [...calls].reverse().find(c => c.table === 'meal_food_mapping'
      && c.filters.some(f => f[0] === 'upsert'));
    const payload = up?.filters.find(f => f[0] === 'upsert')?.[1] || [];
    ok('a save still carries the stored weights through the hidden field',
      payload.some(r => Number(r.food_slot) === 4 && r.bowl_weight_g === 2000),
      JSON.stringify(payload.map(r => r.bowl_weight_g)));
  }
  devices.splice(0, devices.length, ...stacks, ...devices);

  // Out again, so nothing after this block sees the post-cut-over positions.
  for (const [arr, extra] of [[devices, extraDevices], [slots, extraSlots],
                              [quantity, extraQuantity]]) {
    for (const x of extra) arr.splice(arr.indexOf(x), 1);
  }
  window.document.getElementById('refresh-btn').dispatchEvent(new window.Event('click'));
  await new Promise(r => setTimeout(r, 200));
  await go('#/menu?locs=D&meal=Lunch&date=2026-09-01');
  ok('the fixtures are restored, and kg / bowl is back',
    devices.length === 32
    && [...view.querySelectorAll('.row-form .w-input')].every(i => !i.hasAttribute('hidden')));
}

// =======================================================================
//  Hubs and node health -- supabase/migrate_hubs.sql.
//
//  The battery moved to the HUB (one per area, mains + a backup cell). A
//  platform reports none, and carries instead what its hub polls from it:
//  supply_mv, crc_errors, responding, no_load_g -- NULL when not measured.
//  Every legacy stack above kept its OWN battery, and those assertions
//  passing untouched is the proof that still holds. Spliced in and taken
//  out again, the buffer block's pattern, so no count elsewhere moves.
//
//    HUB-D charging    HUB-M on battery (mains lost)    HUB-T low, unknown
//    D/7  BWL-201 healthy node, all four measured
//         BWL-202 not answering the hub                 -> fault
//         LDC-207 I2C: supply and CRC NULL, +650 g      -> drift warning
//    T/7  BWL-204 4.42 V supply, 140 CRC errors         -> warnings
//    M/7  BWL-203 the four columns ABSENT, and the panel's old battery
//         'low' still on the row                        -> ranks nothing
// =======================================================================
console.log('\n[hubs: the battery is the hub\'s, platforms report node health]');
{
  const live = { reported: true, updated_at: iso(now - 20_000), stale_for: '00:00:20',
    in_service: true, offline: false, awaiting_deployment: false, data_is_stale: false,
    missed_last_service: false, stack_count: null, stack_status: null, levels: null,
    sensors_online: null, battery_mv: null, battery_level: null, charging: null,
    uptime_s: 3120, firmware: 'V1.21 261004', mac: '28:84:85:47:AB:02',
    timezone: 'Asia/Kolkata', current_meal: 'Lunch' };
  const hub = (loc, o) => ({ ...live, device_id: `HUB-${loc}`, kind: 'hub', location: loc,
    food_slot: null, label: `${AREA[loc]} hub`, current_food: null, ...o });
  const plat = (id, kind, loc, o) => ({ ...live, device_id: id, kind, location: loc,
    food_slot: 7, label: `${AREA[loc]} slot 7`, current_food: 'Pulao',
    weight_state: 'ok', weight_g: 20000, cells_online: kind === 'buffer' ? 1 : 3,
    gross_g: kind === 'buffer' ? 25000 : null, bowls: kind === 'buffer' ? 2 : null,
    bowls_confirmed: kind === 'buffer' ? true : null,
    counts_per_gram: 20.7, net_counts: 517500,
    supply_mv: 4980, crc_errors: 2, responding: true, no_load_g: 12, ...o });
  const hubs = [
    hub('D', { battery_mv: 4110, battery_level: 'good', charging: true }),
    hub('M', { battery_mv: 3790, battery_level: 'medium', charging: false }),
    hub('T', { battery_mv: 3560, battery_level: 'low', charging: null }),
  ];
  const legacy = plat('BWL-203', 'buffer', 'M', { battery_mv: 3600, battery_level: 'low' });
  for (const k of ['supply_mv', 'crc_errors', 'responding', 'no_load_g']) delete legacy[k];
  const plats = [
    plat('BWL-201', 'buffer', 'D'),
    plat('BWL-202', 'buffer', 'D', { supply_mv: 4890, crc_errors: null, no_load_g: null,
                                     responding: false }),
    plat('LDC-207', 'scale', 'D', { supply_mv: null, crc_errors: null, no_load_g: 650 }),
    plat('BWL-204', 'buffer', 'T', { supply_mv: 4420, crc_errors: 140 }),
    legacy,
  ];
  const extraSlots = ['D', 'M', 'T'].map(loc => ({ location: loc, food_slot: 7,
    current_food: 'Pulao', current_meal: 'Lunch', devices: 0, devices_reported: 0,
    bowls_capacity: 0, bowls_trusted: null, bowls_reported: null, any_fault: false,
    any_degraded: false, any_battery_warn: false, any_offline: false,
    any_missed_service: false, oldest_update: iso(now - 20_000), scales: 0, scales_ok: 0,
    measured_weight_g: null, scale_issues: [], bowl_weight_g: null, buffers: 1,
    buffers_ok: 1, buffer_issues: 0, buffer_measured_g: 20000, buffer_g: 20000,
    buffer_bowls: 2, buffer_unconfirmed: false, weight_g: 20000 }));
  const refetch = async () => {
    window.document.getElementById('refresh-btn').dispatchEvent(new window.Event('click'));
    await new Promise(r => setTimeout(r, 200));
  };
  const chipN = label => [...window.document.getElementById('fleet-chips').children]
    .find(c => c.textContent.replace(/^\d+/, '') === label)?.querySelector('b')?.textContent;
  const hRow = id => [...view.querySelectorAll('.dev')].find(r => r.textContent.includes(id));
  const kvOf = k => [...view.querySelectorAll('dl.kv dt')]
    .find(dt => dt.textContent === k)?.nextElementSibling;
  const platIds = plats.map(p => p.device_id);

  devices.push(...hubs, ...plats);
  slots.push(...extraSlots);
  await refetch();

  // --- the header chip ----------------------------------------------------
  // Two legacy stacks (BWL-005 critical, BWL-011 low) + HUB-T low. BWL-203's
  // leftover 'low' is a platform's, so it is not the area's battery.
  ok('the battery chip counts hubs and stacks, not platforms', chipN('battery') === '3',
    chipN('battery'));

  // --- Health -------------------------------------------------------------
  await go('#/health');
  const first = view.querySelector('.section');
  ok('hubs lead the roster, in a section of their own',
    first?.querySelector('h2')?.textContent === 'Hubs'
    && first.querySelectorAll('.dev').length === 3
    && [...first.querySelectorAll('.dev')].every(r => /HUB-/.test(r.textContent)));
  ok('a hub row draws its battery', hRow('HUB-T')?.querySelector('.batt.lvl-low') != null);
  ok('...and says charging, on battery or unknown beside it',
    hRow('HUB-D')?.querySelector('.dev-count')?.textContent === 'charging'
    && hRow('HUB-M')?.querySelector('.dev-count')?.textContent === 'on battery'
    && hRow('HUB-T')?.querySelector('.dev-count')?.textContent === 'charging unknown');
  ok('a charging hub wears the bolt, one on battery does not',
    hRow('HUB-D')?.querySelector('.batt .bolt') != null
    && hRow('HUB-M')?.querySelector('.batt .bolt') == null);
  ok('a hub draws no level ladder and no cells',
    !!hRow('HUB-D') && hRow('HUB-D').querySelector('.levels, .cells') == null);
  ok('a hub on battery warns that mains is lost',
    /mains lost/.test(hRow('HUB-M')?.title || '')
    && hRow('HUB-M')?.classList.contains('sev-warning'));
  ok('a hub with no slot is not "Not assigned to a position"',
    !/Not assigned/.test(hRow('HUB-D')?.title || ''), hRow('HUB-D')?.title);
  ok('no platform row draws a battery bar',
    platIds.every(id => hRow(id) && !hRow(id).querySelector('.batt')));
  ok('...nor is ever "No battery detected"',
    platIds.every(id => !/no battery detected/i.test(hRow(id)?.title || '')));
  ok('the battery column carries node health instead',
    hRow('BWL-201')?.querySelector('.devc-node')?.textContent
      === '4.98 V · 2 CRC · responding · no-load +12 g',
    hRow('BWL-201')?.querySelector('.devc-node')?.textContent);
  ok('a part not measured is left out, never printed as 0',
    hRow('LDC-207')?.querySelector('.devc-node')?.textContent === 'responding · no-load +650 g',
    hRow('LDC-207')?.querySelector('.devc-node')?.textContent);
  ok('a platform with none of the columns reads —, not 0',
    hRow('BWL-203')?.querySelector('.devc-node')?.textContent === '—',
    hRow('BWL-203')?.querySelector('.devc-node')?.textContent);
  ok('a healthy node is not flagged', hRow('BWL-201')?.classList.contains('sev-good'));
  ok('not answering the hub is a fault',
    hRow('BWL-202')?.querySelector('.st-fault') != null
    && hRow('BWL-202')?.classList.contains('sev-critical')
    && /Not answering the hub/.test(hRow('BWL-202')?.title || ''));
  ok('a zero drifted 500 g or more warns',
    hRow('LDC-207')?.classList.contains('sev-warning')
    && /Zero drifted — reads \+650 g empty/.test(hRow('LDC-207')?.title || ''),
    hRow('LDC-207')?.title);
  ok('low supply and checksum errors warn',
    hRow('BWL-204')?.classList.contains('sev-warning')
    && /Low supply at the node — 4\.42 V/.test(hRow('BWL-204')?.title || '')
    && /140 checksum errors/.test(hRow('BWL-204')?.title || ''),
    hRow('BWL-204')?.title);
  ok("a platform's leftover battery ranks nothing",
    hRow('BWL-203')?.classList.contains('sev-good') && !/Battery/.test(hRow('BWL-203')?.title || ''));
  await go('#/health?f=fault');
  ok('...and the Faults filter lists the unreachable node', text().includes('BWL-202'));
  await go('#/health?f=battery');
  ok('the Battery filter lists the hub, not the platform',
    text().includes('HUB-T') && !text().includes('BWL-203'));

  // --- Stock --------------------------------------------------------------
  await go('#/stock');
  const line = id => [...view.querySelectorAll('.dev-line')]
    .find(l => l.getAttribute('href')?.includes(id));
  ok('Stock: no battery on a platform line',
    platIds.every(id => line(id) && !line(id).querySelector('.batt')));
  ok('...its tooltip carries node health, never "no battery detected"',
    /node: 4\.98 V/.test(line('BWL-201')?.title || '')
    && !/no battery/i.test(line('BWL-203')?.title || ''), line('BWL-201')?.title);
  ok('...while a legacy stack keeps its own',
    line('BWL-005')?.querySelector('.batt.lvl-critical') != null);
  ok('a hub is never drawn on a Stock card', !line('HUB-D'));

  // --- Device -------------------------------------------------------------
  await go('#/device/HUB-M');
  ok('a hub page has the Power card, charging spelt out',
    [...view.querySelectorAll('.chart-title')].some(t => t.textContent === 'Power')
    && kvOf('Charging')?.textContent === 'on battery', kvOf('Charging')?.textContent);
  ok('...with the mains-lost badge', /On battery — mains lost/.test(text()));
  ok('...and no reading card and no history',
    !/Stack now|Counter now|Buffer now|History/.test(text()));
  await go('#/device/LDC-207?h=24');
  await new Promise(r => setTimeout(r, 80));
  ok('a platform page has Node health, not Power',
    [...view.querySelectorAll('.chart-title')].some(t => t.textContent === 'Node health')
    && ![...view.querySelectorAll('.chart-title')].some(t => t.textContent === 'Power'));
  ok('...linking to its hub',
    kvOf('Hub')?.querySelector('a')?.getAttribute('href') === '#/device/HUB-D');
  ok('...a NULL is a dash, never 0',
    kvOf('Supply')?.textContent === '—' && kvOf('Checksum errors')?.textContent === '—');
  ok('...the drift is signed', kvOf('No-load')?.textContent === '+650 g when last empty',
    kvOf('No-load')?.textContent);
  ok('...and its history table has no Battery column',
    view.querySelector('table thead') != null
    && ![...view.querySelectorAll('table thead th')].some(t => t.textContent === 'Battery'));

  // --- a database WITHOUT migrate_hubs.sql: no hub rows, no node columns ---
  for (const x of hubs) devices.splice(devices.indexOf(x), 1);
  await refetch();
  await go('#/health');
  ok('without hubs there is no Hubs section',
    ![...view.querySelectorAll('.section h2')].some(e => e.textContent === 'Hubs'));
  ok('...the chip is the stacks alone again', chipN('battery') === '2', chipN('battery'));
  ok('...and a platform is still never "No battery detected"',
    !/no battery detected/i.test(hRow('BWL-203')?.title || '')
    && hRow('BWL-203')?.classList.contains('sev-good'));
  await go('#/device/BWL-203');
  ok('...its page says there is no hub and dashes the rest',
    kvOf('Hub')?.textContent === 'no hub in this area'
    && ['Link', 'Supply', 'Checksum errors', 'No-load'].every(k => kvOf(k)?.textContent === '—'));

  for (const x of plats) devices.splice(devices.indexOf(x), 1);
  for (const x of extraSlots) slots.splice(slots.indexOf(x), 1);
  await refetch();
  ok('the fixtures are restored', devices.length === 32 && slots.length === 16
    && chipN('battery') === '2');
}


// =======================================================================
//  Load cells -- slotQuantity() against the columns migrate_loadcell.sql
//  adds. Unit tests on the function rather than DOM tests through Master,
//  because what changed is the DECISION (which of two numbers to believe)
//  and the rendering of that decision is one line either way.
//
//  Everything above this point ran with a slot_quantity fixture that has
//  NONE of these columns, which is not an oversight -- it is the
//  un-migrated database, and those 300-odd assertions passing is what
//  proves Master still works on one.
// =======================================================================
{
  console.log('\n[load cells: the measured-vs-estimated decision]');
  const { slotQuantity } = await import(new URL('js/domain.js', webDir).href);

  // A slot with no load cells at all: unchanged from before the migration.
  const est = slotQuantity({
    bowls_trusted: 10, bowls_capacity: 20, est_weight_g: 50000,
    weight_g: 50000, measured_weight_g: null,
  });
  ok('a slot with no scale reads from the buffer alone',
    est.headline === '50.0 kg', est.headline);

  // The same slot, now measured. The measurement wins and the row says so.
  // THE SUM, not a choice. 50 kg buffered plus 4.2 kg on the counter is
  // 54.2 kg of food at that position -- picking one would discard the other.
  const meas = slotQuantity({
    bowls_trusted: 10, bowls_capacity: 20, est_weight_g: 50000,
    weight_g: 54200, measured_weight_g: 4200,
  });
  ok('buffer and counter are added, not chosen between',
    meas.headline === '54.2 kg', meas.headline);
  ok('...and keeps both terms so a screen can show the split',
    meas.estGrams === 50000 && meas.measGrams === 4200);

  // THE CASE THAT MADE THIS NECESSARY. Every stack silent, the scale still
  // weighing. The old code returned 'No data' here and dropped the
  // measurement -- at exactly the moment it is the only thing left.
  const scaleOnly = slotQuantity({
    bowls_trusted: null, bowls_capacity: 0, est_weight_g: null,
    weight_g: 7500, measured_weight_g: 7500,
  });
  ok('a scale-only slot shows its weight, not "No data"',
    scaleOnly.kind === 'count' && scaleOnly.headline === '7.5 kg', scaleOnly.headline);
  ok('...and says where the figure came from',
    /Weighed at the counter/.test(scaleOnly.note), scaleOnly.note);

  // Neither a count nor a weight is still nothing, and must stay nothing.
  const nothing = slotQuantity({
    bowls_trusted: null, bowls_capacity: 0, est_weight_g: null, weight_g: null,
  });
  ok('nothing reporting is still "No data"', nothing.kind === 'nodata');

  // ZERO IS A REAL WEIGHT -- the distinction the whole schema turns on. A
  // measured, empty, tared platform is 0.0 kg and means "refill me"; null
  // means nobody knows. They must not collapse.
  const empty = slotQuantity({
    bowls_trusted: 0, bowls_capacity: 20, est_weight_g: 0,
    weight_g: 0, measured_weight_g: 0,
  });
  ok('a measured empty counter reads 0.0 kg, not "No data"',
    empty.kind === 'count' && empty.headline === '0.0 kg', empty.headline);

  // A hall with a load cell is not a hall missing a weight, so the >= bound
  // and the "Set weight" prompt must not fire on it.
  const mixed = slotQuantity({
    bowls_trusted: 10, bowls_capacity: 20, est_weight_g: 20000,
    weight_g: 45000, measured_weight_g: 25000, est_is_partial: false,
  });
  ok('a weighed hall does not read as a lower bound',
    mixed.partial === false && !mixed.headline.startsWith('≥'), mixed.headline);

  // Bowls known, no weight from either source: the pre-existing branch, which
  // must survive untouched.
  const noWeight = slotQuantity({
    bowls_trusted: 7, bowls_capacity: 20, est_weight_g: null, weight_g: null,
  });
  ok('bowls with no weight still fall back to the count',
    noWeight.kind === 'noweight' && noWeight.headline === '7 bowls', noWeight.headline);

  // --- after migrate_buffer.sql: weight_g is NULL-not-0 ---------------------
  // A per-bowl weight typed and no stack left makes est_weight_g a real 0.
  // Falling back to it on a NULL weight_g would print 0.0 kg for a slot
  // nobody can see; the fallback is only for a row with no weight_g at all.
  const unseen = slotQuantity({
    bowls_trusted: null, bowls_capacity: 0, est_weight_g: 0, weight_g: null,
    buffers: 2, buffers_ok: 0, weight_is_partial: false,
  });
  ok('a NULL weight beside a 0 estimate is "No data", not 0.0 kg',
    unseen.kind === 'nodata' && unseen.headline === 'No data', unseen.headline);
  const nullBound = slotQuantity({
    bowls_trusted: null, bowls_capacity: 0, weight_g: null, weight_is_partial: true,
  });
  ok('never a bound on a NULL', nullBound.headline === 'No data' && !nullBound.partial);
  const emptyBuf = slotQuantity({
    bowls_trusted: null, bowls_capacity: 0, weight_g: 0, buffer_measured_g: 0,
    buffer_g: 0, buffers: 1, buffers_ok: 1, buffer_bowls: 0, weight_is_partial: false,
  });
  ok('an empty buffer is 0.0 kg', emptyBuf.headline === '0.0 kg', emptyBuf.headline);
  const downBuf = slotQuantity({
    bowls_trusted: null, bowls_capacity: 0, weight_g: 40000, buffer_measured_g: 40000,
    buffers: 2, buffers_ok: 1, buffer_bowls: 3, weight_is_partial: true,
  });
  ok('a buffer down makes the figure a bound', downBuf.headline === '≥40.0 kg', downBuf.headline);
  ok('...that the Menu tab cannot fix', downBuf.partial && !downBuf.estPartial);
  ok('...and says where the figure came from', /Weighed in the buffer/.test(downBuf.note),
    downBuf.note);
}

// ---------------------------------------------------------------------
// [demo mode agrees with the view]
//
// THE GAP THAT LET THIS DRIFT. Every assertion above drives slotQuantity()
// with rows written by hand, so all of them passed while mock.js -- the thing
// that actually feeds demo mode -- still implemented the RETRACTED precedence
// rule, still emitted a weight_source field the view had deleted, and still
// missed the third condition on est_is_partial. A suite that only tests the
// consumer cannot see the producer go wrong.
//
// So these drive the mock's own synthesis and assert the three rules the view
// enforces. This is the mock/live divergence CLAUDE.md names, caught on the
// dashboard rather than on the panel.
// ---------------------------------------------------------------------
{
  console.log('\n[demo mode agrees with the view]');
  const { createMockClient } = await import(new URL('js/mock.js', webDir).href);
  const client = createMockClient();
  const { data: slots } = await client.from('slot_quantity').select('*');

  ok('demo mode produces slots at all', Array.isArray(slots) && slots.length > 0,
    String(slots && slots.length));

  const allAreas = slots.flatMap(r => r.areas || []);

  // 1. No field the view does not publish. A mock that invents a column
  //    teaches the UI to depend on something the API will never send.
  const RETIRED = ['weight_source', 'weight_mismatch', 'weight_mismatch_g',
                   'weight_mismatch_areas'];
  const leaked = RETIRED.filter(k =>
    slots.some(r => k in r) || allAreas.some(a => k in a));
  ok('no retired column is still manufactured', leaked.length === 0, leaked.join(','));

  // 2. weight_g = buffer_g + counter_g, per hall AND at the slot, NULL only
  //    when both are. The precedence rule reported 18.0 kg at a position
  //    holding 54.0 kg. The buffer term is now what the platforms WEIGH plus
  //    any stack's bowls x kg -- and the demo has no stack left, so it is the
  //    weighed food alone.
  const sum2 = (b, c) => (b == null && c == null) ? null : (b || 0) + (c || 0);
  const badArea = allAreas.find(a => a.weight_g !== sum2(a.buffer_g, a.measured_weight_g));
  ok("a hall's weight is buffer + counter", !badArea, JSON.stringify(badArea));
  const badSlot = slots.find(r => r.weight_g !== sum2(r.buffer_g, r.counter_g));
  ok("a slot's weight_g is buffer_g + counter_g", !badSlot, JSON.stringify(badSlot));

  const bothTerms = slots.find(r => r.buffer_g != null && r.counter_g != null);
  ok('at least one slot exercises BOTH terms', !!bothTerms,
    'no slot in the fixture has a weighed buffer and a scale together');
  ok('the buffer term is weighed, not estimated: no stack, no estimate',
    slots.every(r => r.est_weight_g == null && r.bowls_trusted == null));

  // 2b. Every buffer row obeys the contract the database CHECKs: numbers only
  //     when ok, food = gross - bowls x 2.5 kg, and never a stack column.
  const { data: rowsNow } = await client.from('device_overview').select('*');
  const buffers = rowsNow.filter(d => d.kind === 'buffer');
  ok('every BWL is a buffer after the cut-over',
    buffers.length === 32 && rowsNow.every(d => d.kind !== 'stack'));
  const badBuf = buffers.find(d => {
    const ok_ = d.weight_state === 'ok';
    return ok_ !== (d.weight_g != null) || ok_ !== (d.gross_g != null)
      || ok_ !== (d.bowls != null) || (d.bowls == null) !== (d.bowls_confirmed == null)
      || (ok_ && d.weight_g !== d.gross_g - d.bowls * 2500)
      || d.stack_count != null || d.levels != null || d.weight_state === 'cells_partial';
  });
  ok('every buffer row obeys the contract', !badBuf, JSON.stringify(badBuf));
  const hubRows = rowsNow.filter(d => d.kind === 'hub');
  ok('demo mode serves three hubs, one on battery and one low',
    hubRows.length === 3 && hubRows.every(d => d.food_slot == null && d.reported)
    && hubRows.some(d => d.charging === false) && hubRows.some(d => d.battery_level === 'low'));
  ok('...and no platform carries a battery',
    rowsNow.filter(d => d.kind !== 'hub')
      .every(d => d.battery_mv == null && d.battery_level == null && d.charging == null));
  ok('...but node health, one unreachable, one drifted, the I2C ones part-NULL',
    rowsNow.some(d => d.responding === false) && rowsNow.some(d => d.no_load_g >= 500)
    && ['LDC-001', 'BWL-001'].every(id => {
      const d = rowsNow.find(r => r.device_id === id);
      return d.supply_mv == null && d.crc_errors == null && d.responding === true;
    })
    && rowsNow.filter(d => d.supply_mv != null)
      .every(d => d.supply_mv >= 4800 && d.supply_mv <= 5050));
  const seeded = s => buffers.some(d => s(d));
  ok('the demo seeds every buffer state the UI has a rule for',
    seeded(d => d.bowls_confirmed === false) && seeded(d => d.offline)
    && seeded(d => d.weight_state === 'no_cells') && seeded(d => d.weight_state === 'settling')
    && seeded(d => d.weight_g === 0));
  const { data: hist } = await client.from('weight_samples').select('*')
    .eq('device_id', 'BWL-001').order('recorded_at', { ascending: false }).limit(1000);
  ok('demo mode serves a buffer its weight history', hist.length > 100
    && hist.every(r => r.device_id === 'BWL-001' && 'bowls' in r && 'gross_g' in r));
  const { data: series } = await client.from('slot_stock_series')
    .select('location, food_slot, at_ts, total_g');
  ok('...and Master its per-hall stock curve',
    series.length > 0 && series.every(p => p.location && p.at_ts));

  // 3. A hall with no dish owes no weight, so it cannot make a slot partial.
  const wrongPartial = slots.find(r => (r.areas_without_weight || [])
    .some(loc => (r.areas || []).find(a => a.location === loc && a.food_name == null)));
  ok('a hall with no dish is never listed as missing a weight',
    !wrongPartial, wrongPartial && String(wrongPartial.food_slot));
}

// ---------------------------------------------------------------------
// [the area tooltip describes the number it is attached to]
//
// It branched on weight_source, which the view deleted with the precedence
// rule -- so live it was always undefined and always claimed the whole figure
// came from bowls, printing "2 bowls x 18.0 kg" beside 54.0 kg.
// ---------------------------------------------------------------------
{
  console.log('\n[the area tooltip describes the number it is attached to]');
  const { areaWeightNote } = await import(new URL('js/views/master.js', webDir).href);

  const both = areaWeightNote(
    { est_weight_g: 36000, measured_weight_g: 18000, bowl_weight_g: 18000, scales: 1 }, 2);
  ok('both terms are named, with the counter kept separate',
    /36\.0 kg buffered/.test(both) && /18\.0 kg weighed at the counter/.test(both), both);
  ok('...and it does not claim the whole figure came from bowls',
    !/^2 bowls/.test(both), both);

  const ctrOnly = areaWeightNote(
    { est_weight_g: null, measured_weight_g: 18000, bowl_weight_g: null, scales: 2 }, null);
  ok('a counter-only hall says so, and how many scales',
    /18\.0 kg, weighed at the counter by 2 scales/.test(ctrOnly), ctrOnly);

  const bufOnly = areaWeightNote(
    { est_weight_g: 36000, measured_weight_g: null, bowl_weight_g: 18000, scales: 0 }, 2);
  ok('a buffer-only hall reads exactly as it did before load cells existed',
    bufOnly === '2 bowls x 18.0 kg, from the Menu tab.', bufOnly);

  ok('a hall with neither gets no tooltip rather than an empty one',
    areaWeightNote({ est_weight_g: null, measured_weight_g: null,
                     bowl_weight_g: null, scales: 0 }, null) === undefined, 'defined');
}

console.log(`\n${fails.length ? `FAILED (${fails.length}): ${fails.join(' | ')}` : 'ALL PASS'}`);
process.exit(fails.length ? 1 : 0);
