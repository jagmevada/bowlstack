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
  const missed = id === 'BWL-021';   // slept through the last completed window
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
    updated_at: never ? null : iso(now - (off ? 20 * 60_000 : missed ? 30 * 3600_000 : 25_000)),
    stale_for: '00:00:25', in_service: true,
    offline: off, awaiting_deployment: never, data_is_stale: false,
    missed_last_service: missed,
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

// Weekly template fixture: D has a full Lunch row set for every weekday.
const templateRows = [];
for (let wd = 0; wd <= 6; wd++) {
  for (let slot = 1; slot <= 5; slot++) {
    templateRows.push({ location: 'D', weekday: wd, meal_type: 'Lunch',
                        food_slot: slot, food_name: `T-${MENU.D[slot]}` });
  }
}

// ---- fake PostgREST ---------------------------------------------------
const calls = [];
let preloadSourceDate = null;   // set per-test to mark a template draft
let authCallback = null;        // captured so tests can fire auth events
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
      : table === 'meal_menu_template' ? templateRows
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
      if (name === 'meal_template_apply') {
        return Promise.resolve({ data: [
          { meal_date: args.p_from, meal_type: 'Lunch', written: 5, skipped: false },
          { meal_date: args.p_from, meal_type: 'Dinner', written: 0, skipped: true },
        ], error: null });
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
ok('dish names resolved', text().includes('Khichdi') && text().includes('Chaas'));
ok('fault slot shows no count', text().includes('Check station'));
ok('no-data slot says No data', text().includes('No data'));
ok('degraded slot marked lower bound', text().includes('Lower bound'));
ok('offline stack surfaced', text().includes('Stack offline'));
ok('stack pills link to devices', view.querySelector('a[href^="#/device/"]') !== null);
ok('capacity comes from the view', text().includes('of 4 bowls') && text().includes('of 12 bowls'));

console.log('\n[offline: last value kept, in red]');
{
  // D5 holds BWL-021 (missed the last completed window) and BWL-022 (fine).
  const cards = [...view.querySelectorAll('.slot')];
  const d5 = cards.find(c => c.textContent.includes('Khichdi'));
  ok('missed-service slot keeps its number', d5?.querySelector('.slot-count') !== null);
  ok('and the number is red', d5?.querySelector('.slot-count.is-offline') !== null);
  ok('with the last-known caption', /last known/.test(d5?.textContent || ''));
  ok('and a hatched meter', d5?.querySelector('.meter.is-stale') !== null);
  ok('badge counts the silent stacks', /Stack offline \u2014 1 of 2/.test(d5?.textContent || ''));
  ok('the pill keeps the number beside the word',
    [...(d5?.querySelectorAll('a.badge') || [])].some(a => /\d+ offline/.test(a.textContent)));

  // M2 is BWL-014: offline AND at 0 bowls -- both meanings must coexist.
  const m2 = cards.find(c => c.querySelector('.slot-count.is-critical.is-offline'));
  ok('an offline+critical slot carries both classes', m2 != null);

  // The fault and no-data treatments are not regressed: neither renders a
  // numeral, so neither can have gained a red one.
  const faultCard = cards.find(c => /Check station/.test(c.textContent));
  ok('fault slot still shows no count at all', faultCard?.querySelector('.slot-count') == null);
  const noData = cards.find(c => /No data/.test(c.textContent));
  ok('no-data slot stays grey', noData?.querySelector('.slot-count') == null
    && noData?.querySelector('.slot-nodata') !== null);

  ok('the banner names the silent devices',
    /devices are offline|device is offline/.test(text())
    && /BWL-014/.test(text()) && /BWL-021/.test(text()));
}

console.log('\n[health]');
await go('#/health');
const rows = [...view.querySelectorAll('.dev')];
ok('lists devices', rows.length >= 24);
ok('offline device sorts first', rows[0].textContent.includes('BWL-014'), rows[0]?.textContent.slice(0, 40));
ok('fault device near top', rows.slice(0, 3).some(r => r.textContent.includes('BWL-002')));
ok('awaiting section present', text().includes('Awaiting deployment'));
ok('no-battery device says No battery', text().includes('No battery'));
ok('never-reported shows no count', text().includes('Never reported'));
{
  const row21 = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-021'));
  ok('missed-service device wears the badge', /Missed last service/.test(row21?.textContent || ''));
  ok('its last count is red', row21?.querySelector('.dev-count.is-offline') !== null);
  ok('missed-service sorts into the top three',
    [...view.querySelectorAll('.dev')].slice(0, 3).some(r => r.textContent.includes('BWL-021')));
  const row14 = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-014'));
  ok('offline device count is red too', row14?.querySelector('.dev-count.is-offline') !== null);
  const faultRow = [...view.querySelectorAll('.dev')].find(r => r.textContent.includes('BWL-002'));
  ok('a fault row never gains the red count', faultRow?.querySelector('.dev-count.is-offline') == null
    && faultRow?.querySelector('.dev-count.na') !== null);
}
await go('#/health?f=offline');
ok('offline filter includes the missed-service device',
  view.querySelectorAll('.dev').length === 2
  && ['BWL-014', 'BWL-021'].every(id => text().includes(id)));

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

  const mealSel = [...view.querySelectorAll('select')].find(sel =>
    [...sel.options].some(o => o.value === 'Dinner'));
  mealSel.value = 'Dinner';
  mealSel.dispatchEvent(new window.Event('change'));
  await new Promise(r => setTimeout(r, 120));
  ok('day survives a meal switch', /day=3/.test(location.hash), location.hash);
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
  ok('template draft banner shown', /From the weekly template/.test(text()));
  ok('and it does not claim a carry-over', !/Carried over from/.test(text()));
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


console.log(`\n${fails.length ? `FAILED (${fails.length}): ${fails.join(' | ')}` : 'ALL PASS'}`);
process.exit(fails.length ? 1 : 0);
