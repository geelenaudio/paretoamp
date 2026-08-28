// SPA entry point. Boots the router, mounts the shell, and lazy-loads the
// per-route page module on navigation. The page module default-exports a
// Preact component that receives `{ api }` as props; it may render anything.

import { html, render, useEffect, useState } from "/vendor/htm-preact.mjs";
import { startRouter, onRouteChange, currentRoute, navigate } from "/router.js";
import { api } from "/api.js";
import {
  IcoHome, IcoMode, IcoKnob, IcoNote, IcoFlask, IcoWifi,
  IcoRefresh, IcoLogs, IcoInfo, IcoSun, IcoMoon,
} from "/icons.js";

// Static registry: (hash path) -> (label, icon, loader). Loader is a callable
// returning Promise<{ default: Component }>. Each page lives in its own file so
// the SPA ships a small shell and streams per-page chunks as the user clicks.
const ROUTES = [
  { path: "/",         label: "Overview",  icon: IcoHome,    loader: () => import("/pages/overview.js") },
  { path: "/mode",     label: "Mode",      icon: IcoMode,    loader: () => import("/pages/mode.js") },
  { path: "/audio",    label: "Audio",     icon: IcoKnob,    loader: () => import("/pages/audio.js") },
  { path: "/dsp",      label: "DSP / EQ",  icon: IcoNote,    loader: () => import("/pages/dsp.js") },
  { path: "/test",     label: "Testing",   icon: IcoFlask,   loader: () => import("/pages/test.js") },
  { path: "/wifi",     label: "Wi-Fi",     icon: IcoWifi,    loader: () => import("/pages/wifi.js") },
  { path: "/reset",    label: "Reset",     icon: IcoRefresh, loader: () => import("/pages/reset.js") },
  { path: "/logs",     label: "Logs",      icon: IcoLogs,    loader: () => import("/pages/logs.js") },
  { path: "/about",    label: "About",     icon: IcoInfo,    loader: () => import("/pages/about.js") },
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

// Header theme toggle. Source of truth lives on `<html data-theme>` (set by the
// pre-paint script in index.html). Click flips it, persists to localStorage so
// the choice survives reloads, and re-renders the button's own icon.
function ThemeToggle() {
  const [theme, setThemeState] = useState(
    () => document.documentElement.dataset.theme || "dark",
  );
  useEffect(() => {
    document.documentElement.dataset.theme = theme;
    try { localStorage.setItem("ga-theme", theme); } catch { /* ignore */ }
  }, [theme]);
  const next = theme === "dark" ? "light" : "dark";
  const title = `Switch to ${next} theme`;
  return html`<button
    type="button"
    class="icon-btn"
    title=${title}
    aria-label=${title}
    onClick=${() => setThemeState(next)}>
    ${theme === "dark" ? html`<${IcoSun} />` : html`<${IcoMoon} />`}
  </button>`;
}

// Render nav items as real <a href> anchors so the native affordances
// (tooltip on hover, right-click → "Open in new tab", cmd/ctrl-click,
// screen-reader semantics) all keep working. Plain left-clicks without
// modifiers are intercepted and routed client-side via pushState; we
// only swallow the default when we're sure we're handling it ourselves.
function Nav({ active }) {
  function onNavClick(ev, path) {
    if (ev.defaultPrevented) return;
    if (ev.button !== 0) return;
    if (ev.metaKey || ev.ctrlKey || ev.shiftKey || ev.altKey) return;
    ev.preventDefault();
    navigate(path);
  }
  return html`<nav class="shell__nav">
    ${ROUTES.map(
      (r) => html`<a
        key=${r.path}
        href=${r.path}
        class=${"nav-item" + (active === r.path ? " nav-item--active" : "")}
        onClick=${(e) => onNavClick(e, r.path)}>
        <span class="nav-item__icon"><${r.icon} /></span>
        <span class="nav-item__label">${r.label}</span>
      </a>`,
    )}
  </nav>`;
}

function App() {
  const [route, setRoute] = useState(currentRoute());
  const [Page, setPage] = useState(null);
  const [loadError, setLoadError] = useState(null);
  const [apOnly, setApOnly] = useState(false);
  const [apSsid, setApSsid] = useState("Geelen-Audio");

  useEffect(() => onRouteChange(setRoute), []);
  useEffect(() => {
    startRouter();
  }, []);
  useEffect(() => {
    // Keep one EventSource for the whole tab so volume (and the rest of
    // state) stays current on every route, including pages that do not
    // subscribe themselves.
    return api.subscribeState(() => {});
  }, []);
  useEffect(() => {
    let cancelled = false;
    api.getWifiState().then(
      (s) => {
        if (cancelled || !s) return;
        setApOnly(!!(s.ap_only || (s.ap_running && !s.sta_connected)));
        if (s.ap_ssid) setApSsid(s.ap_ssid);
      },
      () => {},
    );
    return () => { cancelled = true; };
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
        <div class="shell__brand-mark">
          <img src="/logo/geelen_audio.svg" alt="" width="72" height="42" />
        </div>
        <span class="shell__brand-text">Geelen Audio</span>
      </div>
      <header class="shell__header">
        <div class="shell__header-title">Pareto Firmware</div>
        <div class="shell__header-actions">
          <div class="shell__header-meta" id="hdr-meta"></div>
          <${ThemeToggle} />
        </div>
      </header>
      <${Nav} active=${route} />
      <main class="shell__main">
        ${apOnly ? html`<div class="banner banner--warn" style="margin-bottom:var(--sp-4)">
          <strong>Setup hotspot.</strong>
          This device is not on your home Wi-Fi. Use
          <a href="/wifi" onClick=${(e) => { e.preventDefault(); navigate("/wifi"); }}>Wi-Fi</a>
          or open <span style="font-family:var(--font-mono)">http://192.168.4.1/</span>
          (${apSsid}). Snapcast needs home Wi-Fi.
        </div>` : ""}
        ${body}
      </main>
      <${Toasts} />
    </div>
  `;
}

render(html`<${App} />`, document.getElementById("app"));
