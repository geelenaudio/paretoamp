// Minimal History-API router. Routes are flat paths (`/`, `/mode`, `/dsp` …)
// resolved against `window.location.pathname`, and `navigate()` uses
// `pushState` so the URL bar stays clean — no `#/` hash prefix.
//
// For this to survive a page reload or a bookmark, the firmware's static
// handler falls back to `index.html` when the request path has no file
// extension and isn't an `/api/*` URL (see idf_http_server.cpp's
// handle_static). The SPA then re-resolves the route once the JS boots,
// so deep-linking to `http://<device>/dsp` works.
//
// Intentionally tiny: no nested routes, no params. If we grow a need for
// those we can vendor preact-iso, but today we have seven flat routes.

const listeners = new Set();
let currentPath = null;

function normalize(path) {
  if (!path) return "/";
  if (!path.startsWith("/")) path = "/" + path;
  // Strip trailing slash except for root.
  if (path.length > 1 && path.endsWith("/")) path = path.slice(0, -1);
  // Drop any query string — routes ignore it.
  const q = path.indexOf("?");
  if (q >= 0) path = path.slice(0, q);
  return path;
}

export function currentRoute() {
  // History API: pathname is authoritative. Legacy `#/foo` deep-links
  // (from older builds or bookmarks) rewrite themselves to the clean URL
  // on first load — see startRouter() below.
  return normalize(window.location.pathname || "/");
}

export function navigate(path) {
  const next = normalize(path);
  if (next === currentPath) return;
  // pushState does NOT fire popstate — dispatch an explicit event so the
  // router's subscribers re-render. We reuse popstate so there's a single
  // listening path.
  window.history.pushState({}, "", next);
  window.dispatchEvent(new PopStateEvent("popstate"));
}

export function onRouteChange(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function startRouter() {
  // Migrate any surviving `#/foo` hash link to a clean URL so bookmarks
  // from the old hash build don't look weird after the user upgrades.
  if (window.location.hash && window.location.hash.startsWith("#/")) {
    const migrated = normalize(window.location.hash.slice(1));
    window.history.replaceState({}, "", migrated);
  }

  const emit = () => {
    const next = currentRoute();
    if (next === currentPath) return;
    currentPath = next;
    listeners.forEach((fn) => fn(next));
  };
  window.addEventListener("popstate", emit);
  // Kick initial dispatch on next microtask so subscribers have attached.
  queueMicrotask(emit);
}
