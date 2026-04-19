// SPA entry point. Boots the router, mounts the shell, and lazy-loads the
// per-route page module on navigation. The page module default-exports a
// Preact component that receives `{ api }` as props; it may render anything.

import { html, render, useEffect, useState } from "/vendor/htm-preact.mjs";
import { startRouter, onRouteChange, currentRoute, navigate } from "/router.js";
import { api } from "/api.js";

// Static registry: (hash path) -> (label, icon, loader). Loader is a callable
// returning Promise<{ default: Component }>. Each page lives in its own file so
// the SPA ships a small shell and streams per-page chunks as the user clicks.
const ROUTES = [
  { path: "/",         label: "Overview",  icon: "⌂", loader: () => import("/pages/overview.js") },
  { path: "/mode",     label: "Mode",      icon: "◉", loader: () => import("/pages/mode.js") },
  { path: "/audio",    label: "Audio",     icon: "▥", loader: () => import("/pages/audio.js") },
  { path: "/dsp",      label: "DSP / EQ",  icon: "≋", loader: () => import("/pages/dsp.js") },
  { path: "/reset",    label: "Reset",     icon: "⟲", loader: () => import("/pages/reset.js") },
  { path: "/logs",     label: "Logs",      icon: "☰", loader: () => import("/pages/logs.js") },
  { path: "/about",    label: "About",     icon: "ⓘ", loader: () => import("/pages/about.js") },
];

// Toast bus: page code imports `toast()` from here and gets a quick transient
// notification. Implemented as a global so every page can poke it without
// threading callbacks.
const toastSubs = new Set();
export function toast(message, kind = "ok", ttl_ms = 2600) {
  const entry = { id: Math.random().toString(36).slice(2), message, kind };
  toastSubs.forEach((fn) => fn({ type: "add", entry }));
  setTimeout(() => {
    toastSubs.forEach((fn) => fn({ type: "remove", id: entry.id }));
  }, ttl_ms);
}
window.__geelenToast = toast; // handy for browser-console debugging

function Toasts() {
  const [items, setItems] = useState([]);
  useEffect(() => {
    const sub = (ev) => {
      setItems((prev) => {
        if (ev.type === "add") return [...prev, ev.entry];
        if (ev.type === "remove") return prev.filter((x) => x.id !== ev.id);
        return prev;
      });
    };
    toastSubs.add(sub);
    return () => toastSubs.delete(sub);
  }, []);
  return html`<div class="toasts">${items.map(
    (t) => html`<div key=${t.id} class="toast toast--${t.kind}">${t.message}</div>`,
  )}</div>`;
}

function Nav({ active }) {
  return html`<nav class="shell__nav">
    ${ROUTES.map(
      (r) => html`<div
        key=${r.path}
        class=${"nav-item" + (active === r.path ? " nav-item--active" : "")}
        onClick=${() => navigate(r.path)}
        role="link"
        tabindex="0">
        <span class="nav-item__icon">${r.icon}</span>
        <span class="nav-item__label">${r.label}</span>
      </div>`,
    )}
  </nav>`;
}

function pageTitle(path) {
  return (ROUTES.find((r) => r.path === path) || {}).label || "Not found";
}

function App() {
  const [route, setRoute] = useState(currentRoute());
  const [Page, setPage] = useState(null);
  const [loadError, setLoadError] = useState(null);

  useEffect(() => onRouteChange(setRoute), []);
  useEffect(() => {
    startRouter();
  }, []);

  useEffect(() => {
    setLoadError(null);
    setPage(null);
    const entry = ROUTES.find((r) => r.path === route);
    if (!entry) { setLoadError("No such page"); return; }
    let cancelled = false;
    entry
      .loader()
      .then((m) => { if (!cancelled) setPage(() => m.default); })
      .catch((err) => { if (!cancelled) setLoadError(String(err)); });
    return () => { cancelled = true; };
  }, [route]);

  const body = loadError
    ? html`<div class="card" style="border-color: var(--err)">
        <h2 class="page__title">Could not load page</h2>
        <p class="page__subtitle">${loadError}</p>
      </div>`
    : Page
      ? html`<${Page} api=${api} toast=${toast} navigate=${navigate} />`
      : html`<div class="stub">Loading…</div>`;

  return html`
    <div class="shell">
      <div class="shell__brand">
        <div class="shell__brand-mark">G</div>
        <span class="shell__brand-text">Geelen Audio</span>
      </div>
      <header class="shell__header">
        <div class="shell__header-title">${pageTitle(route)}</div>
        <div class="shell__header-meta" id="hdr-meta"></div>
      </header>
      <${Nav} active=${route} />
      <main class="shell__main">${body}</main>
      <${Toasts} />
    </div>
  `;
}

render(html`<${App} />`, document.getElementById("app"));
