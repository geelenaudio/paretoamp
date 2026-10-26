// Mode switch page.
//
// Audio sources map to the firmware's AudioSource enum:
//   - bluetooth : Classic BT A2DP sink                       (ordinal 0)
//   - snapclient: Snapcast client, configurable host/port    (ordinal 1)
//   - dlna      : Native UPnP/AV renderer, configurable name (ordinal 2)
//   - usb       : UAC playback gadget (S3 only, supports_usb) (ordinal 4)
//
// Every card renders a "Select" CTA plus the mode's own inline settings.
// Changing the active mode reboots the device so the firmware can rebuild its
// task graph (BT stack, Snapclient, DLNA, test-tone generator).
//
// Saving a mode's settings (Snapcast host, DLNA name) is independent of
// selecting it — you can pre-configure Snapcast while BT is active, then
// flip to snapclient knowing the target is already set.

import { html, useEffect, useState } from "/vendor/htm-preact.mjs";
import { IcoHeadphones, IcoCast, IcoTv, IcoFlask, IcoUsb } from "/icons.js";

// Static descriptor. `configure()` returns the inline settings subtree for
// each mode so the card renderer stays declarative.
const MODES = [
  {
    id: "bluetooth",
    label: "Bluetooth",
    icon: IcoHeadphones,
    blurb:
      "Pair your phone, laptop, or tablet directly. Best when you're the " +
      "only listener and don't need multi-room sync.",
  },
  {
    id: "snapclient",
    label: "Snapcast",
    icon: IcoCast,
    blurb:
      "Join a Snapcast server for sample-accurate multi-room audio. Works " +
      "great with Mopidy, Home Assistant, Spotifyd, and shairport-sync.",
  },
  {
    id: "dlna",
    label: "DLNA renderer",
    icon: IcoTv,
    blurb:
      "Appear as a UPnP/AV MediaRenderer on the LAN. Control from BubbleUPnP, " +
      "VLC, Kodi, or the native app of most NAS/streaming services.",
  },
  {
    id: "usb",
    label: "USB audio",
    icon: IcoUsb,
    blurb:
      "Appear as a USB sound card on a computer. Playback only, through the " +
      "on-device EQ. ESP32-S3 boards with native USB; not the WROVER UART jack.",
    requiresUsb: true,
  },
  {
    id: "test_tone",
    label: "Test bench",
    icon: IcoFlask,
    blurb:
      "Built-in sine generator through the full DSP chain. Use for room checks " +
      "and wiring verification. Adjust frequency and level on the Test page after boot.",
  },
];

function SnapcastSettings({ state, onSave, busy }) {
  const [host, setHost] = useState("");
  const [port, setPort] = useState(1704);
  const [dirty, setDirty] = useState(false);

  useEffect(() => {
    setHost(state.snapcast?.host || "");
    setPort(state.snapcast?.port ?? 1704);
    setDirty(false);
  }, [state]);

  return html`
    <div style="margin-top: var(--sp-4); padding-top: var(--sp-4); border-top: 1px solid var(--line)">
      <div class="field">
        <label class="field__label" for="snap-host">Server host</label>
        <input
          id="snap-host"
          class="input"
          type="text"
          value=${host}
          placeholder="snapserver.local or 10.0.80.34"
          onInput=${(e) => { setHost(e.target.value); setDirty(true); }}
          disabled=${busy}
        />
      </div>
      <div class="field">
        <label class="field__label" for="snap-port">Port</label>
        <input
          id="snap-port"
          class="input"
          type="number"
          min="1" max="65535"
          value=${port}
          onInput=${(e) => { setPort(parseInt(e.target.value, 10) || 1704); setDirty(true); }}
          disabled=${busy}
        />
      </div>
      <button
        class="btn btn--primary"
        disabled=${busy || !dirty || !host}
        onClick=${() => onSave({ host, port })}>
        Save Snapcast settings
      </button>
    </div>
  `;
}

function DlnaSettings({ state, onSave, busy }) {
  const [name, setName] = useState("");
  const [enabled, setEnabled] = useState(false);
  const [dirty, setDirty] = useState(false);

  useEffect(() => {
    setName(state.dlna?.name || state.device_name || "Pareto Amp");
    setEnabled(!!state.dlna?.enabled);
    setDirty(false);
  }, [state]);

  return html`
    <div style="margin-top: var(--sp-4); padding-top: var(--sp-4); border-top: 1px solid var(--line)">
      <div class="field">
        <label class="field__label" for="dlna-name">Friendly name (shown on the network)</label>
        <input
          id="dlna-name"
          class="input"
          type="text"
          maxLength="32"
          value=${name}
          placeholder="Living Room Amp"
          onInput=${(e) => { setName(e.target.value); setDirty(true); }}
          disabled=${busy}
        />
      </div>
      <label style="display:flex; align-items:center; gap: var(--sp-3); margin: var(--sp-3) 0;">
        <span class="switch">
          <input
            type="checkbox"
            checked=${enabled}
            onChange=${(e) => { setEnabled(e.target.checked); setDirty(true); }}
            disabled=${busy}
          />
          <span class="switch__slider"></span>
        </span>
        <span>Advertise as a renderer when this mode is active</span>
      </label>
      <button
        class="btn btn--primary"
        disabled=${busy || !dirty}
        onClick=${() => onSave({ name, enabled })}>
        Save DLNA settings
      </button>
    </div>
  `;
}

export default function Mode({ api, toast }) {
  const [state, setState] = useState(null);
  const [busy, setBusy] = useState(false);
  const [apOnly, setApOnly] = useState(false);

  // Push-based subscription replaces the manual `refresh()` after every
  // mutation: the firmware re-publishes the snapshot when stateVersion
  // bumps (which `setMode`, `setSnapcast`, `setDlna`, `setVoiceFeedback`
  // all trigger via markDirty*). One round-trip per mutation, not two.
  useEffect(() => {
    const stop = api.subscribeState((s) => setState(s));
    return stop;
  }, []);

  useEffect(() => {
    let cancelled = false;
    api.getWifiState().then(
      (s) => {
        if (cancelled || !s) return;
        setApOnly(!!(s.ap_only || (s.ap_running && !s.sta_connected)));
      },
      () => {},
    );
    return () => { cancelled = true; };
  }, []);

  const active = state ? (state.mode || (state.source === 0 ? "bluetooth" : "snapclient")) : null;

  async function select(mode) {
    if (busy || mode === active) return;
    setBusy(true);
    try {
      const r = await api.setMode(mode);
      if (r.reboot) {
        toast("Rebooting to switch source. Reconnect in ~15 s.", "warn", 6000);
        return;
      }
      toast(`Mode: ${mode}`, "ok");
    } catch (e) { toast(String(e), "err"); }
    finally { setBusy(false); }
  }

  async function saveSnapcast({ host, port }) {
    setBusy(true);
    try {
      await api.setSnapcast(host, port);
      toast("Snapcast settings saved", "ok");
    } catch (e) { toast(String(e), "err"); }
    finally { setBusy(false); }
  }

  async function saveDlna(patch) {
    setBusy(true);
    try {
      await api.setDlna(patch);
      toast("DLNA settings saved", "ok");
    } catch (e) { toast(String(e), "err"); }
    finally { setBusy(false); }
  }

  async function saveVoiceFeedback(enabled) {
    setBusy(true);
    try {
      await api.setVoiceFeedback(enabled);
      toast("Voice feedback saved", "ok");
    } catch (e) { toast(String(e), "err"); }
    finally { setBusy(false); }
  }

  if (!state) return html`<div class="stub">Loading…</div>`;

  return html`
    <h1 class="page__title">Mode</h1>
    <p class="page__subtitle">
      Pick which source drives the amplifier. Any mode change reboots the device
      so the firmware can start or stop Bluetooth, Snapclient, DLNA, USB, or the test generator.
    </p>
    ${apOnly ? html`<p class="banner banner--warn" style="margin-bottom:var(--sp-4)">
      Setup hotspot only — Snapcast to a home-LAN server and DLNA from a phone on
      home Wi-Fi cannot reach this device until it joins that network.
    </p>` : ""}

    <div class="card" style="margin-bottom: var(--sp-5); max-width: 520px;">
      <div class="card__title">Voice feedback</div>
      <p style="color: var(--fg-1); margin: var(--sp-3) 0;">
        Short spoken cues from built-in clips (Wi‑Fi and input mode at boot;
        Bluetooth connected and pairing while in BT mode).
      </p>
      <label style="display:flex; align-items:center; gap: var(--sp-3);">
        <span class="switch">
          <input
            type="checkbox"
            checked=${!!state.voice_feedback}
            onChange=${(e) => saveVoiceFeedback(e.target.checked)}
            disabled=${busy}
          />
          <span class="switch__slider"></span>
        </span>
        <span>Enable voice prompts</span>
      </label>
    </div>

    <div class="grid" style="grid-template-columns: repeat(auto-fit, minmax(320px, 1fr))">
      ${MODES.filter((m) => !m.requiresUsb || state.supports_usb).map((m) => {
        const isActive = active === m.id;
        return html`
          <div key=${m.id} class="card" style=${{
            borderColor: isActive ? "var(--accent)" : "var(--line)",
            boxShadow: isActive ? "0 0 0 1px var(--accent)" : "var(--shadow-1)",
          }}>
            <div style="display:flex; align-items:flex-start; gap: var(--sp-4);">
              <div class=${"mode-card__icon" + (isActive ? " mode-card__icon--active" : "")} aria-hidden="true">
                <${m.icon} />
              </div>
              <div style="flex:1">
                <div class="card__title">${isActive ? "Active source" : "Available"}</div>
                <div class="card__value" style="font-size: 20px;">${m.label}</div>
              </div>
              ${isActive
                ? html`<span class="pill pill--ok" style="align-self:center">active</span>`
                : ""}
            </div>
            <p style="color: var(--fg-1); margin: var(--sp-3) 0 0;">${m.blurb}</p>
            <div style="margin-top: var(--sp-4)">
              <button
                class=${"btn" + (isActive ? "" : " btn--primary")}
                disabled=${busy || isActive}
                onClick=${() => select(m.id)}>
                ${isActive ? "Selected" : "Switch to " + m.label}
              </button>
            </div>
            ${m.id === "snapclient"
              ? html`<${SnapcastSettings} state=${state} onSave=${saveSnapcast} busy=${busy} />`
              : ""}
            ${m.id === "dlna"
              ? html`<${DlnaSettings} state=${state} onSave=${saveDlna} busy=${busy} />`
              : ""}
          </div>
        `;
      })}
    </div>
  `;
}
