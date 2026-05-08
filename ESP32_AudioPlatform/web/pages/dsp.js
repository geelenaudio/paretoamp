// DSP page: dual parametric EQ, headroom, presets. Preview mirrors `eq_coeffs.cpp`.

import { html, useEffect, useState, useRef, useMemo, useCallback } from "/vendor/htm-preact.mjs";

const SR = 44100;
const MIN_FREQ = 20, MAX_FREQ = 20000;
const MIN_GAIN = -18, MAX_GAIN = 18;
const MIN_Q = 0.1, MAX_Q = 10, MAX_Q_NOTCH = 100;
const BYPASS_DB = 0.01;
const LP_HP_Q = 0.70710678118;
const LAYERS = [
  { id: "calibration", label: "Calibration", blurb: "Corrects the room + speaker response. Normally set once and left alone." },
  { id: "preference", label: "Preference", blurb: "Your taste: bass boost, presence, whatever. Swap with presets." },
];
const LAYER_COLOR = { calibration: "var(--eq-cal)", preference: "var(--eq-pref)" };
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

/** @returns {Array<{k:string,b0?:number,b1?:number,b2?:number,a1?:number,a2?:number}>} */
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

function magLinearLayer(layer, f) {
  if (!layer.enabled) return 1;
  const n = clamp(layer.band_count || 10, 1, 10);
  let mag = 1;
  for (let i = 0; i < n; i++) {
    const stages = bandToStagesMemo(layer.bands[i] || { freq: 1000, gain_db: 0, q: 1.2, type: "bell", slope_db_oct: 12 });
    for (const st of stages) mag *= stageMag(st, f);
  }
  return mag;
}

function magDbCombined(layers, f) {
  let m = 1;
  for (const id of ["calibration", "preference"]) m *= magLinearLayer(layers[id], f);
  return 20 * Math.log10(Math.max(m, 1e-12));
}

function curvePathLayer(layer, w, h) {
  const N = CURVE_POINTS;
  let d = "";
  for (let i = 0; i < N; i++) {
    const t = i / (N - 1);
    const f = Math.exp(logn(MIN_FREQ) + t * (logn(MAX_FREQ) - logn(MIN_FREQ)));
    const g = layer.enabled ? magDbSingleLayer(layer, f) : 0;
    const x = t * w;
    const y = gainToY(clamp(g, -DB_RANGE, DB_RANGE), h);
    d += (i === 0 ? "M" : "L") + x.toFixed(1) + "," + y.toFixed(1) + " ";
  }
  return d;
}
function magDbSingleLayer(layer, f) {
  return 20 * Math.log10(Math.max(magLinearLayer(layer, f), 1e-12));
}

function curvePathCombined(layers, w, h) {
  const N = CURVE_POINTS;
  let d = "";
  for (let i = 0; i < N; i++) {
    const t = i / (N - 1);
    const f = Math.exp(logn(MIN_FREQ) + t * (logn(MAX_FREQ) - logn(MIN_FREQ)));
    const g = magDbCombined(layers, f);
    const x = t * w;
    const y = gainToY(clamp(g, -DB_RANGE, DB_RANGE), h);
    d += (i === 0 ? "M" : "L") + x.toFixed(1) + "," + y.toFixed(1) + " ";
  }
  return d;
}

const TICK_FREQS = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
const TICK_LABELS = { 20: "20", 50: "50", 100: "100", 200: "200", 500: "500", 1000: "1k", 2000: "2k", 5000: "5k", 10000: "10k", 20000: "20k" };

function normalizeLayerCurve(layer) {
  const bands = (layer.bands || []).map((b) => normalizeBand(b));
  return { ...layer, band_count: clamp(layer.band_count || 10, 1, 10), bands };
}

function EqPlot({ layers, activeLayer, onDrag, onDragEnd }) {
  const svgRef = useRef(null);
  const dragBand = useRef(null);
  const pendingDrag = useRef(null);
  const rafId = useRef(0);
  const L = useMemo(() => ({
    calibration: normalizeLayerCurve(layers.calibration),
    preference: normalizeLayerCurve(layers.preference),
  }), [layers]);

  const active = L[activeLayer];
  const activeN = clamp(active.band_count || 10, 1, 10);

  const combined = useMemo(() => curvePathCombined(L, PLOT_W, PLOT_H), [L]);
  const paths = useMemo(() => ({
    calibration: curvePathLayer(L.calibration, PLOT_W, PLOT_H),
    preference: curvePathLayer(L.preference, PLOT_W, PLOT_H),
  }), [L]);

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
      <path d=${paths.calibration} fill="none" stroke="var(--eq-cal)" stroke-width="1.3" opacity="0.55" />
      <path d=${paths.preference} fill="none" stroke="var(--eq-pref)" stroke-width="1.3" opacity="0.55" />
      <path d=${combined} fill="none" stroke="var(--accent)" stroke-width="2.2" />
      ${active.bands.slice(0, activeN).map((b, i) => {
        const nb = normalizeBand(b);
        const cx = freqToX(nb.freq, PLOT_W);
        const cy = gainToY(clamp(nb.gain_db, -DB_RANGE, DB_RANGE), PLOT_H);
        return html`
          <circle key=${i} cx=${cx} cy=${cy} r="9" fill=${LAYER_COLOR[activeLayer]} stroke="var(--bg-1)" stroke-width="2"
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

function BandRow({ band, idx, onChange, layerColor }) {
  const b = normalizeBand(band);
  const freqPos = freqToPos(b.freq);
  const freqPct = (freqPos / FREQ_POS_MAX) * 100;
  const gainPct = pct(b.gain_db, MIN_GAIN, MAX_GAIN);
  const qMax = b.type === "notch" ? MAX_Q_NOTCH : MAX_Q;
  const qPct = pct(b.q, MIN_Q, qMax);
  const showGain = b.type !== "notch" && b.type !== "low_pass" && b.type !== "high_pass";
  const showQ = b.type !== "low_pass" && b.type !== "high_pass";
  const showSlope = b.type === "low_pass" || b.type === "high_pass";

  return html`
    <div class="eq-band">
      <div class="eq-band__head">
        <span class="eq-band__idx"><span style=${`color:${layerColor}`}>●</span> Band ${idx + 1}</span>
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
        <label for=${`b${idx}-f`}>Freq</label>
        <input id=${`b${idx}-f`} class="slider" type="range" min="0" max=${FREQ_POS_MAX} step="1" value=${freqPos}
               style=${`--fill:${freqPct}`}
               onInput=${(e) => onChange({ freq: posToFreq(parseInt(e.target.value, 10)) })} />
        <span class="chip">${formatHz(b.freq)}</span>
      </div>
      ${showGain ? html`
        <div class="eq-band__row">
          <label for=${`b${idx}-g`}>Gain</label>
          <input id=${`b${idx}-g`} class="slider" type="range" min=${MIN_GAIN} max=${MAX_GAIN} step="0.1" value=${b.gain_db}
                 style=${`--fill:${gainPct}`}
                 onInput=${(e) => onChange({ gain_db: clamp(parseFloat(e.target.value), MIN_GAIN, MAX_GAIN) })} />
          <span class="chip">${b.gain_db >= 0 ? "+" : ""}${b.gain_db.toFixed(1)} dB</span>
        </div>` : ""}
      ${showQ ? html`
        <div class="eq-band__row">
          <label for=${`b${idx}-q`}>Q</label>
          <input id=${`b${idx}-q`} class="slider" type="range" min=${MIN_Q} max=${qMax} step="0.05" value=${b.q}
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
  const defBand = () => ({ freq: 1000, gain_db: 0, q: 1.2, type: "bell", slope_db_oct: 12 });
  const normCurve = (c) => {
    const bands = [];
    for (let i = 0; i < 10; i++) bands.push(normalizeBand({ ...defBand(), ...(c.bands && c.bands[i]) }));
    return { enabled: !!c.enabled, band_count: clamp(c.band_count ?? 10, 1, 10), bands };
  };
  return {
    calibration: normCurve(eq.calibration || {}),
    preference: normCurve(eq.preference || {}),
    headroom: eq.headroom || { mode: "auto", manual_db: 0, auto_db: 0 },
    active_slot: eq.active_slot,
  };
}

export default function Dsp({ api, toast }) {
  const [state, setState] = useState(null);
  const [active, setActive] = useState("preference");
  const commitT = useRef(null);

  // Subscribe but ONLY seed from the first frame: the page's optimistic
  // in-progress edits live in `state.eq` and we don't want a delayed SSE
  // snapshot (worst case 250 ms server debounce + 250 ms client debounce)
  // to clobber a band the user is currently dragging. Multi-tab EQ sync
  // would be a Phase 4b once we either move to JSON-Patch with per-key
  // resolution or expose a "user is editing" flag.
  useEffect(() => {
    let seeded = false;
    const stop = api.subscribeState((s) => {
      if (seeded) return;
      seeded = true;
      setState({ ...s, eq: normalizeEqState(s.eq || {}) });
    });
    return stop;
  }, []);

  if (!state || !state.eq) return html`<div class="stub">Loading EQ…</div>`;

  const eq = state.eq;
  const layers = { calibration: eq.calibration, preference: eq.preference };
  const headroom = eq.headroom || { mode: "auto", manual_db: 0, auto_db: 0 };

  function patchLayer(id, next) {
    setState((prev) => ({ ...prev, eq: { ...prev.eq, [id]: { ...prev.eq[id], ...next } } }));
  }

  function patchBand(idx, partial) {
    const layer = active;
    const cur = layers[layer];
    const bands = cur.bands.map((b, i) => i === idx ? normalizeBand({ ...b, ...partial }) : b);
    patchLayer(layer, { bands });
    scheduleCommit({ layer, enabled: cur.enabled, band_count: cur.band_count, bands });
  }

  function setBandCount(n) {
    const layer = active;
    const cur = layers[layer];
    const v = clamp(n, 1, 10);
    patchLayer(layer, { band_count: v });
    scheduleCommit({ layer, enabled: cur.enabled, band_count: v, bands: cur.bands });
  }

  function scheduleCommit(payload) {
    clearTimeout(commitT.current);
    commitT.current = setTimeout(async () => {
      try {
        const r = await api.setEqLayer(payload);
        const key = payload.layer;
        setState((prev) => ({
          ...prev,
          eq: {
            ...prev.eq,
            [key]: normalizeEqState({ [key]: r })[key],
            headroom: r.headroom || prev.eq.headroom,
          },
        }));
      } catch (e) { toast(String(e), "err"); }
    }, 180);
  }

  function toggleLayer(id) {
    const en = !layers[id].enabled;
    patchLayer(id, { enabled: en });
    scheduleCommit({
      layer: id,
      enabled: en,
      band_count: layers[id].band_count,
      bands: layers[id].bands,
    });
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

  const curLayer = layers[active];
  const bc = clamp(curLayer.band_count || 10, 1, 10);

  return html`
    <h1 class="page__title">DSP / EQ</h1>
    <p class="page__subtitle">
      Dual parametric cascades (calibration + preference). Choose filter types,
      active band count, and slopes for HP/LP. Preview follows the same math as firmware.
    </p>

    <div class="eq-layer-tabs">
      ${LAYERS.map((l) => html`
        <button key=${l.id} class=${"eq-tab" + (active === l.id ? " eq-tab--active" : "")} onClick=${() => setActive(l.id)}>
          ${l.label}
          <span class=${"pill " + (layers[l.id].enabled ? "pill--ok" : "")} style="margin-left: var(--sp-2)">
            ${layers[l.id].enabled ? "on" : "off"}
          </span>
        </button>
      `)}
    </div>

    <div class="card" style="padding: var(--sp-4)">
      <${EqPlot} layers=${layers} activeLayer=${active}
        onDrag=${(idx, partial) => patchBand(idx, partial)}
        onDragEnd=${() => {}} />
      <div class="eq-legend">
        <span><i style="background:#e6b566"></i> Calibration</span>
        <span><i style="background:#9d7cff"></i> Preference</span>
        <span><i style="background:var(--accent)"></i> Combined</span>
      </div>
    </div>

    <div class=${"card" + (curLayer.enabled ? "" : " card--muted")}>
      <div style="display:flex; justify-content:space-between; align-items:center; gap: var(--sp-4)">
        <div>
          <div class="card__title">${LAYERS.find((l) => l.id === active).label} layer</div>
          <p style="color: var(--fg-1); margin: var(--sp-2) 0 0;">${LAYERS.find((l) => l.id === active).blurb}</p>
        </div>
        <label style="display:flex; align-items:center; gap: var(--sp-3)">
          <span style="color:var(--fg-1)">Enabled</span>
          <span class="switch">
            <input type="checkbox" checked=${curLayer.enabled} onChange=${() => toggleLayer(active)} />
            <span class="switch__slider"></span>
          </span>
        </label>
      </div>

      ${!curLayer.enabled ? html`
        <div class="banner banner--warn" style="margin-top: var(--sp-3)">
          <strong>Layer is off.</strong> Edits are saved but not applied until you enable the layer.
          <button class="btn btn--primary" style="margin-left: var(--sp-3)" onClick=${() => toggleLayer(active)}>Enable now</button>
        </div>` : ""}

      <div class="eq-band__row" style="margin-top: var(--sp-3)">
        <label>Active bands</label>
        <input class="slider" type="range" min="1" max="10" step="1" value=${bc}
               style=${`--fill:${pct(bc, 1, 10)}`}
               onInput=${(e) => setBandCount(parseInt(e.target.value, 10))} />
        <span class="chip">${bc} / 10</span>
      </div>

      <div class="eq-bands">
        ${curLayer.bands.slice(0, bc).map((b, i) =>
          html`<${BandRow} key=${i} band=${b} idx=${i} layerColor=${LAYER_COLOR[active]}
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

    ${active === "preference" ? html`
      <div class="card">
        <div class="card__title">Preference presets</div>
        <p style="color: var(--fg-1); margin: var(--sp-2) 0 var(--sp-3);">
          10 slots. Save/load the preference curve (including band count and types).
        </p>
        <${Presets} api=${api} toast=${toast}
                    activeSlot=${eq.active_slot < 10 ? eq.active_slot : null}
                    onLoaded=${refresh} />
      </div>` : ""}
  `;
}
