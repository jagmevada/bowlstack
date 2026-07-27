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
const now = Date.now();
const iso = ms => new Date(ms).toISOString();

const devices = [];
ASSIGN.forEach(([loc, slot, n], i) => {
  const id = `BWL-${n}`;
  // One device seeded into each awkward state the UI has a rule for.
  const fault = id === 'BWL-002';
  const degr = id === 'BWL-008';
  const off = id === 'BWL-014';
  const noBatt = id === 'BWL-019';
  const critB = id === 'BWL-005';
  const never = id === 'BWL-024';
  const levels = fault ? ['absent', 'present', 'absent', 'absent']
    : degr ? ['present', 'present', 'unknown', 'absent']
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
    updated_at: never ? null : iso(now - (off ? 20 * 60_000 : 25_000)),
    stale_for: '00:00:25', in_service: true,
    offline: off, awaiting_deployment: never, data_is_stale: false,
    stack_count: never ? null : levels.filter(l => l === 'present').length,
    stack_status: never ? null : fault ? 'discontiguous' : degr ? 'degraded' : 'ok',
    levels: never ? null : levels,
    sensors_online: never ? null : degr ? 3 : 4,
    battery_mv: never ? null : noBatt ? null : critB ? 3320 : 4020,
    battery_level: never ? null : noBatt ? null : critB ? 'critical' : 'good',
    charging: never ? null : id === 'BWL-001',
    uptime_s: never ? null : 7412, firmware: never ? null : '0.2.0',
    mac: never ? null : 'A0:B1:C2:D3:E4:F5',
  });
});
for (let n = 25; n <= 32; n++) {
  devices.push({
    device_id: `BWL-0${n}`, location: 'R', food_slot: null, label: 'Reserved',
    timezone: 'Asia/Kolkata', current_food: null, current_meal: 'Lunch',
    reported: false, updated_at: null, stale_for: null, in_service: true,
    offline: false, awaiting_deployment: true, data_is_stale: false,
    stack_count: null, stack_status: null, levels: null, sensors_online: null,
    battery_mv: null, battery_level: null, charging: null, uptime_s: null,
    firmware: null, mac: null,
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
    });
  }
}
slots.push({
  location: 'M', food_slot: 6, current_food: null, current_meal: 'Lunch',
  devices: 1, devices_reported: 0, bowls_capacity: 4, bowls_trusted: null,
  bowls_reported: null, any_fault: false, any_degraded: false,
  any_battery_warn: false, any_offline: false, oldest_update: null,
});

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

const preload = [1, 2, 3, 4, 5].map(s => ({
  food_slot: s, food_name: MENU.D[s], source_date: '2026-07-26', is_saved: false,
}));

// ---- fake PostgREST ---------------------------------------------------
const calls = [];
function builder(table) {
  const rec = { table, filters: [] };
  calls.push(rec);
  const api = {};
  for (const m of ['select', 'order', 'eq', 'gte', 'in', 'limit', 'upsert', 'update', 'delete']) {
    api[m] = (...args) => { rec.filters.push([m, ...args]); return api; };
  }
  api.then = (res, rej) => {
    const data = table === 'device_overview' ? devices
      : table === 'slot_overview' ? slots
      : table === 'status_events' ? events
      : table === 'meal_food_mapping' ? [{ meal_type: 'Lunch', food_slot: 1, food_name: 'Rice' }]
      : [];
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
      return Promise.resolve({ data: preload, error: null });
    },
    auth: {
      getSession: async () => ({ data: { session }, error: null }),
      getUser: async () => ({ data: { user: session?.user ?? null }, error: null }),
      onAuthStateChange: () => ({ data: { subscription: { unsubscribe() {} } } }),
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

console.log('\n[shell]');
ok('app pane visible', !hidden('app'));
ok('queried device_overview', calls.some(c => c.table === 'device_overview'));
ok('queried slot_overview', calls.some(c => c.table === 'slot_overview'));
ok('fleet chips rendered', window.document.getElementById('fleet-chips').children.length >= 5);
ok('offline chip counts 1', /1offline/.test(window.document.getElementById('fleet-chips').textContent));
ok('meal indicator live', /Lunch/.test(window.document.getElementById('meal-indicator').textContent));

console.log('\n[stock]');
ok('renders three areas', ['Darshanarthi', 'Mahatma', 'Tiffin'].every(a => text().includes(a)));
ok('dish names resolved', text().includes('Khichdi') && text().includes('Chaas'));
ok('fault slot shows no count', text().includes('Check station'));
ok('no-data slot says No data', text().includes('No data'));
ok('degraded slot marked lower bound', text().includes('Lower bound'));
ok('offline stack surfaced', text().includes('Stack offline'));
ok('stack pills link to devices', view.querySelector('a[href^="#/device/"]') !== null);
ok('capacity comes from the view', text().includes('of 4 bowls') && text().includes('of 12 bowls'));

console.log('\n[health]');
await go('#/health');
const rows = [...view.querySelectorAll('.dev')];
ok('lists devices', rows.length >= 24);
ok('offline device sorts first', rows[0].textContent.includes('BWL-014'), rows[0]?.textContent.slice(0, 40));
ok('fault device near top', rows.slice(0, 3).some(r => r.textContent.includes('BWL-002')));
ok('awaiting section present', text().includes('Awaiting deployment'));
ok('no-battery device says No battery', text().includes('No battery'));
ok('never-reported shows no count', text().includes('Never reported'));
await go('#/health?f=offline');
ok('filter narrows to offline', view.querySelectorAll('.dev').length === 1);

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

const queriesBefore = calls.filter(c => c.table === 'status_events').length;
window.dispatchEvent(new window.Event('hashchange'));
await new Promise(r => setTimeout(r, 60));
ok('history is cached across redraws',
  calls.filter(c => c.table === 'status_events').length === queriesBefore);

console.log('\n[menu]');
await go('#/menu?loc=D&meal=Lunch&date=2026-07-27');
ok('called preload rpc', calls.some(c => c.rpc === 'meal_mapping_preload'));
ok('draft banner shown for inherited menu', text().includes('Not saved for this date'));
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

console.log(`\n${fails.length ? `FAILED (${fails.length}): ${fails.join(' | ')}` : 'ALL PASS'}`);
process.exit(fails.length ? 1 : 0);
