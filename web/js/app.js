// ====================================================================
//  Shell: connection gate, auth gate, router, polling.
//
//  Polling, not realtime. The firmware heartbeat is 20 s, so 32 devices
//  would push ~140k broadcast messages a day at a screen that is glanced
//  at every few minutes. Polling two small views is cheaper and cannot
//  fall silently out of sync.
// ====================================================================

import {
  getClient, readConfig, saveConfig, clearConfig, readAutoLogin, dashboardUrl,
  unwrap, describeError,
} from './supa.js';
import { h, mount, clear, reconcile, toast, copyText } from './ui.js';
import { fleetSummary, serviceState, fmtRelative, fmtClock } from './domain.js';
import { renderStock } from './views/stock.js';
import { renderHealth } from './views/health.js';
import { renderDevice, clearHistoryCache } from './views/device.js';
import { renderMenu } from './views/menu.js';
import { renderAssign } from './views/assign.js';
import { APP_VERSION } from './version.js';

// Sized against the server's own alarm, not picked round. `offline` fires at
// 40 s without a report, so worst-case time-to-notice is 40 s + one poll: 60 s
// at a 20 s poll, which meets "within a minute" with no margin at all and
// misses it entirely if a single request is slow. At 15 s it is 40-55 s --
// still inside the minute, with enough slack to absorb a slow round trip,
// without a 10 s poll's request volume. Polling stops while the tab is hidden.
const POLL_MS = 15_000;

const state = {
  devices: [],
  slots: [],
  loadedAt: null,
  error: null,
  loading: false,
};

const el = {
  setup: document.getElementById('setup'),
  login: document.getElementById('login'),
  connecting: document.getElementById('connecting'),
  app: document.getElementById('app'),
  view: document.getElementById('view'),
  chips: document.getElementById('fleet-chips'),
  meal: document.getElementById('meal-indicator'),
  freshness: document.getElementById('freshness'),
};

let client = null;
let pollTimer = null;
let tickTimer = null;
// A deliberate sign-out must not be caught by the re-authenticate handler,
// which exists for sessions that expire rather than ones you end.
let signingOut = false;

const ctx = {
  get client() { return client; },
  refresh: () => refresh(true),
  // Re-run the current view without a round trip. Needed because setting
  // location.hash to the value it already has fires no event, so a screen that
  // navigates to itself after saving would never redraw.
  rerender: () => render(true),
};

// --- gates -----------------------------------------------------------

function show(which) {
  el.setup.hidden = which !== 'setup';
  el.login.hidden = which !== 'login';
  el.connecting.hidden = which !== 'connecting';
  el.app.hidden = which !== 'app';
}

/**
 * Sign in with nobody typing anything.
 *
 * `anonymous` mints a genuine `authenticated` JWT, so every policy in
 * schema.sql keeps working untouched — this is a way of GETTING the
 * authenticated role, not of bypassing it. Nothing about the grants changes,
 * and `anon` stays write-only for the devices.
 */
async function autoSignIn(auto, { quiet = false } = {}) {
  const msg = document.getElementById('connecting-msg');
  const errBox = document.getElementById('connecting-error');

  // `quiet` is the mid-session path: the session hiccuped while the app is on
  // screen (token refresh raced by another tab, a network blip at renewal).
  // Swapping to the full-screen gate here was a literal blackout — and the
  // forced re-render on the way back wiped whatever was being typed, which is
  // how "the page flickers and I can't edit settings for 10-15 s" happened.
  // Recover invisibly; the gate appears only if sign-in actually FAILS.
  if (!quiet) {
    show('connecting');
    msg.textContent = 'Signing in…';
    errBox.hidden = true;
    document.getElementById('connecting-retry').hidden = true;
    document.getElementById('connecting-manual').hidden = true;
  }

  const { error } = auto.mode === 'anonymous'
    ? await client.auth.signInAnonymously()
    : await client.auth.signInWithPassword({ email: auto.email, password: auto.password });

  if (!error) {
    if (quiet) {
      const { data } = await client.auth.getUser();
      const u = data?.user;
      document.getElementById('menu-user').textContent =
        u?.email || (u?.is_anonymous ? 'Signed in anonymously' : 'Signed in');
      // Resume polling and refetch, but NEVER force a render from here: a
      // forced render redraws the editable screens, and this path can fire
      // while someone is halfway through a dish name.
      startPolling();
      refresh(false);
      return true;
    }
    await onSignedIn();
    return true;
  }

  show('connecting');
  msg.textContent = 'Could not sign in.';

  // Anonymous sign-in is off by default in Supabase, and the error it returns
  // names neither the setting nor the page it lives on. Say both, and link
  // straight to it — this is the one step between a fresh deploy and a working
  // dashboard, so it should take a click rather than a search.
  const isDisabled = auto.mode === 'anonymous'
    && /anonymous|disabled|not enabled/i.test(error.message || '');
  const providers = dashboardUrl('/auth/providers');

  errBox.replaceChildren();
  if (isDisabled) {
    errBox.append(
      h('div', {}, error.message),
      h('div', { style: 'margin-top:.4rem' },
        'Turn on ',
        h('b', {}, 'Allow anonymous sign-ins'),
        ' under User Signups, then press Try again.'),
      providers ? h('div', { style: 'margin-top:.4rem' },
        h('a', { href: providers, target: '_blank', rel: 'noopener' },
          'Open Authentication → Sign In / Providers ↗')) : null);
  } else {
    errBox.textContent = describeError(error);
  }
  errBox.hidden = false;
  document.getElementById('connecting-retry').hidden = false;
  document.getElementById('connecting-manual').hidden = false;
  return false;
}

/** Session gone: get another one the same way we got the first. Deduped —
 *  a failing poll and an auth event can both notice the loss at once. */
let reauthing = false;
async function reauthenticate() {
  if (reauthing) return;
  reauthing = true;
  try {
    stopPolling();
    const auto = readAutoLogin();
    if (!auto) { show('login'); return; }
    // Quiet whenever the app is already on screen; the gate is for cold boots.
    await autoSignIn(auto, { quiet: !el.app.hidden });
  } finally {
    reauthing = false;
  }
}

document.getElementById('connecting-retry').addEventListener('click', () => {
  const auto = readAutoLogin();
  if (auto) autoSignIn(auto); else show('login');
});
document.getElementById('connecting-manual').addEventListener('click', () => show('login'));

document.getElementById('setup-form').addEventListener('submit', e => {
  e.preventDefault();
  const f = new FormData(e.target);
  const err = document.getElementById('setup-error');
  try {
    saveConfig(String(f.get('url')), String(f.get('key')));
    location.reload();
  } catch (ex) {
    err.textContent = describeError(ex);
    err.hidden = false;
  }
});

document.getElementById('login-reconfigure').addEventListener('click', () => {
  clearConfig();
  location.reload();
});

document.getElementById('login-form').addEventListener('submit', async e => {
  e.preventDefault();
  const err = document.getElementById('login-error');
  err.hidden = true;
  const btn = e.target.querySelector('button[type=submit]');
  btn.disabled = true;
  const f = new FormData(e.target);
  const { error } = await client.auth.signInWithPassword({
    email: String(f.get('email')).trim(),
    password: String(f.get('password')),
  });
  btn.disabled = false;
  if (error) {
    err.textContent = error.message;
    err.hidden = false;
    return;
  }
  await onSignedIn();
});

// --- chrome ----------------------------------------------------------

document.getElementById('refresh-btn').addEventListener('click', () => refresh(true));

const THEME_KEY = 'bowlstack.theme';
function applyTheme() {
  const t = localStorage.getItem(THEME_KEY);
  if (t === 'light' || t === 'dark') document.documentElement.dataset.theme = t;
  else delete document.documentElement.dataset.theme;
}
document.getElementById('theme-btn').addEventListener('click', () => {
  const cur = localStorage.getItem(THEME_KEY);
  const next = cur === 'light' ? 'dark' : cur === 'dark' ? '' : 'light';
  if (next) localStorage.setItem(THEME_KEY, next); else localStorage.removeItem(THEME_KEY);
  applyTheme();
  toast(next ? `${next} theme` : 'Follows the device theme');
});
applyTheme();

// Rendered from the constant rather than written into the markup, so there is
// one place to bump and the header can never disagree with the diagnostics.
document.getElementById('app-version').textContent = `v${APP_VERSION}`;

document.querySelector('.menu-panel').addEventListener('click', async e => {
  const act = e.target.closest('button')?.dataset.act;
  if (!act) return;
  document.querySelector('.menu').removeAttribute('open');
  if (act === 'signout') {
    signingOut = true;
    await client.auth.signOut();
    stopPolling();
    show('login');
    signingOut = false;
  } else if (act === 'reconfigure') {
    if (client) await client.auth.signOut().catch(() => {});
    clearConfig();
    location.reload();
  } else if (act === 'update') {
    if ('serviceWorker' in navigator) {
      const regs = await navigator.serviceWorker.getRegistrations();
      await Promise.all(regs.map(r => r.unregister()));
    }
    location.reload();
  } else if (act === 'copy-diag') {
    copyText(fleetDiagnostics(), 'Fleet diagnostics copied');
  }
});

// Close the menu when anything outside it is touched.
document.addEventListener('click', e => {
  const m = document.querySelector('.menu');
  if (m && m.open && !m.contains(e.target)) m.removeAttribute('open');
});

// --- data ------------------------------------------------------------

async function refresh(manual = false) {
  if (state.loading) return;
  if (manual) clearHistoryCache();
  state.loading = true;

  // No dimming on a normal poll. The view is patched in place, so there is
  // nothing to cover up, and a twice-a-minute flicker on a screen someone
  // watches through a service is worse than no feedback at all. Only a fetch
  // slow enough to be worth mentioning gets the treatment, and it is cancelled
  // below if the data arrives first.
  const slowHint = setTimeout(() => el.view.classList.add('is-refetching'), 1200);

  try {
    const [devices, slots] = await Promise.all([
      client.from('device_overview').select('*')
        .order('location', { nullsFirst: false }).order('food_slot', { nullsFirst: false })
        .then(unwrap),
      client.from('slot_overview').select('*')
        .order('location').order('food_slot')
        .then(unwrap),
    ]);
    state.devices = devices || [];
    state.slots = slots || [];
    state.loadedAt = Date.now();
    state.error = null;
  } catch (err) {
    state.error = describeError(err);
    // A session that has gone away reads as an auth error, not empty data.
    const { data } = await client.auth.getSession();
    if (!data.session) { await reauthenticate(); return; }
    if (manual) toast(state.error, true);
  } finally {
    clearTimeout(slowHint);
    state.loading = false;
    el.view.classList.remove('is-refetching');
    renderChrome();
    render(manual);
  }
}

function startPolling() {
  stopPolling();
  pollTimer = setInterval(() => { if (!document.hidden) refresh(); }, POLL_MS);
  tickTimer = setInterval(renderFreshness, 5000);
}

function stopPolling() {
  clearInterval(pollTimer); clearInterval(tickTimer);
  pollTimer = tickTimer = null;
}

document.addEventListener('visibilitychange', () => {
  if (!document.hidden && client && pollTimer) refresh();
});

// --- chrome rendering -------------------------------------------------

function renderChrome() {
  const s = fleetSummary(state.devices);
  const { inService, meal } = serviceState(state.devices);

  el.meal.textContent = inService
    ? `${meal || 'Service'} · live`
    : 'Outside service hours';
  el.meal.title = inService
    ? 'Devices are powered and reporting.'
    : 'Devices are powered only during meal service. Values shown are last-known.';

  const chips = [];
  const chip = (label, n, tone, filter) => h('button', {
    class: `chip${n ? ` is-${tone}` : ''}`,
    onclick: () => { location.hash = filter ? `#/health?f=${filter}` : '#/health'; },
  }, h('b', {}, String(n)), label);

  chips.push(chip('offline', s.offline, 'critical', 'offline'));
  chips.push(chip('faults', s.fault, 'critical', 'fault'));
  chips.push(chip('degraded', s.degraded, 'warning', 'fault'));
  chips.push(chip('battery', s.batteryWarn, 'warning', 'battery'));
  if (s.awaiting) chips.push(chip('not deployed', s.awaiting, 'good', null));
  chips.push(h('span', {
    class: 'chip', style: 'cursor:default',
    title: 'Deployed devices not currently flagged offline or missed-service. '
      + 'Outside meals a healthy dark device still counts — dark is normal.',
  }, h('b', {}, `${s.reporting}/${s.total - s.awaiting}`), 'reporting'));
  reconcile(el.chips, chips);

  renderFreshness();
}

function renderFreshness() {
  const bits = [];
  if (state.error) bits.push(h('span', { class: 'err' }, `⚠ ${state.error}`));
  else if (state.loadedAt) bits.push(h('span', {}, `Updated ${fmtRelative(new Date(state.loadedAt).toISOString())}`));
  const oldest = state.devices
    .filter(d => d.reported && d.updated_at)
    .map(d => d.updated_at).sort()[0];
  if (oldest) bits.push(h('span', { class: 'dim' },
    `· oldest device report ${fmtRelative(oldest)}`));
  reconcile(el.freshness, bits);
}

// --- routing ----------------------------------------------------------

function parseRoute() {
  const raw = location.hash.replace(/^#/, '') || '/stock';
  const [path, query = ''] = raw.split('?');
  const parts = path.split('/').filter(Boolean);
  const params = new URLSearchParams(query);
  const route = parts[0] || 'stock';
  if (route === 'device' && parts[1]) params.set('id', parts[1]);
  return { route, params };
}

const VIEWS = {
  stock: renderStock,
  health: renderHealth,
  device: renderDevice,
  menu: renderMenu,
  assign: renderAssign,
};

// Screens that hold unsaved user input. A background poll must not redraw them
// — someone half-way through typing a dish name would lose it every 20 seconds.
const EDITABLE = new Set(['menu', 'assign']);

let lastRoute = null;

function render(forced = true) {
  const { route, params } = parseRoute();
  const key = location.hash;
  const routeChanged = key !== lastRoute;

  if (!forced && !routeChanged && EDITABLE.has(route)) return;

  const fn = VIEWS[route] || renderStock;
  const tab = route === 'device' ? 'health' : route;
  for (const a of document.querySelectorAll('.tabs a')) {
    a.classList.toggle('active', a.dataset.tab === tab);
  }
  try {
    // A poll patches the live DOM; a navigation replaces it. Patching is what
    // stops the screen blinking every refresh — and it keeps scroll position,
    // the caret in the search box, and an open history table where they were.
    // Across a route change there is nothing to preserve and morphing two
    // unrelated trees would just be slower.
    if (routeChanged) mount(el.view, fn(state, params, ctx));
    else reconcile(el.view, fn(state, params, ctx));
  } catch (err) {
    console.error(err);
    clear(el.view);
    el.view.append(h('div', { class: 'banner is-critical' }, `Render failed: ${err.message}`));
  }
  // Only on a genuine navigation — otherwise every poll would yank a scrolled
  // health list back to the top.
  if (routeChanged) window.scrollTo(0, 0);
  lastRoute = key;
}

window.addEventListener('hashchange', () => render(true));

// The SVG figures are sized in pixels at render time, so a rotated tablet needs
// a re-render rather than a reflow.
let resizeTimer = null;
let lastWidth = window.innerWidth;
window.addEventListener('resize', () => {
  if (Math.abs(window.innerWidth - lastWidth) < 24) return;
  lastWidth = window.innerWidth;
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => { if (!el.app.hidden) render(); }, 200);
});

// --- fleet diagnostics -------------------------------------------------

function fleetDiagnostics() {
  const { inService, meal, tz } = serviceState(state.devices);
  return JSON.stringify({
    captured_at: new Date().toISOString(),
    dashboard_version: APP_VERSION,
    site: { timezone: tz, in_service: inService, meal, local_time: fmtClock(new Date().toISOString(), tz) },
    summary: fleetSummary(state.devices),
    slots: state.slots,
    devices: state.devices,
  }, null, 2);
}

// --- boot --------------------------------------------------------------

async function onSignedIn() {
  const { data } = await client.auth.getUser();
  const u = data?.user;
  document.getElementById('menu-user').textContent =
    u?.email || (u?.is_anonymous ? 'Signed in anonymously' : 'Signed in');
  show('app');
  if (!location.hash) location.hash = '#/stock';
  await refresh(true);
  startPolling();
}

async function boot() {
  if (!readConfig()) { show('setup'); return; }
  client = getClient();
  if (!client) { show('setup'); return; }

  // With auto-login there is no account to sign out of, and offering it would
  // only bounce straight back through autoSignIn().
  document.querySelector('[data-act="signout"]').hidden = !!readAutoLogin();

  client.auth.onAuthStateChange((event) => {
    if (event === 'SIGNED_OUT' && !signingOut) reauthenticate();
  });

  const { data, error } = await client.auth.getSession();
  if (error) {
    const e = document.getElementById('login-error');
    e.textContent = describeError(error);
    e.hidden = false;
  }
  if (data?.session) { await onSignedIn(); return; }

  const auto = readAutoLogin();
  if (auto) await autoSignIn(auto);
  else show('login');
}

boot();

if ('serviceWorker' in navigator && location.protocol === 'https:') {
  navigator.serviceWorker.register('sw.js').catch(() => { /* offline shell is optional */ });
}
