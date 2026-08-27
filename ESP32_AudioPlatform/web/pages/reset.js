// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// Reset page: granular scope-targeted wipes of persistent memory.
//
// The firmware's `/api/reset` endpoint accepts a single `{scope}` payload
// where `scope` is one of the values exported by the OpenAPI spec
// (wifi / bluetooth / snapcast / dlna / eq / audio / factory). WiFi, BT,
// and factory scopes force a reboot; the rest take effect live.
//
// UX guardrails:
//   - Every reset requires a two-step click: the initial "Reset" button
//     turns the whole row red and swaps in a "Confirm" primary button.
//     Clicking anywhere else on the page cancels the confirm state.
//   - Scopes that force a reboot surface that fact in both the blurb and
//     the post-reset toast so the user isn't surprised when the device
//     disappears for ~15 s.
//   - Scopes are grouped by impact: Connectivity, Audio state, Danger
//     zone — with a tone stripe on each row (accent / warn / err) so the
//     page reads as a menu instead of a stack of identical cards.

import { html, useEffect, useState } from "/vendor/htm-preact.mjs";

// Grouped definitions. `tone` drives the left stripe + icon colour, `icon` is
// an emoji / glyph rendered in a circle, `reboot` controls the warning text.
const SECTIONS = [
  {
    id: "connectivity",
    title: "Connectivity",
    scopes: [
      {
        id: "wifi", label: "WiFi credentials", tone: "warn", icon: "📶", reboot: true,
        blurb: "Clear the saved SSID and password. The device will boot into Wi-Fi provisioning mode (AP + captive portal).",
      },
      {
        id: "bluetooth", label: "Bluetooth pairings", tone: "warn", icon: "🎧", reboot: true,
        blurb: "Drop all paired A2DP devices. You'll need to re-pair from each source. Only relevant in Bluetooth mode.",
      },
      {
        id: "snapcast", label: "Snapcast host", tone: "ok", icon: "📡", reboot: false,
        blurb: "Forget the configured Snapcast server host and port. The Snapclient will go idle until a new host is set.",
      },
      {
        id: "dlna", label: "DLNA settings", tone: "ok", icon: "📺", reboot: false,
        blurb: "Disable DLNA advertising and clear the friendly name override.",
      },
    ],
  },
  {
    id: "audio",
    title: "Audio state",
    scopes: [
      {
        id: "eq", label: "EQ + presets", tone: "ok", icon: "🎚", reboot: false,
        blurb: "Flatten the parametric EQ curve and clear all 10 preset slots.",
      },
      {
        id: "audio", label: "Audio settings", tone: "ok", icon: "🔊", reboot: false,
        blurb: "Volume, balance, mute, channel enables, stereo/mono, and status LED colour/brightness back to factory defaults. EQ is preserved.",
      },
    ],
  },
];

const FACTORY = {
  id: "factory",
  label: "Factory reset (everything)",
  tone: "danger",
  icon: "⚠",
  reboot: true,
  blurb: "Wipe all persistent state: WiFi, BT, Snapcast, DLNA, EQ, presets, volume, device name. The device will reboot and come up in Wi-Fi provisioning mode.",
};

function ResetRow({ api, toast, scope }) {
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);

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

  const klass = "reset-row" + (confirming ? " is-confirming" : "");

  return html`
    <div class=${klass} data-tone=${scope.tone} data-reset-scope=${scope.id}>
      <div class="reset-row__icon" aria-hidden="true">${scope.icon}</div>
      <div>
        <div class="reset-row__title">${scope.label}</div>
        <p class="reset-row__blurb">${scope.blurb}</p>
        ${scope.reboot
          ? html`<div class="reset-row__note">⟳ Reboots the device (~15 s).</div>`
          : ""}
      </div>
      <div class="reset-row__actions">
        ${confirming
          ? html`
            <button class="btn" disabled=${busy} onClick=${() => setConfirming(false)}>Cancel</button>
            <button class="btn btn--danger" disabled=${busy} onClick=${doIt}
                    style="background: var(--err); color: var(--accent-ink); border-color: var(--err);">
              ${busy ? "…" : `Confirm — ${scope.label.toLowerCase()}`}
            </button>`
          : html`
            <button class="btn btn--danger" onClick=${() => setConfirming(true)}>
              Reset
            </button>`}
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

    ${SECTIONS.map((section) => html`
      <h2 class="section-title">${section.title}</h2>
      ${section.scopes.map((s) => html`
        <${ResetRow} key=${s.id} api=${api} toast=${toast} scope=${s} />
      `)}
    `)}

    <h2 class="section-title section-title--danger">Danger zone</h2>
    <${ResetRow} api=${api} toast=${toast} scope=${FACTORY} />
  `;
}
