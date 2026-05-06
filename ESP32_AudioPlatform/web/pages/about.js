// About page. Cards show firmware build metadata, chip/hardware identity,
// network MACs, filesystem info and links to the embedded API docs and
// project source.
//
// Identity comes from `/api/system/identity`; heap/PSRAM from
// `/api/system/metrics` (same split as Overview polling).

import { html, useEffect, useState } from "/vendor/htm-preact.mjs";

function Card({ title, children }) {
  return html`
    <div class="card">
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

export default function About({ api }) {
  const [sys, setSys] = useState(null);
  const [err, setErr] = useState(null);

  useEffect(() => {
    Promise.all([api.getSystemIdentity(), api.getSystemMetrics()])
      .then(([id, m]) => setSys({ ...id, ...m }))
      .catch((e) => setErr(String(e)));
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

    <div class="grid">
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
        <${Row} k="Manual test matrix">
          <a href="https://github.com/" target="_blank" rel="noopener">docs/manual-test-matrix.md ↗</a>
        </${Row}>
      </${Card}>

      <${Card} title="Licensing">
        <${Row} k="Firmware">MIT © Geelen</${Row}>
        <${Row} k="ESP-IDF">Apache-2.0 © Espressif</${Row}>
        <${Row} k="Preact / HTM">MIT</${Row}>
        <${Row} k="Swagger UI">Apache-2.0</${Row}>
        <${Row} k="libhelix-mp3">RealNetworks RPSL</${Row}>
      </${Card}>
    </div>
  `;
}
