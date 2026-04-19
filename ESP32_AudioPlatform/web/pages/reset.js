// Reset page: granular scope-targeted wipes of persistent memory.
//
// The firmware's `/api/reset` endpoint accepts a single `{scope}` payload
// where `scope` is one of the values exported by the OpenAPI spec
// (wifi / bluetooth / snapcast / dlna / eq / audio / factory). WiFi, BT,
// and factory scopes force a reboot; the rest take effect live.
//
// UX guardrails:
//   - Every reset requires a two-step click: the initial button reveals a
//     "Confirm — {action}" red button that actually fires the request.
//   - The confirm button auto-cancels if the user navigates away or clicks
//     anywhere else on the page.
//   - Scopes that force a reboot surface that fact in both the blurb and
//     the post-reset toast so the user isn't surprised when the device
//     disappears for ~15 s.

import { html, useEffect, useState } from "/vendor/htm-preact.mjs";

const SCOPES = [
  {
    id: "wifi",
    label: "WiFi credentials",
    blurb: "Clear the saved SSID and password. The device will boot into Wi-Fi provisioning mode (AP + captive portal).",
    reboot: true,
  },
  {
    id: "bluetooth",
    label: "Bluetooth pairings",
    blurb: "Drop all paired A2DP devices. You'll need to re-pair from each source. Only relevant in Bluetooth mode.",
    reboot: true,
  },
  {
    id: "snapcast",
    label: "Snapcast host",
    blurb: "Forget the configured Snapcast server host and port. The Snapclient will go idle until a new host is set.",
    reboot: false,
  },
  {
    id: "dlna",
    label: "DLNA settings",
    blurb: "Disable DLNA advertising and clear the friendly name override.",
    reboot: false,
  },
  {
    id: "eq",
    label: "EQ + presets",
    blurb: "Flatten both the calibration and preference curves and clear all 10 preset slots.",
    reboot: false,
  },
  {
    id: "audio",
    label: "Audio settings",
    blurb: "Volume, balance, mute, channel enables, and stereo/mono back to factory defaults. EQ is preserved.",
    reboot: false,
  },
];

const FACTORY = {
  id: "factory",
  label: "Factory reset (everything)",
  blurb: "Wipe all persistent state: WiFi, BT, Snapcast, DLNA, EQ, presets, volume, device name. The device will reboot and come up in Wi-Fi provisioning mode.",
  reboot: true,
};

function ResetRow({ api, toast, scope }) {
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);

  // Auto-cancel the confirm state if the user clicks elsewhere.
  useEffect(() => {
    if (!confirming) return;
    const off = (e) => {
      if (!e.target.closest(`[data-reset-scope="${scope.id}"]`)) setConfirming(false);
    };
    document.addEventListener("click", off, true);
    return () => document.removeEventListener("click", off, true);
  }, [confirming, scope.id]);

  async function doIt() {
    setBusy(true);
    try {
      const r = await api.reset(scope.id);
      if (r.reboot) {
        toast(`Reset: ${scope.label}. Rebooting…`, "warn", 5000);
      } else {
        toast(`Reset: ${scope.label}`, "ok");
      }
      setConfirming(false);
    } catch (e) {
      toast(String(e), "err");
    } finally {
      setBusy(false);
    }
  }

  return html`
    <div class="card" data-reset-scope=${scope.id}>
      <div style="display:flex; justify-content:space-between; align-items:flex-start; gap: var(--sp-4)">
        <div style="flex:1">
          <div class="card__title">${scope.label}</div>
          <p style="color: var(--fg-1); margin: var(--sp-2) 0 0;">${scope.blurb}</p>
          ${scope.reboot
            ? html`<p style="color: var(--warn); font-size: 12px; margin-top: var(--sp-2)">This reset reboots the device.</p>`
            : ""}
        </div>
        <div style="display:flex; gap: var(--sp-2); flex-shrink: 0;">
          ${confirming
            ? html`
              <button class="btn" disabled=${busy} onClick=${() => setConfirming(false)}>Cancel</button>
              <button class="btn btn--danger" disabled=${busy} onClick=${doIt}>
                ${busy ? "…" : `Confirm — ${scope.label.toLowerCase()}`}
              </button>`
            : html`
              <button class="btn btn--danger" onClick=${() => setConfirming(true)}>
                Reset
              </button>`}
        </div>
      </div>
    </div>
  `;
}

export default function Reset({ api, toast }) {
  return html`
    <h1 class="page__title">Reset</h1>
    <p class="page__subtitle">
      Scope-targeted wipes of persistent memory. Each reset requires a
      separate confirm click.
    </p>

    ${SCOPES.map((s) => html`<${ResetRow} key=${s.id} api=${api} toast=${toast} scope=${s} />`)}

    <div style="margin-top: var(--sp-6); padding-top: var(--sp-5); border-top: 1px solid var(--line)">
      <h2 style="margin: 0 0 var(--sp-3); font-size: 16px; color: var(--err);">
        Danger zone
      </h2>
      <${ResetRow} api=${api} toast=${toast} scope=${FACTORY} />
    </div>
  `;
}
