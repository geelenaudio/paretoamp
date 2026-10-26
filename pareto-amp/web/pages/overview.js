// Overview page: a live status dashboard.
//
// Data model:
//   - /api/state/stream : push-based configuration snapshot (device name,
//                   mode, volume, Snapcast host, …). The SPA subscribes once;
//                   the firmware emits a fresh snapshot whenever ConfigManager's
//                   stateVersion advances (debounced ~250 ms). No polling.
//   - /api/system/summary : compact runtime metrics (uptime, heap, PSRAM,
//                   WiFi RSSI, CPU). Polled every POLL_MS while the page is
//                   visible. Automatic throttling-to-zero on tab hide keeps
//                   the device's HTTP load minimal. Full `/api/system/metrics`
//                   (includes audio meters) is reserved for the test page.
//
// The metrics poll uses `setTimeout` in a chain (not `setInterval`) so a
// stalled fetch can't pile up requests; if the previous request is still in
// flight we skip the tick.

import { html, useEffect, useState, useRef } from "/vendor/htm-preact.mjs";

// 3 s is "live enough" for heap / RSSI / CPU while still costing the device
// ~20 % of what the original 1 Hz poll did. The page only mounts when the
// user is on /, so nothing pulls /api/system while they're on other routes.
const POLL_MS = 3000;

// Map a 0..100 value to one of the bar colour ramps so bars turn amber
// before red. Accepts an optional `invert` flag for metrics where low is
// bad (e.g. free heap, RSSI strength) — callers pre-normalise into 0..100.
function levelFor(pct, invert = false) {
  const v = invert ? 100 - pct : pct;
  if (v >= 85) return "err";
  if (v >= 50) return "warn";
  return "ok";
}

function fmtUptime(s) {
  if (s == null) return "—";
  s = Math.floor(s);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const ss = s % 60;
  if (d) return `${d}d ${h}h ${m}m`;
  if (h) return `${h}h ${m}m ${ss}s`;
  if (m) return `${m}m ${ss}s`;
  return `${ss}s`;
}

function fmtBytes(n) {
  if (n == null) return "—";
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(0)} KiB`;
  return `${(n / 1024 / 1024).toFixed(1)} MiB`;
}

// RSSI -> percentage. Standard mapping: -30 dBm is "full strength", -90 is
// "barely connected". Clamp outside the window.
function rssiToPct(rssi) {
  if (rssi == null) return 0;
  const pct = ((rssi + 90) / 60) * 100;
  return Math.max(0, Math.min(100, Math.round(pct)));
}

function modeLabel(mode) {
  if (mode === "bluetooth") return "Bluetooth A2DP";
  if (mode === "snapclient") return "Snapclient";
  if (mode === "dlna") return "DLNA renderer";
  if (mode === "test_tone") return "Test bench";
  if (mode === "usb") return "USB audio";
  return mode || "—";
}

function ledPhaseLabel(phase) {
  if (phase === "boot") return "Waiting for Wi-Fi";
  if (phase === "wifi") return "Wi-Fi connected";
  if (phase === "pairing") return "Bluetooth pairing";
  if (phase === "user") return "User colour";
  return phase || "—";
}

export default function Overview({ api, navigate, toast }) {
  const [state, setState] = useState(null);
  const [sys, setSys] = useState(null);
  const [err, setErr] = useState(null);
  const inflight = useRef(false);
  const [ledColor, setLedColor] = useState("#ffffff");
  const [ledBri, setLedBri] = useState(0);
  const ledSeeded = useRef(false);
  const [nameDraft, setNameDraft] = useState("");
  const [nameBusy, setNameBusy] = useState(false);
  const nameSeeded = useRef(false);
  const [pairLeftMs, setPairLeftMs] = useState(0);
  const [pairBusy, setPairBusy] = useState(false);

  // Push-based config subscription. The first frame arrives within one
  // RTT of mount (the broadcaster sends an initial snapshot before
  // detaching the socket); subsequent frames arrive only when a mutation
  // bumps stateVersion.
  useEffect(() => {
    const stop = api.subscribeState((s) => {
      setState(s);
      if (!ledSeeded.current && s.led) {
        ledSeeded.current = true;
        if (s.led.color) setLedColor(s.led.color);
        if (typeof s.led.brightness === "number") setLedBri(s.led.brightness);
      }
      if (!nameSeeded.current && s.device_name) {
        nameSeeded.current = true;
        setNameDraft(s.device_name);
      }
    });
    return stop;
  }, []);

  // 1 Hz metrics poll.
  useEffect(() => {
    let cancelled = false;
    let timer = null;

    async function tick() {
      if (cancelled) return;
      if (document.visibilityState !== "visible") {
        timer = setTimeout(tick, POLL_MS);
        return;
      }
      if (inflight.current) {
        timer = setTimeout(tick, POLL_MS);
        return;
      }
      inflight.current = true;
      try {
        const data = await api.getSystemSummary();
        if (!cancelled && !data._unimplemented) setSys(data);
      } catch {
        // Swallow transient errors; next tick will retry.
      } finally {
        inflight.current = false;
      }
      timer = setTimeout(tick, POLL_MS);
    }

    tick();
    return () => { cancelled = true; if (timer) clearTimeout(timer); };
  }, []);

  const bt = (state && state.bluetooth) || {};
  useEffect(() => {
    const base = bt.window_ms_left || 0;
    if (!bt.discoverable || base <= 0) {
      setPairLeftMs(0);
      return;
    }
    const start = Date.now();
    setPairLeftMs(base);
    const t = setInterval(() => {
      setPairLeftMs(Math.max(0, base - (Date.now() - start)));
    }, 500);
    return () => clearInterval(t);
  }, [bt.discoverable, bt.window_ms_left, bt.connected]);

  if (err) return html`<div class="card">${err}</div>`;
  if (!state) return html`<div class="stub">Loading device state…</div>`;

  const mode = state.mode || (state.source === 0 ? "bluetooth" : "snapclient");

  // Derived metrics.
  const heapPct = sys && sys.free_heap != null && sys.min_free_heap != null
    ? Math.max(0, Math.min(100, Math.round(
        // Rough "heap pressure": how close current free is to the lowest we've ever seen.
        // If current ≈ min, pressure is 100%. If free is much higher, pressure is small.
        100 - ((sys.free_heap - sys.min_free_heap) / Math.max(1, sys.free_heap) * 100)
      )))
    : null;
  const rssiPct = sys && sys.wifi ? rssiToPct(sys.wifi.rssi) : 0;
  const c0 = sys && sys.cpu ? sys.cpu.core0_pct : 0;
  const c1 = sys && sys.cpu ? sys.cpu.core1_pct : 0;

  const nameDirty = nameDraft !== (state.device_name || "");

  async function pairNew() {
    setPairBusy(true);
    try {
      await api.setBluetooth({ discoverable: true });
      toast && toast("Discoverable for 3 minutes", "ok");
    } catch (e) {
      toast && toast(String(e), "err");
    } finally {
      setPairBusy(false);
    }
  }

  async function saveName() {
    const n = (nameDraft || "").trim();
    if (!n) return;
    setNameBusy(true);
    try {
      await api.setDeviceName(n);
      toast && toast("Device renamed", "ok");
    } catch (e) {
      toast && toast(String(e), "err");
    } finally {
      setNameBusy(false);
    }
  }

  return html`
    <h1 class="page__title">Overview</h1>
    <p class="page__subtitle">Live status for ${state.device_name || "this device"}.</p>

    <div class="grid">
      <div class="card card--metric">
        <div class="card__title">Device</div>
        <div class="field" style="margin-top:6px">
          <input
            class="input"
            type="text"
            maxlength="31"
            value=${nameDraft}
            placeholder="Geelen Audio"
            aria-label="Device name"
            onInput=${(e) => setNameDraft(e.target.value)}
            onKeyDown=${(e) => { if (e.key === "Enter" && nameDirty) saveName(); }}
            disabled=${nameBusy}
          />
        </div>
        <div class="card__sub" style="display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-top:8px">
          <span>Firmware ${state.firmware || "dev"} · up ${fmtUptime(sys?.uptime_s)}</span>
          ${nameDirty ? html`<button type="button" class="btn btn--primary" style="padding:4px 10px"
              disabled=${nameBusy} onClick=${saveName}>Save name</button>` : ""}
        </div>
      </div>

      <div class="card card--metric">
        <div class="card__title">Mode</div>
        <div class="card__value">${modeLabel(mode)}</div>
        <div class="card__sub">
          <span class="pill pill--ok">active</span>
          ${state.volume?.muted ? html`<span class="pill pill--warn" style="margin-left:6px">muted</span>` : ""}
        </div>
      </div>

      <div class="card card--metric">
        <div class="card__title">Volume</div>
        <div class="card__value">
          ${state.volume?.l ?? "—"}%${
            state.volume && state.volume.l !== state.volume.r
              ? html` / <span style="color:var(--fg-1)">${state.volume.r}%</span>`
              : ""
          }
        </div>
        <div class="card__sub">
          ${state.channels?.stereo_mono === "mono" ? "Mono sum" : "Stereo"} ·
          ${state.channels?.l_on === false ? "L off · " : ""}
          ${state.channels?.r_on === false ? "R off · " : ""}
          muted=${state.volume?.muted ? "yes" : "no"}
        </div>
      </div>

      <div class="card card--metric">
        <div class="card__title">WiFi</div>
        <div class="card__value" style="font-size:18px;font-family:var(--font-mono)">
          ${sys?.wifi?.ssid || (sys?.wifi?.connected === false ? "disconnected" : "—")}
        </div>
        <div class="card__sub">
          ${sys?.wifi?.connected
            ? html`${sys.wifi.rssi} dBm · ch ${sys.wifi.channel ?? "?"} · ${sys.wifi.ip}`
            : "No link"}
        </div>
        <div class="bar" data-level=${levelFor(rssiPct, true)} style=${`--fill:${rssiPct}`}></div>
      </div>

      <div class="card card--metric">
        <div class="card__title">CPU</div>
        <div class="card__value">${c0 || 0}% <span style="color:var(--fg-2);font-size:14px">core 0</span></div>
        <div class="bar" data-level=${levelFor(c0)} style=${`--fill:${c0}`}></div>
        <div class="card__sub" style="margin-top:var(--sp-3)">${c1 || 0}% core 1</div>
        <div class="bar" data-level=${levelFor(c1)} style=${`--fill:${c1}`}></div>
      </div>

      <div class="card card--metric">
        <div class="card__title">Memory</div>
        <div class="card__value">${fmtBytes(sys?.free_heap)} <span style="color:var(--fg-2);font-size:14px">free heap</span></div>
        ${heapPct != null
          ? html`<div class="bar" data-level=${levelFor(heapPct)} style=${`--fill:${heapPct}`}></div>`
          : ""}
        <div class="card__sub" style="margin-top:var(--sp-3)">
          Min free: ${fmtBytes(sys?.min_free_heap)}
          ${sys?.free_psram ? html` · PSRAM free: ${fmtBytes(sys.free_psram)}` : ""}
        </div>
      </div>
    </div>

    ${state.led?.present ? html`
      <div class="card" style="margin-top:var(--sp-4)">
        <div class="card__title">Status LED</div>
        <p style="color: var(--fg-1); margin-top: var(--sp-2)">
          ${ledPhaseLabel(state.led.phase)}
          ${state.led.count > 1 ? html` · ${state.led.count} pixels` : ""}
        </p>
        <div class="field" style="margin-top: var(--sp-4)">
          <label class="field__label" for="led-color">Colour</label>
          <div class="row" style="gap: var(--sp-3)">
            <input id="led-color" type="color" value=${ledColor}
                   onInput=${(e) => setLedColor(e.target.value)}
                   onChange=${(e) => {
                     const c = e.target.value;
                     setLedColor(c);
                     api.setLed({ color: c, brightness: ledBri }).catch((err) => toast && toast(String(err), "err"));
                   }} />
            <span class="chip">${ledColor}</span>
          </div>
        </div>
        <div class="field" style="margin-top: var(--sp-4)">
          <label class="field__label" for="led-bri">Brightness</label>
          <div class="row" style="gap: var(--sp-3)">
            <input id="led-bri" class="slider" type="range" min="0" max="100"
                   value=${ledBri}
                   style=${`--fill:${ledBri}`}
                   onInput=${(e) => setLedBri(parseInt(e.target.value, 10))}
                   onChange=${(e) => {
                     const v = parseInt(e.target.value, 10);
                     setLedBri(v);
                     api.setLed({ color: ledColor, brightness: v }).catch((err) => toast && toast(String(err), "err"));
                   }} />
            <span class="chip">${ledBri}%</span>
          </div>
        </div>
      </div>` : ""}

    ${mode === "bluetooth"
      ? html`
        <div class="card" style="margin-top:var(--sp-4)">
          <div class="card__title">Bluetooth</div>
          <div class="card__value" style="font-size:18px">
            ${bt.connected
              ? "Connected"
              : bt.discoverable
                ? `Pairing — ${Math.ceil(pairLeftMs / 1000)}s left`
                : "Hidden — last paired phone can reconnect"}
          </div>
          <p class="card__sub" style="margin:8px 0 12px">
            Discoverable for 3 minutes after Bluetooth starts, after a disconnect,
            or when you tap Pair. Only one phone at a time.
          </p>
          <button
            type="button"
            class="btn btn--primary"
            disabled=${pairBusy || !!bt.connected}
            onClick=${pairNew}>
            ${pairBusy ? "…" : "Pair new device"}
          </button>
        </div>`
      : ""}

    ${state.mode === "snapclient" || state.source === 1
      ? html`
        <div class="card" style="margin-top:var(--sp-4)">
          <div class="card__title">Snapcast</div>
          <dl class="kv" style="margin-top:var(--sp-3)">
            <dt>Host</dt><dd>${state.snapcast?.host || "(not set)"}</dd>
            <dt>Port</dt><dd>${state.snapcast?.port ?? 1704}</dd>
          </dl>
        </div>`
      : ""}
  `;
}
