/**
 * Service worker for the AC Control PWA.
 *
 * Caches the static app shell (HTML/CSS/JS/icons) so the UI opens instantly and
 * works offline. Device data under /api/* is never cached — it must always be
 * live — so those requests fall through to the network untouched.
 *
 * Bump CACHE when the shell changes to evict the old copy on next launch.
 */
const CACHE = "ac-control-v1";
const SHELL = [
  "./",
  "./index.html",
  "./style.css",
  "./script.js",
  "./manifest.json",
  "./icons/icon-192.png",
  "./icons/icon-512.png",
  "./icons/maskable-512.png",
  "./icons/apple-touch.png",
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE)
      .then((cache) => cache.addAll(SHELL))
      .then(() => self.skipWaiting())
      .catch(() => {}),
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

self.addEventListener("fetch", (event) => {
  const req = event.request;
  const url = new URL(req.url);

  // Live device state: let the app's own fetch (with its timeout) handle it.
  if (req.method !== "GET" || url.pathname.startsWith("/api/")) return;

  // Stale-while-revalidate for the app shell: serve from cache immediately,
  // refresh the cached copy in the background when the device is reachable.
  event.respondWith(
    caches.open(CACHE).then(async (cache) => {
      const cached = await cache.match(req, { ignoreSearch: true });
      const network = fetch(req)
        .then((res) => {
          if (res && res.ok && url.origin === self.location.origin) cache.put(req, res.clone());
          return res;
        })
        .catch(() => cached);
      return cached || network;
    }),
  );
});
