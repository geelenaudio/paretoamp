// Minimal hash router. Routes are registered with `.add(path, loader)` where
// `loader` is an async function returning a Preact component. Pages are
// lazy-loaded via dynamic import() so the initial page-load only fetches
// index.html, app.js, app.css, and htm-preact; subsequent navigation pulls in
// the page JS on demand.
//
// Intentionally tiny: no nested routes, no params. If we grow a need for those
// we can vendor preact-iso, but today we have seven flat routes.

const listeners = new Set();
let currentPath = null;

function normalize(path) {
  if (!path) return "/";
  if (!path.startsWith("/")) path = "/" + path;
  // Strip trailing slash except for root.
  if (path.length > 1 && path.endsWith("/")) path = path.slice(0, -1);
  return path;
}

export function currentRoute() {
  const hash = window.location.hash || "#/";
  return normalize(hash.slice(1) || "/");
}

export function navigate(path) {
  const next = normalize(path);
  if (next === currentPath) return;
  window.location.hash = "#" + next;
}

export function onRouteChange(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function startRouter() {
  const emit = () => {
    const next = currentRoute();
    if (next === currentPath) return;
    currentPath = next;
    listeners.forEach((fn) => fn(next));
  };
  window.addEventListener("hashchange", emit);
  // Kick initial dispatch on next microtask so subscribers have attached.
  queueMicrotask(emit);
}
