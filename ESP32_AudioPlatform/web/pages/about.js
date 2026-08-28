// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// About page. Cards show firmware build metadata, chip/hardware identity,
// editable device identity (name, hostname, MAC override), filesystem info
// and links to the embedded API docs and project source.
//
// Identity comes from `/api/identity` plus `/api/system/identity`;
// heap/PSRAM from `/api/system/metrics`.

import { html, useEffect, useState } from "/vendor/htm-preact.mjs";

function Card({ title, children, className = "" }) {
  return html`
    <div class=${"card" + (className ? " " + className : "")}>
      <div class="card__title">${title}</div>
      <dl class="kv">${children}</dl>
    </div>
  `;
}

function Row({ k, v, mono = false, children }) {
  return html`
    <dt>${k}</dt>
    <dd style=${mono ? "font-family: var(--font-mono); font-size: 12px;" : ""}>${children != null ? children : (v || "—")}</dd>
  `;
}

function IdentityEditor({ api, toast, ident, onSaved }) {
  const [name, setName] = useState(ident.device_name || "");
  const [hostname, setHostname] = useState(ident.hostname || "");
  const [sta, setSta] = useState(ident.mac?.sta?.current || "");
  const [bt, setBt] = useState(ident.mac?.bt?.current || "");
  const [overSta, setOverSta] = useState(!!ident.mac?.sta?.overridden);
  const [overBt, setOverBt] = useState(!!ident.mac?.bt?.overridden);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    setName(ident.device_name || "");
    setHostname(ident.hostname || "");
    setSta(ident.mac?.sta?.current || "");
    setBt(ident.mac?.bt?.current || "");
    setOverSta(!!ident.mac?.sta?.overridden);
    setOverBt(!!ident.mac?.bt?.overridden);
  }, [ident]);

  const mdnsUrl = hostname ? `http://${hostname}.local/` : "";

  async function save() {
    setBusy(true);
    try {
      const patch = { name };
      const hostDirty = hostname !== (ident.hostname || "") ||
        (!hostname && ident.hostname_custom);
      if (hostDirty) patch.hostname = hostname || null;

      const staCur = (ident.mac?.sta?.current || "").toLowerCase();
      const btCur = (ident.mac?.bt?.current || "").toLowerCase();
      if (overSta) {
        if (sta.toLowerCase() !== staCur || !ident.mac?.sta?.overridden) patch.sta_mac = sta;
      } else if (ident.mac?.sta?.overridden) {
        patch.sta_mac = null;
      }
      if (overBt) {
        if (bt.toLowerCase() !== btCur || !ident.mac?.bt?.overridden) patch.bt_mac = bt;
      } else if (ident.mac?.bt?.overridden) {
        patch.bt_mac = null;
      }
      const res = await api.setIdentity(patch);
      if (res.reboot) {
        toast && toast("Saved — rebooting to apply MAC", "ok", 5000);
      } else {
        toast && toast("Identity saved", "ok");
        onSaved && onSaved(res);
      }
    } catch (e) {
      toast && toast(String(e), "err");
    } finally {
      setBusy(false);
    }
  }

  function resetSlug() {
    setHostname("");
  }

  return html`
    <div class="card about-grid__span-all">
      <div class="card__title">Identity</div>
      <p class="page__subtitle" style="margin:0 0 12px">
        Friendly name is shown in Bluetooth, DLNA, and mDNS. Hostname is the
        DHCP / <code>.local</code> label. Custom MACs reboot the device, break
        existing BT pairing, and may change the DHCP IP.
      </p>
      <div class="about-ident">
        <div class="field">
          <label class="field__label" for="id-name">Device name</label>
          <input id="id-name" class="input" type="text" maxlength="31" value=${name}
            disabled=${busy} onInput=${(e) => setName(e.target.value)} />
          <div class="about-ident__hint card__sub">Bluetooth, DLNA, mDNS instance</div>
        </div>
        <div class="field">
          <label class="field__label" for="id-host">Hostname</label>
          <input id="id-host" class="input" type="text" maxlength="31" value=${hostname}
            placeholder="slug from device name" disabled=${busy}
            onInput=${(e) => setHostname(e.target.value.toLowerCase())} />
          <div class="about-ident__hint card__sub">
            <span style="font-family:var(--font-mono);font-size:12px">${mdnsUrl || "—"}</span>
            <button type="button" class="btn btn--ghost" style="padding:4px 10px"
              disabled=${busy} onClick=${resetSlug}>Reset to slug</button>
          </div>
        </div>
        <div class="field">
          <label class="field__label" for="id-sta">Wi-Fi STA MAC</label>
          <input id="id-sta" class="input" type="text" maxlength="17" value=${sta}
            disabled=${busy || !overSta} style="font-family:var(--font-mono)"
            onInput=${(e) => setSta(e.target.value)} />
          <div class="about-ident__hint card__sub">
            <label>
              <input type="checkbox" checked=${overSta} disabled=${busy}
                onChange=${(e) => {
                  setOverSta(e.target.checked);
                  if (!e.target.checked) setSta(ident.mac?.sta?.factory || sta);
                }} />
              Override
            </label>
            <span>Factory ${ident.mac?.sta?.factory || "—"}</span>
          </div>
        </div>
        <div class="field">
          <label class="field__label" for="id-bt">Bluetooth MAC</label>
          <input id="id-bt" class="input" type="text" maxlength="17" value=${bt}
            disabled=${busy || !overBt} style="font-family:var(--font-mono)"
            onInput=${(e) => setBt(e.target.value)} />
          <div class="about-ident__hint card__sub">
            <label>
              <input type="checkbox" checked=${overBt} disabled=${busy}
                onChange=${(e) => {
                  setOverBt(e.target.checked);
                  if (!e.target.checked) setBt(ident.mac?.bt?.factory || bt);
                }} />
              Override
            </label>
            <span>Factory ${ident.mac?.bt?.factory || "—"}</span>
          </div>
        </div>
      </div>
      <button type="button" class="btn btn--primary" style="margin-top:14px"
        disabled=${busy} onClick=${() => save()}>Save identity</button>
    </div>
  `;
}

export default function About({ api, toast }) {
  const [sys, setSys] = useState(null);
  const [ident, setIdent] = useState(null);
  const [err, setErr] = useState(null);

  function load() {
    return Promise.all([api.getSystemIdentity(), api.getSystemMetrics(), api.getIdentity()])
      .then(([id, m, identBody]) => {
        setSys({ ...id, ...m });
        setIdent(identBody && !identBody._unimplemented ? identBody : (id && id.identity) || null);
      });
  }

  useEffect(() => {
    load().catch((e) => setErr(String(e)));
  }, []);

  if (err) return html`<div class="stub">Failed to load system info: ${err}</div>`;
  if (!sys) return html`<div class="stub">Loading…</div>`;

  const fw = sys.firmware || {};
  const chip = sys.chip || {};
  const net = sys.net || {};
  const wifi = sys.wifi || {};

  return html`
    <h1 class="page__title">About</h1>
    <p class="page__subtitle">Firmware, hardware and references.</p>

    <div class="about-grid">
      ${ident ? html`<${IdentityEditor} api=${api} toast=${toast} ident=${ident} onSaved=${setIdent} />` : ""}

      <${Card} title="Firmware">
        <${Row} k="Version"     v=${fw.version} mono />
        <${Row} k="Project"     v=${fw.project} />
        <${Row} k="Built"       v=${`${fw.build_date || ""} ${fw.build_time || ""}`.trim()} mono />
        <${Row} k="ESP-IDF"     v=${fw.idf_ver} mono />
        <${Row} k="ELF SHA-256" mono>
          <span title=${fw.elf_sha256 || ""}>
            ${fw.elf_sha256 ? fw.elf_sha256.slice(0, 16) + "…" : "—"}
          </span>
        </${Row}>
      </${Card}>

      <${Card} title="Chip">
        <${Row} k="Model"       v=${chip.model} />
        <${Row} k="Revision"    v=${chip.revision != null ? `rev ${chip.revision}` : null} />
        <${Row} k="Cores"       v=${chip.cores} />
        <${Row} k="Free heap"   v=${sys.free_heap != null ? `${(sys.free_heap / 1024).toFixed(1)} KB` : null} />
        <${Row} k="Free PSRAM"  v=${sys.free_psram != null ? `${(sys.free_psram / 1024).toFixed(1)} KB` : null} />
      </${Card}>

      <${Card} title="Network">
        <${Row} k="Hostname"    v=${net.hostname} mono />
        <${Row} k="STA MAC"     v=${net.sta_mac} mono />
        <${Row} k="AP MAC"      v=${net.ap_mac} mono />
        <${Row} k="BT MAC"      v=${net.bt_mac} mono />
        <${Row} k="IP"          v=${wifi.ip} mono />
        <${Row} k="SSID"        v=${wifi.ssid} />
      </${Card}>

      <${Card} title="Documentation">
        <${Row} k="API reference">
          <a href="/docs" target="_blank" rel="noopener">Swagger UI ↗</a>
        </${Row}>
        <${Row} k="OpenAPI spec">
          <a href="/openapi.yaml" target="_blank" rel="noopener">openapi.yaml ↗</a>
        </${Row}>
        <${Row} k="Identity">
          Device name, hostname, MAC — see the Identity card above.
        </${Row}>
      </${Card}>

      <${Card} title="Licensing" className="about-grid__span-2">
        <${Row} k="Firmware">
          GPL-3.0-or-later ·
          <a href="/COPYING" target="_blank" rel="noopener">COPYING ↗</a>
        </${Row}>
        <${Row} k="sonocotta tas5805m (Louder)">GPL-3.0-or-later</${Row}>
        <${Row} k="ESP-IDF">Apache-2.0 © Espressif</${Row}>
        <${Row} k="esp32-audioI2S / esp32-a2dp">LGPL-3.0</${Row}>
        <${Row} k="Preact / HTM">MIT</${Row}>
        <${Row} k="Swagger UI">Apache-2.0</${Row}>
      </${Card}>
    </div>
  `;
}
