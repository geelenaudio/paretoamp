// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Geelen Audio contributors
//
// DSP page — single-layer parametric EQ, headroom, presets.
//
// Preview math mirrors `eq_coeffs.cpp` so what you see on the plot is what
// the device applies. The `state.eq.max_bands` value (from the server) sets
// the runtime cap (10 on Amped, 15 on Louder); `state.eq.backend` reports
// which backend actually applies the biquads ("software", "hardware", or
// "software (fallback)"). Louder may set `supports_split`; Amped is always
// one ganged curve.

import { html, useEffect, useState, useRef, useMemo, useCallback } from "/vendor/htm-preact.mjs";

const SR = 44100;
const MIN_FREQ = 20, MAX_FREQ = 20000;
const MIN_GAIN = -18, MAX_GAIN = 18;
const MIN_Q = 0.1, MAX_Q = 10, MAX_Q_NOTCH = 100;
const BYPASS_DB = 0.01;
const LP_HP_Q = 0.70710678118;
// Storage capacity. The runtime cap comes from `state.eq.max_bands` and
// gates the +/− band controls at render time. 15 matches
// `geelen::EQ_MAX_BANDS` on the firmware side.
const MAX_STORAGE_BANDS = 15;
const DEFAULT_MAX_BANDS = 10;

const PLOT_W = 860;
const PLOT_H = 280;
const DB_RANGE = 20;
const FREQ_POS_MAX = 1000;

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
function logn(x) { return Math.log(x); }
function pct(v, lo, hi) { return clamp(((v - lo) / (hi - lo)) * 100, 0, 100); }

function freqToX(f, w) {
  const t = (logn(f) - logn(MIN_FREQ)) / (logn(MAX_FREQ) - logn(MIN_FREQ));
  return t * w;
}
function xToFreq(x, w) {
  const t = clamp(x / w, 0, 1);
  return Math.exp(logn(MIN_FREQ) + t * (logn(MAX_FREQ) - logn(MIN_FREQ)));
}
function gainToY(g, h) { return h / 2 - (g / DB_RANGE) * (h / 2); }
function yToGain(y, h) { return -((y - h / 2) / (h / 2)) * DB_RANGE; }
function freqToPos(f) { return Math.round(freqToX(clamp(f, MIN_FREQ, MAX_FREQ), FREQ_POS_MAX)); }
function posToFreq(p) { return Math.round(xToFreq(clamp(p, 0, FREQ_POS_MAX), FREQ_POS_MAX)); }
function formatHz(f) {
  if (f >= 1000) return (f / 1000).toFixed(f >= 10000 ? 0 : 2).replace(/\.?0+$/, "") + " kHz";
  return Math.round(f) + " Hz";
}

function normalizeBand(b) {
  const freq = clamp(b.freq ?? 1000, MIN_FREQ, SR * 0.45);
  const gain_db = clamp(b.gain_db ?? 0, MIN_GAIN, MAX_GAIN);
  const qHi = b.type === "notch" ? MAX_Q_NOTCH : MAX_Q;
  const q = clamp(b.q ?? 1.2, MIN_Q, qHi);
  let slope = b.slope_db_oct ?? 12;
  if (slope !== 6 && slope !== 12 && slope !== 24) slope = 12;
  const types = ["bell", "low_shelf", "high_shelf", "notch", "low_pass", "high_pass"];
  const type = types.includes(b.type) ? b.type : "bell";
  return { freq, gain_db, q, type, slope_db_oct: slope };
}

function normBq(b0, b1, b2, a0, a1, a2) {
  return { k: "bq", b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 };
}
function norm1p(b0, b1, a1) {
  return { k: "1p", b0, b1, a1 };
}

function coeffsBell(freq, gain_db, q) {
  const A = Math.pow(10, gain_db / 40);
  const w0 = 2 * Math.PI * freq / SR;
  const cw = Math.cos(w0), sw = Math.sin(w0);
  const alpha = sw / (2 * q);
  const a0u = 1 + alpha / A;
  const b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A, a1 = -2 * cw, a2 = 1 - alpha / A;
  return normBq(b0, b1, b2, a0u, a1, a2);
}
function coeffsLowShelf(freq, gain_db) {
  const A = Math.pow(10, gain_db / 40);
  const w0 = 2 * Math.PI * freq / SR;
  const cw = Math.cos(w0), sw = Math.sin(w0);
  const S = 1;
  const alpha = sw / 2 * Math.sqrt((A + 1 / A) * (1 / S - 1) + 2);
  const beta = Math.sqrt(A) / S;
  const b0 = A * ((A + 1) - (A - 1) * cw + beta * sw);
  const b1 = 2 * A * ((A - 1) - (A + 1) * cw);
  const b2 = A * ((A + 1) - (A - 1) * cw - beta * sw);
  const a0 = (A + 1) + (A - 1) * cw + beta * sw;
  const a1 = -2 * ((A - 1) + (A + 1) * cw);
  const a2 = (A + 1) + (A - 1) * cw - beta * sw;
  return normBq(b0, b1, b2, a0, a1, a2);
}
function coeffsHighShelf(freq, gain_db) {
  const A = Math.pow(10, gain_db / 40);
  const w0 = 2 * Math.PI * freq / SR;
  const cw = Math.cos(w0), sw = Math.sin(w0);
  const S = 1;
  const alpha = sw / 2 * Math.sqrt((A + 1 / A) * (1 / S - 1) + 2);
  const beta = Math.sqrt(A) / S;
  const b0 = A * ((A + 1) + (A - 1) * cw + beta * sw);
  const b1 = -2 * A * ((A - 1) + (A + 1) * cw);
  const b2 = A * ((A + 1) + (A - 1) * cw - beta * sw);
  const a0 = (A + 1) - (A - 1) * cw + beta * sw;
  const a1 = 2 * ((A - 1) - (A + 1) * cw);
  const a2 = (A + 1) - (A - 1) * cw - beta * sw;
  return normBq(b0, b1, b2, a0, a1, a2);
}
function coeffsNotch(freq, q) {
  const w0 = 2 * Math.PI * freq / SR;
  const cw = Math.cos(w0), sw = Math.sin(w0);
  const alpha = sw / (2 * q);
  return normBq(1, -2 * cw, 1, 1 + alpha, -2 * cw, 1 - alpha);
}
function coeffsLp2(freq) {
  const w0 = 2 * Math.PI * freq / SR;
  const cw = Math.cos(w0), sw = Math.sin(w0);
  const alpha = sw / (2 * LP_HP_Q);
  return normBq((1 - cw) * 0.5, 1 - cw, (1 - cw) * 0.5, 1 + alpha, -2 * cw, 1 - alpha);
}
function coeffsHp2(freq) {
  const w0 = 2 * Math.PI * freq / SR;
  const cw = Math.cos(w0), sw = Math.sin(w0);
  const alpha = sw / (2 * LP_HP_Q);
  return normBq((1 + cw) * 0.5, -(1 + cw), (1 + cw) * 0.5, 1 + alpha, -2 * cw, 1 - alpha);
}
function coeffsOnePoleLp(freq) {
  const dt = 1 / SR;
  const RC = 1 / (2 * Math.PI * freq);
  const alpha = dt / (RC + dt);
  return norm1p(alpha, 0, -(1 - alpha));
}
function coeffsOnePoleHp(freq) {
  const dt = 1 / SR;
  const RC = 1 / (2 * Math.PI * freq);
  const k = RC / (RC + dt);
  return norm1p(k, -k, -k);
}

function bandToStages(b0) {
  const b = normalizeBand(b0);
  const out = [];
  if (b.type === "bell") {
    if (Math.abs(b.gain_db) < BYPASS_DB) return out;
    out.push(coeffsBell(b.freq, b.gain_db, b.q));
    return out;
  }
  if (b.type === "low_shelf" || b.type === "high_shelf") {
    if (Math.abs(b.gain_db) < BYPASS_DB) return out;
    out.push(b.type === "low_shelf" ? coeffsLowShelf(b.freq, b.gain_db) : coeffsHighShelf(b.freq, b.gain_db));
    return out;
  }
  if (b.type === "notch") {
    out.push(coeffsNotch(b.freq, b.q));
    return out;
  }
  if (b.type === "low_pass") {
    const s = b.slope_db_oct;
    if (s === 6) out.push(coeffsOnePoleLp(b.freq));
    else if (s === 12) out.push(coeffsLp2(b.freq));
    else { out.push(coeffsLp2(b.freq)); out.push(coeffsLp2(b.freq)); }
    return out;
  }
  if (b.type === "high_pass") {
    const s = b.slope_db_oct;
    if (s === 6) out.push(coeffsOnePoleHp(b.freq));
    else if (s === 12) out.push(coeffsHp2(b.freq));
    else { out.push(coeffsHp2(b.freq)); out.push(coeffsHp2(b.freq)); }
    return out;
  }
  return out;
}

const bandStagesCache = new Map();
function bandToStagesMemo(b0) {
  const b = normalizeBand(b0);
  const key = `${b.type}|${b.freq}|${b.gain_db}|${b.q}|${b.slope_db_oct}`;
  let v = bandStagesCache.get(key);
  if (!v) {
    v = bandToStages(b);
    if (bandStagesCache.size > 200) bandStagesCache.clear();
    bandStagesCache.set(key, v);
  }
  return v;
}

const CURVE_POINTS = 128;

function magBq(st, w) {
  const cw = Math.cos(w), sw = Math.sin(w);
  const c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
  const nr = st.b0 + st.b1 * cw + st.b2 * c2;
  const ni = -st.b1 * sw - st.b2 * s2;
  const dr = 1 + st.a1 * cw + st.a2 * c2;
  const di = -st.a1 * sw - st.a2 * s2;
  const n = Math.hypot(nr, ni), d = Math.hypot(dr, di);
  return d > 1e-20 ? n / d : 1;
}
function mag1p(st, w) {
  const cw = Math.cos(w), sw = Math.sin(w);
  const nr = st.b0 + st.b1 * cw;
  const ni = -st.b1 * sw;
  const dr = 1 + st.a1 * cw;
  const di = -st.a1 * sw;
  const n = Math.hypot(nr, ni), d = Math.hypot(dr, di);
  return d > 1e-20 ? n / d : 1;
}
function stageMag(st, f) {
  const w = 2 * Math.PI * f / SR;
  return st.k === "1p" ? mag1p(st, w) : magBq(st, w);
}

function magLinearCurve(curve, f) {
  if (!curve.enabled) return 1;
  const n = clamp(curve.band_count || DEFAULT_MAX_BANDS, 1, MAX_STORAGE_BANDS);
  let mag = 1;
  for (let i = 0; i < n; i++) {
    const stages = bandToStagesMemo(curve.bands[i] || { freq: 1000, gain_db: 0, q: 1.2, type: "bell", slope_db_oct: 12 });
    for (const st of stages) mag *= stageMag(st, f);
  }
  return mag;
}

function magDb(curve, f) {
  return 20 * Math.log10(Math.max(magLinearCurve(curve, f), 1e-12));
}

function curvePath(curve, w, h) {
  const N = CURVE_POINTS;
  let d = "";
  for (let i = 0; i < N; i++) {
    const t = i / (N - 1);
    const f = Math.exp(logn(MIN_FREQ) + t * (logn(MAX_FREQ) - logn(MIN_FREQ)));
    const g = curve.enabled ? magDb(curve, f) : 0;
    const x = t * w;
    const y = gainToY(clamp(g, -DB_RANGE, DB_RANGE), h);
    d += (i === 0 ? "M" : "L") + x.toFixed(1) + "," + y.toFixed(1) + " ";
  }
  return d;
}

const TICK_FREQS = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
const TICK_LABELS = { 20: "20", 50: "50", 100: "100", 200: "200", 500: "500", 1000: "1k", 2000: "2k", 5000: "5k", 10000: "10k", 20000: "20k" };

function normalizeBands(src) {
  const defBand = { freq: 1000, gain_db: 0, q: 1.2, type: "bell", slope_db_oct: 12 };
  const bands = [];
  for (let i = 0; i < MAX_STORAGE_BANDS; i++) {
    bands.push(normalizeBand({ ...defBand, ...(src && src[i]) }));
  }
  return bands;
}

function normalizeCurve(curve, maxBands) {
  const bands = normalizeBands(curve.bands);
  const bands_r = curve.bands_r ? normalizeBands(curve.bands_r) : bands.map((b) => ({ ...b }));
  const cap = clamp(maxBands || DEFAULT_MAX_BANDS, 1, MAX_STORAGE_BANDS);
  return {
    enabled: !!curve.enabled,
    band_count: clamp(curve.band_count ?? cap, 1, cap),
    bands,
    bands_r,
    channel_mode: curve.channel_mode === "split" ? "split" : "ganged",
    supports_split: !!curve.supports_split,
  };
}

function EqPlot({ curve, curveR, activeCh, maxBands, onDrag, onDragEnd }) {
  const svgRef = useRef(null);
  const dragBand = useRef(null);
  const pendingDrag = useRef(null);
  const rafId = useRef(0);

  const activeN = clamp(curve.band_count || DEFAULT_MAX_BANDS, 1, maxBands);
  const pathL = useMemo(() => curvePath(curve, PLOT_W, PLOT_H), [curve]);
  const pathR = useMemo(() => curveR ? curvePath(curveR, PLOT_W, PLOT_H) : null, [curveR]);
  const handles = (activeCh === "R" && curveR) ? curveR : curve;
  const handleFill = (activeCh === "R" && curveR) ? "var(--eq-cal)" : "var(--eq-pref)";

  const eventToSvg = useCallback((e) => {
    const r = svgRef.current.getBoundingClientRect();
    const x = (e.clientX - r.left) * (PLOT_W / r.width);
    const y = (e.clientY - r.top) * (PLOT_H / r.height);
    return { x, y };
  }, []);

  function onPointerDown(idx, e) {
    if (idx >= activeN) return;
    e.preventDefault();
    dragBand.current = idx;
    e.target.setPointerCapture(e.pointerId);
  }
  function flushDrag() {
    rafId.current = 0;
    const p = pendingDrag.current;
    if (p == null) return;
    onDrag(p.idx, { freq: p.f, gain_db: p.g });
  }
  function onPointerMove(e) {
    if (dragBand.current == null) return;
    const { x, y } = eventToSvg(e);
    const f = clamp(Math.round(xToFreq(x, PLOT_W)), MIN_FREQ, MAX_FREQ);
    const g = clamp(+yToGain(y, PLOT_H).toFixed(2), MIN_GAIN, MAX_GAIN);
    pendingDrag.current = { idx: dragBand.current, f, g };
    if (!rafId.current) {
      rafId.current = requestAnimationFrame(flushDrag);
    }
  }
  function onPointerUp() {
    if (rafId.current) {
      cancelAnimationFrame(rafId.current);
      rafId.current = 0;
    }
    const p = pendingDrag.current;
    if (p != null) onDrag(p.idx, { freq: p.f, gain_db: p.g });
    pendingDrag.current = null;
    dragBand.current = null;
    onDragEnd?.();
  }

  return html`
    <svg ref=${svgRef} class="eq-plot" viewBox="0 0 ${PLOT_W} ${PLOT_H}" preserveAspectRatio="none"
         onPointerMove=${onPointerMove} onPointerUp=${onPointerUp} onPointerCancel=${onPointerUp}>
      <rect x="0" y="0" width=${PLOT_W} height=${PLOT_H} fill="var(--bg-2)" rx="10" />
      ${[-12, -6, 0, 6, 12].map((g) => html`
        <line x1="0" x2=${PLOT_W} y1=${gainToY(g, PLOT_H)} y2=${gainToY(g, PLOT_H)}
              stroke=${g === 0 ? "var(--line-strong)" : "var(--line)"} stroke-dasharray=${g === 0 ? "0" : "2 4"} />
        <text x="4" y=${gainToY(g, PLOT_H) - 2} fill="var(--fg-2)" font-size="10" font-family="var(--font-mono)">${g > 0 ? "+" : ""}${g} dB</text>
      `)}
      ${TICK_FREQS.map((f) => html`
        <line x1=${freqToX(f, PLOT_W)} x2=${freqToX(f, PLOT_W)} y1="0" y2=${PLOT_H} stroke="var(--line)" stroke-dasharray="2 4" />
        <text x=${freqToX(f, PLOT_W) + 3} y=${PLOT_H - 6} fill="var(--fg-2)" font-size="10" font-family="var(--font-mono)">${TICK_LABELS[f]}</text>
      `)}
      ${pathR ? html`<path d=${pathR} fill="none" stroke="var(--eq-cal)" stroke-width="2.2" opacity="0.85" />` : ""}
      <path d=${pathL} fill="none" stroke="var(--eq-pref)" stroke-width="2.2" />
      ${handles.bands.slice(0, activeN).map((b, i) => {
        const nb = normalizeBand(b);
        const cx = freqToX(nb.freq, PLOT_W);
        const cy = gainToY(clamp(nb.gain_db, -DB_RANGE, DB_RANGE), PLOT_H);
        return html`
          <circle key=${i} cx=${cx} cy=${cy} r="9" fill=${handleFill} stroke="var(--bg-1)" stroke-width="2"
                  style="cursor: grab; touch-action: none;" onPointerDown=${(e) => onPointerDown(i, e)}>
            <title>Band ${i + 1}: ${nb.type} ${formatHz(nb.freq)}, ${nb.gain_db.toFixed(1)} dB</title>
          </circle>`;
      })}
    </svg>`;
}

const FILTER_OPTIONS = [
  ["bell", "Bell"],
  ["low_shelf", "Low shelf"],
  ["high_shelf", "High shelf"],
  ["notch", "Notch"],
  ["low_pass", "Low-pass"],
  ["high_pass", "High-pass"],
];

function BandRow({ band, idx, onChange, idPrefix = "b", swatch = "var(--eq-pref)" }) {
  const b = normalizeBand(band);
  const freqPos = freqToPos(b.freq);
  const freqPct = (freqPos / FREQ_POS_MAX) * 100;
  const gainPct = pct(b.gain_db, MIN_GAIN, MAX_GAIN);
  const qMax = b.type === "notch" ? MAX_Q_NOTCH : MAX_Q;
  const qPct = pct(b.q, MIN_Q, qMax);
  const showGain = b.type !== "notch" && b.type !== "low_pass" && b.type !== "high_pass";
  const showQ = b.type !== "low_pass" && b.type !== "high_pass";
  const showSlope = b.type === "low_pass" || b.type === "high_pass";
  const fid = `${idPrefix}${idx}`;

  return html`
    <div class="eq-band">
      <div class="eq-band__head">
        <span class="eq-band__idx"><span style="color:${swatch}">●</span> Band ${idx + 1}</span>
        <span>${b.type} · ${formatHz(b.freq)}${showGain ? ` · ${b.gain_db >= 0 ? "+" : ""}${b.gain_db.toFixed(1)} dB` : ""}${showQ ? ` · Q ${b.q.toFixed(2)}` : ""}</span>
      </div>
      <div class="eq-band__row">
        <label>Type</label>
        <select class="btn" style="flex:1; min-height:2rem" value=${b.type}
                onChange=${(e) => onChange({ type: e.target.value, slope_db_oct: ["low_pass", "high_pass"].includes(e.target.value) ? b.slope_db_oct : 12 })}>
          ${FILTER_OPTIONS.map(([v, lab]) => html`<option value=${v} selected=${v === b.type}>${lab}</option>`)}
        </select>
      </div>
      ${showSlope ? html`
        <div class="eq-band__row">
          <label>Slope</label>
          <select class="btn" style="flex:1" value=${String(b.slope_db_oct)}
                  onChange=${(e) => onChange({ slope_db_oct: parseInt(e.target.value, 10) })}>
            <option value="6">6 dB/oct</option>
            <option value="12">12 dB/oct</option>
            <option value="24">24 dB/oct</option>
          </select>
        </div>` : ""}
      <div class="eq-band__row">
        <label for=${`${fid}-f`}>Freq</label>
        <input id=${`${fid}-f`} class="slider" type="range" min="0" max=${FREQ_POS_MAX} step="1" value=${freqPos}
               style=${`--fill:${freqPct}`}
               onInput=${(e) => onChange({ freq: posToFreq(parseInt(e.target.value, 10)) })} />
        <span class="chip">${formatHz(b.freq)}</span>
      </div>
      ${showGain ? html`
        <div class="eq-band__row">
          <label for=${`${fid}-g`}>Gain</label>
          <input id=${`${fid}-g`} class="slider" type="range" min=${MIN_GAIN} max=${MAX_GAIN} step="0.1" value=${b.gain_db}
                 style=${`--fill:${gainPct}`}
                 onInput=${(e) => onChange({ gain_db: clamp(parseFloat(e.target.value), MIN_GAIN, MAX_GAIN) })} />
          <span class="chip">${b.gain_db >= 0 ? "+" : ""}${b.gain_db.toFixed(1)} dB</span>
        </div>` : ""}
      ${showQ ? html`
        <div class="eq-band__row">
          <label for=${`${fid}-q`}>Q</label>
          <input id=${`${fid}-q`} class="slider" type="range" min=${MIN_Q} max=${qMax} step="0.05" value=${b.q}
                 style=${`--fill:${qPct}`}
                 onInput=${(e) => onChange({ q: clamp(parseFloat(e.target.value), MIN_Q, qMax) })} />
          <span class="chip">${b.q.toFixed(2)}</span>
        </div>` : ""}
    </div>`;
}

function Presets({ api, toast, activeSlot, onLoaded }) {
  const [list, setList] = useState(null);
  async function refresh() {
    try { setList(await api.getPresets()); } catch (e) { toast(String(e), "err"); }
  }
  useEffect(() => { refresh(); }, []);
  async function save(idx) {
    const name = prompt(`Name for slot ${idx + 1}:`, list?.slots?.[idx]?.name || `Preset ${idx + 1}`);
    if (name == null) return;
    try { await api.savePreset(idx, name.slice(0, 32)); toast("Preset saved", "ok"); await refresh(); }
    catch (e) { toast(String(e), "err"); }
  }
  async function load(idx) {
    try { await api.loadPreset(idx); toast("Preset loaded", "ok"); onLoaded?.(); await refresh(); }
    catch (e) { toast(String(e), "err"); }
  }
  async function rename(idx) {
    const name = prompt(`Rename slot ${idx + 1}:`, list?.slots?.[idx]?.name || "");
    if (!name) return;
    try { await api.renamePreset(idx, name.slice(0, 32)); await refresh(); } catch (e) { toast(String(e), "err"); }
  }
  async function del(idx) {
    if (!confirm(`Delete slot ${idx + 1}?`)) return;
    try { await api.deletePreset(idx); toast("Slot cleared", "ok"); await refresh(); } catch (e) { toast(String(e), "err"); }
  }
  if (!list) return html`<div class="stub">Loading presets…</div>`;
  return html`
    <div class="presets">
      ${list.slots.map((slot, i) => {
        const saved = !!slot.saved;
        const act = activeSlot === i;
        return html`
          <div key=${i} class=${"preset" + (act ? " preset--active" : "")}>
            <div class="preset__idx">${i + 1}</div>
            <div class="preset__body">
              <div class="preset__name">${saved ? slot.name : html`<span style="color:var(--fg-2)">empty</span>`}</div>
              ${act ? html`<span class="pill pill--ok">active</span>` : ""}
            </div>
            <div class="preset__actions">
              ${saved ? html`<button class="btn" onClick=${() => load(i)}>Load</button>` : ""}
              <button class="btn" onClick=${() => save(i)}>${saved ? "Overwrite" : "Save"}</button>
              ${saved ? html`<button class="btn" onClick=${() => rename(i)}>Rename</button>` : ""}
              ${saved ? html`<button class="btn btn--danger" onClick=${() => del(i)}>×</button>` : ""}
            </div>
          </div>`;
      })}
    </div>`;
}

function normalizeEqState(eq) {
  const maxBands = clamp(eq.max_bands ?? DEFAULT_MAX_BANDS, 1, MAX_STORAGE_BANDS);
  return {
    ...normalizeCurve(eq, maxBands),
    max_bands: maxBands,
    backend: eq.backend || "software",
    headroom: eq.headroom || { mode: "auto", manual_db: 0, auto_db: 0 },
    active_slot: eq.active_slot,
  };
}

function backendBadge(backend) {
  if (backend === "hardware") return html`<span class="pill pill--ok">HW PEQ</span>`;
  if (backend === "software (fallback)") return html`<span class="pill pill--warn">SW fallback</span>`;
  return html`<span class="pill">software</span>`;
}

export default function Dsp({ api, toast }) {
  const [state, setState] = useState(null);
  const [editCh, setEditCh] = useState("L");
  const commitT = useRef(null);

  // Subscribe but ONLY seed from the first frame: the page's optimistic
  // in-progress edits live in `state.eq` and we don't want a delayed SSE
  // snapshot (worst case 250 ms server debounce + 250 ms client debounce)
  // to clobber a band the user is currently dragging.
  useEffect(() => {
    let seeded = false;
    const stop = api.subscribeState((s) => {
      if (seeded) return;
      seeded = true;
      setState({ ...s, eq: normalizeEqState(s.eq || {}) });
    });
    return stop;
  }, []);

  // Force a snapshot re-seed after a deliberate user action (preset load) —
  // fetches once via REST and overwrites the local `eq` slice.
  async function reloadFromServer() {
    try {
      const s = await api.getState();
      setState((prev) => ({ ...prev, eq: normalizeEqState(s.eq || {}) }));
    } catch (e) { toast(String(e), "err"); }
  }

  if (!state || !state.eq) return html`<div class="stub">Loading EQ…</div>`;

  const eq = state.eq;
  const headroom = eq.headroom || { mode: "auto", manual_db: 0, auto_db: 0 };
  const maxBands = eq.max_bands;
  const split = eq.supports_split && eq.channel_mode === "split";

  function patchCurve(next) {
    setState((prev) => ({ ...prev, eq: { ...prev.eq, ...next } }));
  }

  function commitBody(overrides = {}) {
    const channel_mode = overrides.channel_mode ?? eq.channel_mode ?? "ganged";
    const payload = {
      enabled: overrides.enabled ?? eq.enabled,
      band_count: overrides.band_count ?? eq.band_count,
      channel_mode,
      bands: overrides.bands ?? eq.bands,
    };
    if (eq.supports_split && channel_mode === "split") {
      payload.bands_r = overrides.bands_r ?? eq.bands_r;
    }
    return payload;
  }

  function patchBand(idx, partial) {
    if (split && editCh === "R") {
      const bands_r = eq.bands_r.map((b, i) => i === idx ? normalizeBand({ ...b, ...partial }) : b);
      patchCurve({ bands_r });
      scheduleCommit(commitBody({ bands_r }));
      return;
    }
    const bands = eq.bands.map((b, i) => i === idx ? normalizeBand({ ...b, ...partial }) : b);
    patchCurve({ bands });
    scheduleCommit(commitBody({ bands }));
  }

  function setBandCount(n) {
    const v = clamp(n, 1, maxBands);
    patchCurve({ band_count: v });
    scheduleCommit(commitBody({ band_count: v }));
  }

  function scheduleCommit(payload) {
    clearTimeout(commitT.current);
    commitT.current = setTimeout(async () => {
      try {
        const r = await api.setEqCurve(payload);
        setState((prev) => ({
          ...prev,
          eq: {
            ...prev.eq,
            ...normalizeEqState({ ...r, max_bands: prev.eq.max_bands, backend: r.backend || prev.eq.backend }),
            headroom: r.headroom || prev.eq.headroom,
          },
        }));
      } catch (e) { toast(String(e), "err"); }
    }, 180);
  }

  function toggleEq() {
    const en = !eq.enabled;
    patchCurve({ enabled: en });
    scheduleCommit(commitBody({ enabled: en }));
  }

  function unlink() {
    const bands_r = eq.bands.map((b) => ({ ...b }));
    patchCurve({ channel_mode: "split", bands_r });
    setEditCh("L");
    scheduleCommit(commitBody({ channel_mode: "split", bands_r }));
  }

  function relink() {
    if (!confirm("Relink channels? Right will match Left.")) return;
    const bands_r = eq.bands.map((b) => ({ ...b }));
    patchCurve({ channel_mode: "ganged", bands_r });
    setEditCh("L");
    scheduleCommit(commitBody({ channel_mode: "ganged", bands: eq.bands }));
  }

  function copyLR() {
    const bands_r = eq.bands.map((b) => ({ ...b }));
    patchCurve({ bands_r });
    scheduleCommit(commitBody({ bands_r }));
  }

  function copyRL() {
    const bands = eq.bands_r.map((b) => ({ ...b }));
    patchCurve({ bands });
    scheduleCommit(commitBody({ bands }));
  }

  async function setHeadroomMode(mode) {
    try {
      const r = await api.setHeadroom({ mode, manual_db: headroom.manual_db });
      setState((prev) => ({ ...prev, eq: { ...prev.eq, headroom: r } }));
    } catch (e) { toast(String(e), "err"); }
  }
  async function setManualDb(db) {
    try {
      const r = await api.setHeadroom({ mode: "manual", manual_db: db });
      setState((prev) => ({ ...prev, eq: { ...prev.eq, headroom: r } }));
    } catch (e) { toast(String(e), "err"); }
  }

  const bc = clamp(eq.band_count || DEFAULT_MAX_BANDS, 1, maxBands);
  const editorBands = split && editCh === "R" ? eq.bands_r : eq.bands;
  const editorSwatch = split && editCh === "R" ? "var(--eq-cal)" : "var(--eq-pref)";
  const plotR = split ? { ...eq, bands: eq.bands_r } : null;
  const plotL = { ...eq, bands: eq.bands };

  return html`
    <h1 class="page__title">DSP / EQ</h1>
    <p class="page__subtitle">
      ${split ? "Independent left and right parametric curves" : "Single parametric cascade"}, up to ${maxBands} bands on this device.
      Filter types, per-band frequency / gain / Q, and slope on HP/LP filters.
      ${backendBadge(eq.backend)}
    </p>

    <div class="card" style="padding: var(--sp-4)">
      <${EqPlot} curve=${plotL} curveR=${plotR} activeCh=${split ? editCh : "L"} maxBands=${maxBands}
        onDrag=${(idx, partial) => patchBand(idx, partial)}
        onDragEnd=${() => {}} />
      ${split ? html`
        <div style="display:flex; gap: var(--sp-4); margin-top: var(--sp-2); font-size: 0.85rem; color: var(--fg-1)">
          <span><span style="color:var(--eq-pref)">●</span> Left</span>
          <span><span style="color:var(--eq-cal)">●</span> Right</span>
        </div>` : ""}
    </div>

    <div class=${"card" + (eq.enabled ? "" : " card--muted")}>
      <div style="display:flex; justify-content:space-between; align-items:center; gap: var(--sp-4); flex-wrap: wrap">
        <div>
          <div class="card__title">Parametric EQ</div>
          <p style="color: var(--fg-1); margin: var(--sp-2) 0 0;">
            Room correction and personal preference. Backend: ${eq.backend}.
          </p>
        </div>
        <div style="display:flex; align-items:center; gap: var(--sp-3); flex-wrap: wrap; justify-content: flex-end">
          ${eq.supports_split ? html`
            ${split ? html`
              <div class="pill-group">
                <button class=${"btn" + (editCh === "L" ? " btn--primary" : "")} onClick=${() => setEditCh("L")}>Left</button>
                <button class=${"btn" + (editCh === "R" ? " btn--primary" : "")} onClick=${() => setEditCh("R")}>Right</button>
              </div>
              <button class="btn" onClick=${copyLR}>Copy L→R</button>
              <button class="btn" onClick=${copyRL}>Copy R→L</button>
              <button class="btn" onClick=${relink}>Relink</button>
            ` : html`
              <button class="btn" onClick=${unlink}>Unlink L/R</button>
            `}
          ` : ""}
          <label style="display:flex; align-items:center; gap: var(--sp-3)">
            <span style="color:var(--fg-1)">Enabled</span>
            <span class="switch">
              <input type="checkbox" checked=${eq.enabled} onChange=${toggleEq} />
              <span class="switch__slider"></span>
            </span>
          </label>
        </div>
      </div>

      ${!eq.enabled ? html`
        <div class="banner banner--warn" style="margin-top: var(--sp-3)">
          <strong>EQ is off.</strong> Edits are saved but not applied until you enable the EQ.
          <button class="btn btn--primary" style="margin-left: var(--sp-3)" onClick=${toggleEq}>Enable now</button>
        </div>` : ""}

      <div class="eq-band__row" style="margin-top: var(--sp-3)">
        <label>Active bands</label>
        <input class="slider" type="range" min="1" max=${maxBands} step="1" value=${bc}
               style=${`--fill:${pct(bc, 1, maxBands)}`}
               onInput=${(e) => setBandCount(parseInt(e.target.value, 10))} />
        <span class="chip">${bc} / ${maxBands}</span>
      </div>

      <div class="eq-bands">
        ${editorBands.slice(0, bc).map((b, i) =>
          html`<${BandRow} key=${(split ? editCh : "G") + "-" + i} band=${b} idx=${i}
                           idPrefix=${split ? (editCh === "R" ? "br" : "bl") : "b"}
                           swatch=${editorSwatch}
                           onChange=${(p) => patchBand(i, p)} />`)}
      </div>
    </div>

    <div class="card">
      <div class="card__title">Headroom</div>
      <p style="color: var(--fg-1); margin: var(--sp-2) 0 var(--sp-3);">
        EQ can push above 0 dBFS. Auto headroom pre-attenuates from the measured peak (+ inter-sample margin on device).
      </p>
      <div style="display: flex; gap: var(--sp-2); margin-bottom: var(--sp-3);">
        <button class=${"btn" + (headroom.mode === "auto" ? " btn--primary" : "")} onClick=${() => setHeadroomMode("auto")}>Auto</button>
        <button class=${"btn" + (headroom.mode === "manual" ? " btn--primary" : "")} onClick=${() => setHeadroomMode("manual")}>Manual</button>
      </div>
      <dl class="kv" style="margin-bottom: var(--sp-3)">
        <dt>Auto attenuation</dt>
        <dd>${Number(headroom.auto_db || 0).toFixed(1)} dB ${headroom.auto_db < 0 ? "(EQ peaks above 0 dB, attenuating)" : "(no attenuation needed)"}</dd>
        <dt>Effective</dt>
        <dd>${(headroom.mode === "auto" ? (headroom.auto_db || 0) : headroom.manual_db).toFixed(1)} dB</dd>
      </dl>
      ${headroom.mode === "manual" ? html`
        <div class="field">
          <label class="field__label" for="hr-manual">Manual cap</label>
          <div class="row" style="gap: var(--sp-3)">
            <input id="hr-manual" class="slider" type="range" min="-24" max="0" step="0.5" value=${headroom.manual_db}
                   style=${`--fill:${pct(headroom.manual_db, -24, 0)}`}
                   onChange=${(e) => setManualDb(parseFloat(e.target.value))} />
            <span class="chip">${Number(headroom.manual_db).toFixed(1)} dB</span>
          </div>
        </div>` : ""}
    </div>

    <div class="card">
      <div class="card__title">Presets</div>
      <p style="color: var(--fg-1); margin: var(--sp-2) 0 var(--sp-3);">
        10 slots. Save/load the full EQ curve including band count, filter types,
        and on Louder both L/R curves. Presets round-trip across boards; loading a
        15-band Louder preset on Amped activates the first ${maxBands} bands (left only).
      </p>
      <${Presets} api=${api} toast=${toast}
                  activeSlot=${eq.active_slot < 10 ? eq.active_slot : null}
                  onLoaded=${reloadFromServer} />
    </div>
  `;
}
