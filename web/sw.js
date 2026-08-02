// Offline shell for a kitchen tablet on unreliable WiFi.
//
// NETWORK FIRST, deliberately. A cache-first worker is the classic way to
// ship a fix during a trial and have nobody see it — and during a trial the
// code changes daily. The cache exists so the app opens at all when the
// network drops, not to make it faster.
//
// Supabase requests are never cached: a stale bowl count is worse than none,
// and every screen already says how old its data is.

const CACHE = 'bowlstack-shell-v1.9';
const SHELL = [
  './',
  'index.html',
  'app.css',
  'config.js',
  'icon.svg',
  'manifest.webmanifest',
  'vendor/supabase.js',
  'js/app.js',
  'js/supa.js',
  'js/domain.js',
  'js/version.js',
  'js/ui.js',
  'js/chart.js',
  'js/views/stock.js',
  'js/views/health.js',
  'js/views/device.js',
  'js/views/menu.js',
  'js/views/assign.js',
];

self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE)
      .then(c => c.addAll(SHELL))
      .catch(() => { /* a missing entry must not block activation */ })
      .then(() => self.skipWaiting()));
});

self.addEventListener('activate', event => {
  event.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k)));
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', event => {
  const req = event.request;
  if (req.method !== 'GET') return;

  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;   // Supabase and anything else: straight through

  event.respondWith((async () => {
    try {
      const fresh = await fetch(req);
      if (fresh && fresh.ok) {
        const cache = await caches.open(CACHE);
        cache.put(req, fresh.clone());
      }
      return fresh;
    } catch (err) {
      const cached = await caches.match(req, { ignoreSearch: true });
      if (cached) return cached;
      if (req.mode === 'navigate') {
        const shell = await caches.match('index.html');
        if (shell) return shell;
      }
      throw err;
    }
  })());
});
