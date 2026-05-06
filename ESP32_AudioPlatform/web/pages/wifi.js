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

import { html, useEffect, useState } from "/vendor/htm-preact.mjs";

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

  useEffect(() => {
    let cancelled = false;
    api.getWifiState().then(
      (s) => { if (!cancelled) setState(s); },
      () => { if (!cancelled) setState({ _err: true }); },
    );
    return () => { cancelled = true; };
  }, []);

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
  `;
}
