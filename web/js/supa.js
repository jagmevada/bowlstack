// Supabase connection: where the credentials come from, and the one client.

const LS_KEY = 'bowlstack.connection';

/**
 * Precedence: whatever this browser was told explicitly wins over the value
 * baked into the deployment, so a tester can point a hosted build at their own
 * project without a rebuild.
 */
export function readConfig() {
  try {
    const stored = JSON.parse(localStorage.getItem(LS_KEY) || 'null');
    if (stored && stored.url && stored.anonKey) return stored;
  } catch { /* corrupt entry — fall through to the deployment default */ }

  const baked = window.BOWLSTACK_CONFIG || {};
  if (baked.url && baked.anonKey) return { url: baked.url, anonKey: baked.anonKey };
  return null;
}

export function saveConfig(url, anonKey) {
  localStorage.setItem(LS_KEY, JSON.stringify({
    url: url.trim().replace(/\/+$/, ''),
    anonKey: anonKey.trim(),
  }));
}

export function clearConfig() {
  localStorage.removeItem(LS_KEY);
}

/**
 * How to authenticate without asking anyone.
 *
 * Reads require the `authenticated` role — `anon` is write-only so that a
 * device can never read another installation's telemetry. Skipping sign-in
 * entirely therefore does not produce a public dashboard; it produces an empty
 * one, which is indistinguishable on screen from a dead fleet.
 *
 * Deployment-level only, never localStorage: it is a property of the
 * installation, not of the browser looking at it.
 */
export function readAutoLogin() {
  const a = (window.BOWLSTACK_CONFIG || {}).autoLogin;
  if (!a) return null;
  if (a === 'anonymous') return { mode: 'anonymous' };
  if (a.email && a.password) return { mode: 'password', email: a.email, password: a.password };
  return null;
}

let client = null;

export function getClient() {
  if (client) return client;
  const cfg = readConfig();
  if (!cfg) return null;
  client = window.supabase.createClient(cfg.url, cfg.anonKey, {
    auth: { persistSession: true, autoRefreshToken: true },
    // The dashboard polls; it does not subscribe. 32 devices updating every
    // 60 s would be ~46k broadcast messages a day for a screen that is glanced
    // at every few minutes (docs/FRONTEND_HANDOFF.md §7).
    realtime: { params: { eventsPerSecond: 1 } },
  });
  return client;
}

/** Unwrap a PostgREST response, turning the error into something throwable. */
export function unwrap({ data, error }) {
  if (error) {
    const e = new Error(error.message || 'Request failed');
    e.details = error.details;
    e.hint = error.hint;
    e.code = error.code;
    throw e;
  }
  return data;
}

/**
 * The Supabase dashboard page for this project, or null if the URL is not a
 * hosted supabase.co project (self-hosted has no such console).
 */
export function dashboardUrl(path = '') {
  const cfg = readConfig();
  const ref = cfg && /^https:\/\/([a-z0-9]+)\.supabase\.co$/i.exec(cfg.url.replace(/\/+$/, ''));
  return ref ? `https://supabase.com/dashboard/project/${ref[1]}${path}` : null;
}

export function describeError(err) {
  if (!err) return 'Unknown error';
  const parts = [err.message];
  if (err.details) parts.push(err.details);
  if (err.hint) parts.push(`Hint: ${err.hint}`);
  if (err.code) parts.push(`(${err.code})`);
  return parts.filter(Boolean).join(' — ');
}
