// Wi-Fi page: change credentials from the SPA.
//
// On mount this page fires off two requests in parallel:
//   - /api/wifi/state  — current STA SSID/IP/RSSI + AP fallback flag, used
//     for the status header.
//   - /api/wifi/scan   — list of nearby SSIDs, used to populate the dropdown.
//
// Submitting the form posts form-encoded creds to POST /api/wifi (same
// endpoint the inline AP-mode portal uses) and shows a 15 s reboot
// countdown so the user knows when to expect the device back online.
//
// The Settings card at the bottom hosts power-user knobs (Wi-Fi power-save
// today; pencil in further radio toggles here as they land). It subscribes
// to the SSE state stream rather than polling so a flip from another tab
// is reflected immediately.

import { html, useEffect, useState } from "/vendor/htm-preact.mjs";

// Wi-Fi power-save options. `value` matches the firmware's wire format
// (POST /api/wifi/power-save accepts these strings); `label` is what the
// user sees in the segmented control.
const WIFI_PS_OPTIONS = [
  { value: "none",      label: "Off",        hint: "Best for streaming. Highest power draw." },
  { value: "min_modem", label: "Min modem",  hint: "Modem-sleep. ~1–2 ms extra inbound latency." },
  { value: "max_modem", label: "Max modem",  hint: "Aggressive sleep. Visible audio glitches likely." },
];

function rssiPct(rssi) {
  if (rssi == null) return 0;
  const p = ((rssi + 90) / 60) * 100;
  return Math.max(0, Math.min(100, Math.round(p)));
}

function authLabel(a) {
  // Order matches enum wifi_auth_mode_t in esp_wifi_types.h.
  const map = ["open", "WEP", "WPA", "WPA2", "WPA/WPA2", "Enterprise", "WPA3", "WPA2/WPA3", "WAPI"];
  return map[a] || "unknown";
}

export default function Wifi({ api, toast }) {
  const [state, setState] = useState(null);
  const [scan, setScan] = useState(null);
  const [scanErr, setScanErr] = useState(null);
  const [scanning, setScanning] = useState(false);
  const [ssid, setSsid] = useState("");
  const [manual, setManual] = useState(false);
  const [pass, setPass] = useState("");
  const [busy, setBusy] = useState(false);
  const [countdown, setCountdown] = useState(null);
  // Settings card — Wi-Fi power-save selector. Mirrors `state.wifi.power_save`
  // from /api/state. `psBusy` gates the buttons during the POST round-trip
  // so a fast double-tap doesn't fire two requests.
  const [ps, setPs] = useState(null);
  const [psBusy, setPsBusy] = useState(false);

  useEffect(() => {
    let cancelled = false;
    api.getWifiState().then(
      (s) => { if (!cancelled) setState(s); },
      () => { if (!cancelled) setState({ _err: true }); },
    );
    return () => { cancelled = true; };
  }, []);

  // Subscribe to the SSE device-state stream for the power-save value so
  // a flip from another tab / curl reflects here instantly. We keep the
  // one-shot getWifiState() above for the live STA status block (RSSI,
  // IP, AP fallback) which is not part of the persisted state snapshot.
  useEffect(() => {
    const stop = api.subscribeState((s) => {
      const next = s && s.wifi && s.wifi.power_save;
      if (next) setPs(next);
    });
    return stop;
  }, []);

  async function onChangePs(next) {
    if (psBusy || next === ps) return;
    setPsBusy(true);
    const prev = ps;
    setPs(next); // optimistic — SSE will reaffirm
    try {
      await api.setWifiPowerSave(next);
      toast && toast("Saved. Reboot to apply.", "ok");
    } catch (e) {
      setPs(prev);
      toast && toast(`Save failed: ${e.message || e}`, "err", 5000);
    } finally {
      setPsBusy(false);
    }
  }

  async function rescan() {
    setScanning(true);
    setScanErr(null);
    try {
      const r = await api.getWifiScan();
      const list = (r && r.networks) || [];
      setScan(list);
      if (!ssid && list.length) setSsid(list[0].ssid);
    } catch (e) {
      setScanErr(String(e));
      setScan([]);
    } finally {
      setScanning(false);
    }
  }
  useEffect(() => { rescan(); }, []);

  async function onSubmit(ev) {
    ev.preventDefault();
    if (busy) return;
    const target = manual ? ssid.trim() : ssid;
    if (!target) {
      toast && toast("Pick or type a network name first.", "err");
      return;
    }
    setBusy(true);
    try {
      await api.setWifi(target, pass);
      toast && toast("Saving and rebooting…", "ok", 4000);
      setCountdown(15);
    } catch (e) {
      toast && toast(`Save failed: ${e.message || e}`, "err", 5000);
      setBusy(false);
    }
  }

  // Reboot countdown (purely cosmetic — we have no way to ping the device
  // while it reboots, but a ticking number reassures the user).
  useEffect(() => {
    if (countdown == null) return;
    if (countdown <= 0) return;
    const t = setTimeout(() => setCountdown((n) => (n == null ? null : n - 1)), 1000);
    return () => clearTimeout(t);
  }, [countdown]);

  if (countdown != null) {
    return html`
      <h1 class="page__title">Wi-Fi setup</h1>
      <p class="page__subtitle">Device is rebooting and joining ${ssid || "your network"}.</p>
      <div class="card" style="text-align:center;padding:32px">
        <div style="font-size:42px;font-weight:600;font-family:var(--font-mono)">${Math.max(0, countdown)}</div>
        <div class="page__subtitle">Seconds until you can reach it again at its new address.</div>
        <p class="page__subtitle" style="margin-top:16px">
          If you were on the <strong>Geelen-Audio</strong> setup hotspot,
          switch your phone/laptop back to your normal Wi-Fi.
        </p>
      </div>
    `;
  }

  return html`
    <h1 class="page__title">Wi-Fi setup</h1>
    <p class="page__subtitle">Change the network the amplifier connects to.</p>

    <div class="card" style="margin-bottom:var(--sp-4)">
      <div class="card__title">Status</div>
      ${!state
        ? html`<div class="page__subtitle">Loading…</div>`
        : state._err
          ? html`<div class="page__subtitle">Could not read Wi-Fi state.</div>`
          : html`
            <dl class="kv" style="margin-top:var(--sp-3)">
              <dt>Mode</dt>
              <dd>
                ${state.ap_only
                  ? html`<span class="pill pill--warn">Setup hotspot</span>`
                  : state.sta_connected
                    ? html`<span class="pill pill--ok">Joined ${state.ssid || "network"}</span>`
                    : html`<span class="pill pill--err">Disconnected</span>`}
              </dd>
              ${state.sta_connected ? html`
                <dt>SSID</dt><dd style="font-family:var(--font-mono)">${state.ssid || "—"}</dd>
                <dt>IP</dt><dd style="font-family:var(--font-mono)">${state.ip || "—"}</dd>
                <dt>RSSI</dt><dd>${state.rssi} dBm (${rssiPct(state.rssi)}%)</dd>
              ` : ""}
              ${state.ap_only ? html`
                <dt>Hotspot SSID</dt><dd style="font-family:var(--font-mono)">${state.ap_ssid || "Geelen-Audio"}</dd>
                <dt>Hotspot IP</dt><dd style="font-family:var(--font-mono)">${state.ap_ip || "192.168.4.1"}</dd>
              ` : ""}
            </dl>`}
    </div>

    <form class="card" onSubmit=${onSubmit}>
      <div class="card__title" style="display:flex;justify-content:space-between;align-items:center">
        <span>Connect to network</span>
        <button type="button" class="btn btn--ghost" onClick=${rescan} disabled=${scanning}>
          ${scanning ? "Scanning…" : "Rescan"}
        </button>
      </div>

      <label style="display:block;margin-top:var(--sp-3)">
        <div class="page__subtitle" style="margin-bottom:4px">Network</div>
        ${manual
          ? html`<input
              type="text"
              value=${ssid}
              onInput=${(e) => setSsid(e.target.value)}
              placeholder="Network name (SSID)"
              style="width:100%" />`
          : html`<select
              value=${ssid}
              onChange=${(e) => setSsid(e.target.value)}
              style="width:100%">
              ${(scan || []).map((n) => html`
                <option value=${n.ssid}>
                  ${n.ssid} — ${n.rssi} dBm · ch ${n.channel} · ${authLabel(n.auth)}
                </option>
              `)}
              ${!scan || scan.length === 0
                ? html`<option value="">${scanning ? "Scanning…" : "No networks found"}</option>`
                : ""}
            </select>`}
        <div style="margin-top:6px">
          <button type="button" class="btn btn--ghost" onClick=${() => setManual((m) => !m)}>
            ${manual ? "Pick from list" : "Type SSID manually"}
          </button>
        </div>
        ${scanErr ? html`<div class="page__subtitle" style="color:var(--err);margin-top:4px">${scanErr}</div>` : ""}
      </label>

      <label style="display:block;margin-top:var(--sp-3)">
        <div class="page__subtitle" style="margin-bottom:4px">Password</div>
        <input
          type="password"
          value=${pass}
          onInput=${(e) => setPass(e.target.value)}
          autocomplete="new-password"
          placeholder=${state && state.sta_connected ? "(leave blank to keep current)" : "WPA2 password"}
          style="width:100%" />
      </label>

      <div style="margin-top:var(--sp-4);display:flex;gap:8px;align-items:center">
        <button type="submit" class="btn btn--primary" disabled=${busy}>
          ${busy ? "Saving…" : "Save & connect"}
        </button>
        <span class="page__subtitle">Device will reboot to apply.</span>
      </div>
    </form>

    <div class="card" style="margin-top:var(--sp-4)">
      <div class="card__title">Settings</div>

      <div style="margin-top:var(--sp-3)">
        <div class="page__subtitle" style="margin-bottom:6px">
          Wi-Fi power-save
        </div>
        <div role="radiogroup"
             aria-label="Wi-Fi power-save mode"
             style="display:flex;gap:6px;flex-wrap:wrap">
          ${WIFI_PS_OPTIONS.map((opt) => {
            const active = ps === opt.value;
            return html`<button
              type="button"
              role="radio"
              aria-checked=${active}
              class=${"btn " + (active ? "btn--primary" : "btn--ghost")}
              disabled=${psBusy || ps == null}
              title=${opt.hint}
              onClick=${() => onChangePs(opt.value)}>
              ${opt.label}
            </button>`;
          })}
        </div>
        <div class="page__subtitle" style="margin-top:8px">
          ${ps == null
            ? "Loading current setting…"
            : (WIFI_PS_OPTIONS.find((o) => o.value === ps) || {}).hint || ""}
        </div>
        <div class="page__subtitle" style="margin-top:6px">
          Persisted in NVS, applies on next boot. Default <strong>Off</strong>
          is the right answer for any device that streams audio; the modem-sleep
          options trade audio reliability for ≈30–80 mA of saved current.
        </div>
      </div>
    </div>
  `;
}
