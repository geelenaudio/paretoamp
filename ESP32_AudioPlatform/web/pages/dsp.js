// DSP page: dual 10-band parametric EQ, headroom, preset slots.
//
// Data model (one-to-one with /api/state.eq):
//   {
//     calibration : { enabled, bands: [{freq, gain_db, q} x 10] },
//     preference  : { enabled, bands: [...] },
//     headroom    : { mode: "auto"|"manual", manual_db, auto_db },
//     active_slot : 0..9 or 255 (unsaved)
//   }
//
// The UI has one big SVG plot of the combined frequency response plus
// overlayed per-layer curves. Ten handles on the *active* layer are
// draggable: drag X to change frequency, Y to change gain. Each band also
// has a numeric editor for Q plus exact freq/gain inputs.
//
// All band edits are debounced (180 ms) before the POST so a user dragging
// a handle doesn't hammer the device, but releases commit immediately.
//
// Biquad math below mirrors `src/audio_engine.cpp`'s `rebuildCascade` and
// `computeEqPeakGainDb` exactly so the preview curve matches what the
// device plays.

import { html, useEffect, useState, useRef, useMemo, useCallback } from "/vendor/htm-preact.mjs";

const SR = 44100;
const MIN_FREQ = 20, MAX_FREQ = 20000;
const MIN_GAIN = -18, MAX_GAIN = 18;
const MIN_Q = 0.1, MAX_Q = 10;
const LAYERS = [
  { id: "calibration", label: "Calibration", blurb: "Corrects the room + speaker response. Normally set once and left alone." },
  { id: "preference",  label: "Preference",  blurb: "Your taste: bass boost, presence, whatever. Swap with presets." },
];
const LAYER_COLOR = { calibration: "var(--eq-cal)", preference: "var(--eq-pref)" };
const PLOT_W = 860;
const PLOT_H = 280;
const DB_RANGE = 20;            // ±20 dB window, same as ±18 band limit with headroom

// Freq sliders use a 0..1000 "position" axis mapped through a log transform so
// the thumb resolution is uniform across 20 Hz..20 kHz (stepping one position
// unit near 20 Hz is ~0.09 Hz, near 20 kHz ~90 Hz — useful both ends).
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

// Peaking-EQ coefficients (RBJ). Identical to firmware.
function bandCoeffs({ freq, gain_db, q }) {
  const A = Math.pow(10, gain_db / 40);
  const w0 = 2 * Math.PI * freq / SR;
  const cw = Math.cos(w0), sw = Math.sin(w0);
  const alpha = sw / (2 * q);
  const a0 = 1 + alpha / A;
  return {
    b0: (1 + alpha * A) / a0,
    b1: (-2 * cw) / a0,
    b2: (1 - alpha * A) / a0,
    a1: (-2 * cw) / a0,
    a2: (1 - alpha / A) / a0,
  };
}

function magDb(bands, f) {
  const w = 2 * Math.PI * f / SR;
  const cw = Math.cos(w), sw = Math.sin(w);
  const c2 = Math.cos(2 * w), s2 = Math.sin(2 * w);
  let mag = 1;
  for (const b of bands) {
    if (Math.abs(b.gain_db) < 0.01) continue; // identity band
    const { b0, b1, b2, a1, a2 } = bandCoeffs(b);
    const nr = b0 + b1 * cw + b2 * c2;
    const ni = -b1 * sw - b2 * s2;
    const dr = 1 + a1 * cw + a2 * c2;
    const di = -a1 * sw - a2 * s2;
    const n = Math.hypot(nr, ni);
    const d = Math.hypot(dr, di);
    if (d > 1e-12) mag *= n / d;
  }
  return 20 * Math.log10(mag);
}

function curvePath(bands, w, h) {
  const N = 256;
  let d = "";
  for (let i = 0; i < N; i++) {
    const t = i / (N - 1);
    const f = Math.exp(logn(MIN_FREQ) + t * (logn(MAX_FREQ) - logn(MIN_FREQ)));
    const g = magDb(bands, f);
    const x = t * w;
    const y = gainToY(clamp(g, -DB_RANGE, DB_RANGE), h);
    d += (i === 0 ? "M" : "L") + x.toFixed(1) + "," + y.toFixed(1) + " ";
  }
  return d;
}

// Decade ticks on a log axis. Show labels at 20/50/100/200/500/1k/2k/5k/10k/20k.
const TICK_FREQS = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
const TICK_LABELS = { 20: "20", 50: "50", 100: "100", 200: "200", 500: "500", 1000: "1k", 2000: "2k", 5000: "5k", 10000: "10k", 20000: "20k" };

function EqPlot({ layers, activeLayer, onDrag, onDragEnd }) {
  const svgRef = useRef(null);
  const dragBand = useRef(null);

  const active = layers[activeLayer];
  const combined = useMemo(() => {
    const all = [];
    for (const l of Object.values(layers)) if (l.enabled) all.push(...l.bands);
    return curvePath(all, PLOT_W, PLOT_H);
  }, [layers]);
  const paths = useMemo(() => {
    const out = {};
    for (const [id, l] of Object.entries(layers)) {
      out[id] = curvePath(l.bands, PLOT_W, PLOT_H);
    }
    return out;
  }, [layers]);

  const eventToSvg = useCallback((e) => {
    const r = svgRef.current.getBoundingClientRect();
    const x = (e.clientX - r.left) * (PLOT_W / r.width);
    const y = (e.clientY - r.top) * (PLOT_H / r.height);
    return { x, y };
  }, []);

  function onPointerDown(idx, e) {
    e.preventDefault();
    dragBand.current = idx;
    e.target.setPointerCapture(e.pointerId);
  }
  function onPointerMove(e) {
    if (dragBand.current == null) return;
    const { x, y } = eventToSvg(e);
    const f = clamp(Math.round(xToFreq(x, PLOT_W)), MIN_FREQ, MAX_FREQ);
    const g = clamp(+yToGain(y, PLOT_H).toFixed(2), MIN_GAIN, MAX_GAIN);
    onDrag(dragBand.current, { freq: f, gain_db: g });
  }
  function onPointerUp(e) {
    if (dragBand.current == null) return;
    const idx = dragBand.current;
    dragBand.current = null;
    onDragEnd(idx);
  }

  return html`
    <svg
      ref=${svgRef}
      class="eq-plot"
      viewBox="0 0 ${PLOT_W} ${PLOT_H}"
      preserveAspectRatio="none"
      onPointerMove=${onPointerMove}
      onPointerUp=${onPointerUp}
      onPointerCancel=${onPointerUp}
    >
      <rect x="0" y="0" width=${PLOT_W} height=${PLOT_H} fill="var(--bg-2)" rx="10" />

      ${[-12, -6, 0, 6, 12].map((g) => html`
        <line x1="0" x2=${PLOT_W} y1=${gainToY(g, PLOT_H)} y2=${gainToY(g, PLOT_H)}
              stroke=${g === 0 ? "var(--line-strong)" : "var(--line)"}
              stroke-dasharray=${g === 0 ? "0" : "2 4"} />
        <text x="4" y=${gainToY(g, PLOT_H) - 2}
              fill="var(--fg-2)" font-size="10" font-family="var(--font-mono)">${g > 0 ? "+" : ""}${g} dB</text>
      `)}
      ${TICK_FREQS.map((f) => html`
        <line x1=${freqToX(f, PLOT_W)} x2=${freqToX(f, PLOT_W)} y1="0" y2=${PLOT_H}
              stroke="var(--line)" stroke-dasharray="2 4" />
        <text x=${freqToX(f, PLOT_W) + 3} y=${PLOT_H - 6}
              fill="var(--fg-2)" font-size="10" font-family="var(--font-mono)">${TICK_LABELS[f]}</text>
      `)}

      <path d=${paths.calibration} fill="none" stroke="var(--eq-cal)"  stroke-width="1.3" opacity="0.55" />
      <path d=${paths.preference}  fill="none" stroke="var(--eq-pref)" stroke-width="1.3" opacity="0.55" />
      <path d=${combined}          fill="none" stroke="var(--accent)"  stroke-width="2.2" />

      ${active.bands.map((b, i) => {
        const cx = freqToX(b.freq, PLOT_W);
        const cy = gainToY(clamp(b.gain_db, -DB_RANGE, DB_RANGE), PLOT_H);
        const col = LAYER_COLOR[activeLayer];
        return html`
          <circle key=${i}
                  cx=${cx} cy=${cy} r="9"
                  fill=${col} stroke="var(--bg-1)" stroke-width="2"
                  style="cursor: grab; touch-action: none;"
                  onPointerDown=${(e) => onPointerDown(i, e)}>
            <title>Band ${i + 1}: ${formatHz(b.freq)}, ${b.gain_db.toFixed(1)} dB, Q ${b.q.toFixed(2)}</title>
          </circle>
        `;
      })}
    </svg>
  `;
}

// One band: three sliders (log-scaled Hz, linear dB, linear Q) + a value chip
// and a -/+ nudge pair for fine edits. Sliders fire onInput at ~60 Hz so the
// parent debounces via scheduleCommit(); nudges fire onClick (single commit).
function BandRow({ band, idx, onChange, layerColor }) {
  const freqPos = freqToPos(band.freq);
  const freqPct = (freqPos / FREQ_POS_MAX) * 100;
  const gainPct = pct(band.gain_db, MIN_GAIN, MAX_GAIN);
  const qPct    = pct(band.q,       MIN_Q,    MAX_Q);

  const nudgeFreq = (dir) => {
    // Nudge by one log-position step (~0.1 % of the axis).
    const step = Math.max(1, Math.round(FREQ_POS_MAX * 0.005));
    onChange({ freq: posToFreq(clamp(freqPos + dir * step, 0, FREQ_POS_MAX)) });
  };
  const nudgeGain = (dir) => onChange({ gain_db: clamp(+(band.gain_db + dir * 0.5).toFixed(1), MIN_GAIN, MAX_GAIN) });
  const nudgeQ    = (dir) => onChange({ q:       clamp(+(band.q       + dir * 0.1).toFixed(2), MIN_Q,    MAX_Q) });

  return html`
    <div class="eq-band">
      <div class="eq-band__head">
        <span class="eq-band__idx">
          <span style=${`color:${layerColor}`}>●</span> Band ${idx + 1}
        </span>
        <span>${formatHz(band.freq)} · ${band.gain_db >= 0 ? "+" : ""}${band.gain_db.toFixed(1)} dB · Q ${band.q.toFixed(2)}</span>
      </div>

      <div class="eq-band__row">
        <label for=${`b${idx}-f`}>Freq</label>
        <input id=${`b${idx}-f`} class="slider" type="range"
               min="0" max=${FREQ_POS_MAX} step="1"
               value=${freqPos}
               style=${`--fill:${freqPct}`}
               onInput=${(e) => onChange({ freq: posToFreq(parseInt(e.target.value, 10)) })} />
        <span class="chip">${formatHz(band.freq)}</span>
        <span class="eq-band__nudge">
          <button type="button" class="btn" onClick=${() => nudgeFreq(-1)} aria-label="Lower freq">−</button>
          <button type="button" class="btn" onClick=${() => nudgeFreq(+1)} aria-label="Raise freq">+</button>
        </span>
      </div>

      <div class="eq-band__row">
        <label for=${`b${idx}-g`}>Gain</label>
        <input id=${`b${idx}-g`} class="slider" type="range"
               min=${MIN_GAIN} max=${MAX_GAIN} step="0.1"
               value=${band.gain_db}
               style=${`--fill:${gainPct}`}
               onInput=${(e) => onChange({ gain_db: clamp(parseFloat(e.target.value), MIN_GAIN, MAX_GAIN) })} />
        <span class="chip">${band.gain_db >= 0 ? "+" : ""}${band.gain_db.toFixed(1)} dB</span>
        <span class="eq-band__nudge">
          <button type="button" class="btn" onClick=${() => nudgeGain(-1)} aria-label="Lower gain">−</button>
          <button type="button" class="btn" onClick=${() => nudgeGain(+1)} aria-label="Raise gain">+</button>
        </span>
      </div>

      <div class="eq-band__row">
        <label for=${`b${idx}-q`}>Q</label>
        <input id=${`b${idx}-q`} class="slider" type="range"
               min=${MIN_Q} max=${MAX_Q} step="0.05"
               value=${band.q}
               style=${`--fill:${qPct}`}
               onInput=${(e) => onChange({ q: clamp(parseFloat(e.target.value), MIN_Q, MAX_Q) })} />
        <span class="chip">${band.q.toFixed(2)}</span>
        <span class="eq-band__nudge">
          <button type="button" class="btn" onClick=${() => nudgeQ(-1)} aria-label="Lower Q">−</button>
          <button type="button" class="btn" onClick=${() => nudgeQ(+1)} aria-label="Raise Q">+</button>
        </span>
      </div>
    </div>
  `;
}

function Presets({ api, toast, activeSlot, onLoaded }) {
  const [list, setList] = useState(null);

  async function refresh() {
    try { setList(await api.getPresets()); }
    catch (e) { toast(String(e), "err"); }
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
    try { await api.renamePreset(idx, name.slice(0, 32)); await refresh(); }
    catch (e) { toast(String(e), "err"); }
  }
  async function del(idx) {
    if (!confirm(`Delete slot ${idx + 1}?`)) return;
    try { await api.deletePreset(idx); toast("Slot cleared", "ok"); await refresh(); }
    catch (e) { toast(String(e), "err"); }
  }

  if (!list) return html`<div class="stub">Loading presets…</div>`;

  return html`
    <div class="presets">
      ${list.slots.map((slot, i) => {
        const saved = !!slot.saved;
        const active = activeSlot === i;
        return html`
          <div key=${i} class=${"preset" + (active ? " preset--active" : "")}>
            <div class="preset__idx">${i + 1}</div>
            <div class="preset__body">
              <div class="preset__name">${saved ? slot.name : html`<span style="color:var(--fg-2)">empty</span>`}</div>
              ${active ? html`<span class="pill pill--ok">active</span>` : ""}
            </div>
            <div class="preset__actions">
              ${saved ? html`<button class="btn" onClick=${() => load(i)}>Load</button>` : ""}
              <button class="btn" onClick=${() => save(i)}>${saved ? "Overwrite" : "Save"}</button>
              ${saved ? html`<button class="btn" onClick=${() => rename(i)}>Rename</button>` : ""}
              ${saved ? html`<button class="btn btn--danger" onClick=${() => del(i)}>×</button>` : ""}
            </div>
          </div>
        `;
      })}
    </div>
  `;
}

export default function Dsp({ api, toast }) {
  const [state, setState] = useState(null);
  const [active, setActive] = useState("preference");
  const commitT = useRef(null);

  async function refresh() {
    try {
      const s = await api.getState();
      setState(s);
    } catch (e) { toast(String(e), "err"); }
  }
  useEffect(() => { refresh(); }, []);

  if (!state || !state.eq) return html`<div class="stub">Loading EQ…</div>`;

  const eq = state.eq;
  const layers = { calibration: eq.calibration, preference: eq.preference };
  const headroom = eq.headroom || { mode: "auto", manual_db: 0, auto_db: 0 };

  function patchLayer(id, next) {
    setState((prev) => ({
      ...prev,
      eq: { ...prev.eq, [id]: { ...prev.eq[id], ...next } },
    }));
  }

  function patchBand(idx, partial) {
    const layer = active;
    const bands = layers[layer].bands.map((b, i) => i === idx ? { ...b, ...partial } : b);
    patchLayer(layer, { bands });
    scheduleCommit({ layer, enabled: layers[layer].enabled, bands });
  }

  function scheduleCommit(payload) {
    clearTimeout(commitT.current);
    commitT.current = setTimeout(async () => {
      try {
        const r = await api.setEqLayer(payload);
        setState((prev) => ({
          ...prev,
          eq: { ...prev.eq, [payload.layer]: { enabled: r.enabled, bands: r.bands }, headroom: r.headroom || prev.eq.headroom },
        }));
      } catch (e) { toast(String(e), "err"); }
    }, 180);
  }

  function toggleLayer(id) {
    const en = !layers[id].enabled;
    patchLayer(id, { enabled: en });
    scheduleCommit({ layer: id, enabled: en, bands: layers[id].bands });
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

  return html`
    <h1 class="page__title">DSP / EQ</h1>
    <p class="page__subtitle">
      Dual 10-band parametric cascades. Drag the coloured handles or edit
      the numbers below to shape the response.
    </p>

    <div class="eq-layer-tabs">
      ${LAYERS.map((l) => html`
        <button key=${l.id}
                class=${"eq-tab" + (active === l.id ? " eq-tab--active" : "")}
                onClick=${() => setActive(l.id)}>
          ${l.label}
          <span class=${"pill " + (layers[l.id].enabled ? "pill--ok" : "")}
                style="margin-left: var(--sp-2)">
            ${layers[l.id].enabled ? "on" : "off"}
          </span>
        </button>
      `)}
    </div>

    <div class="card" style="padding: var(--sp-4)">
      <${EqPlot}
        layers=${layers}
        activeLayer=${active}
        onDrag=${(idx, partial) => patchBand(idx, partial)}
        onDragEnd=${() => { /* final commit already debounced */ }}
      />
      <div class="eq-legend">
        <span><i style="background:#e6b566"></i> Calibration</span>
        <span><i style="background:#9d7cff"></i> Preference</span>
        <span><i style="background:var(--accent)"></i> Combined</span>
      </div>
    </div>

    <div class=${"card" + (layers[active].enabled ? "" : " card--muted")}>
      <div style="display:flex; justify-content:space-between; align-items:center; gap: var(--sp-4)">
        <div>
          <div class="card__title">${LAYERS.find((l) => l.id === active).label} layer</div>
          <p style="color: var(--fg-1); margin: var(--sp-2) 0 0;">
            ${LAYERS.find((l) => l.id === active).blurb}
          </p>
        </div>
        <label style="display:flex; align-items:center; gap: var(--sp-3)">
          <span style="color:var(--fg-1)">Enabled</span>
          <span class="switch">
            <input type="checkbox"
                   checked=${layers[active].enabled}
                   onChange=${() => toggleLayer(active)} />
            <span class="switch__slider"></span>
          </span>
        </label>
      </div>

      ${layers[active].enabled ? "" : html`
        <div class="banner banner--warn" style="margin-top: var(--sp-3)">
          <strong>Layer is off.</strong>
          Band edits below are being saved, but they will not affect the
          audio until you flip the <em>Enabled</em> switch above.
          <button class="btn btn--primary" style="margin-left: var(--sp-3)"
                  onClick=${() => toggleLayer(active)}>Enable now</button>
        </div>`}

      <div class="eq-bands">
        ${layers[active].bands.map((b, i) =>
          html`<${BandRow} key=${i} band=${b} idx=${i}
                           layerColor=${LAYER_COLOR[active]}
                           onChange=${(p) => patchBand(i, p)} />`)}
      </div>
    </div>

    <div class="card">
      <div class="card__title">Headroom</div>
      <p style="color: var(--fg-1); margin: var(--sp-2) 0 var(--sp-3);">
        Parametric EQ gain can push the signal above 0 dBFS → digital
        clipping. Headroom management automatically pre-attenuates to
        keep the peak at or below unity, or you can cap it yourself.
      </p>

      <div style="display: flex; gap: var(--sp-2); margin-bottom: var(--sp-3);">
        <button class=${"btn" + (headroom.mode === "auto" ? " btn--primary" : "")}
                onClick=${() => setHeadroomMode("auto")}>Auto</button>
        <button class=${"btn" + (headroom.mode === "manual" ? " btn--primary" : "")}
                onClick=${() => setHeadroomMode("manual")}>Manual</button>
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
            <input id="hr-manual" class="slider" type="range"
                   min="-24" max="0" step="0.5"
                   value=${headroom.manual_db}
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
          10 slots. Save the current preference curve, or load a stored one
          to replace it. The active slot is highlighted; editing any band
          marks the live curve as <em>unsaved</em>.
        </p>
        <${Presets} api=${api} toast=${toast}
                    activeSlot=${eq.active_slot < 10 ? eq.active_slot : null}
                    onLoaded=${refresh} />
      </div>` : ""}
  `;
}
