// Audio test bench: signal generators + guided checks (POST /api/test-tone).

import { html, useEffect, useState, useRef } from "/vendor/htm-preact.mjs";

function clamp(v, lo, hi) {
  return Math.max(lo, Math.min(hi, v));
}

const MODE_LABELS = [
  "Sine",
  "Square",
  "Triangle",
  "Sawtooth",
  "White noise",
  "Pink noise",
  "Log sweep",
  "Channel check",
  "Polarity check",
  "Dual-tone (IMD)",
  "Silence",
];

const MODE_HELP = [
  "Pure tone. Use for frequency response, distortion, and hearing thresholds.",
  "Rich odd harmonics. Reveals amplifier ringing on edges. Above ~3 kHz you will hear aliasing; that is normal for a digital square.",
  "Gentler harmonics than square; good for checking amplifier linearity.",
  "Maximum harmonic energy; exercises bass driver excursion.",
  "Flat hiss. Use to chase noise floor and treble balance.",
  "Equal energy per octave — the usual reference for room and speaker tuning.",
  "Slow glide from low to high. Hunts rattles and room resonances.",
  "Short tone on left, then right, repeating. Confirms speaker wiring.",
  "Low tone switches between in-phase and anti-phase every 3 s. Use a stereo pair, not headphones.",
  "Two very high tones together. Any growl or beat in the midrange is intermodulation distortion from the amplifier.",
  "Digital zeros. Lets you hear hum or hiss from the amplifier with no signal.",
];

/** Presets for guided-test cards (merge with current UI numbers where omitted). */
const GUIDED_TESTS = [
  {
    id: "channel",
    title: "Channel check",
    checks: "Confirms the cable from Left goes to the left speaker and Right to the right.",
    listen: [
      "A short tone pings from the left speaker, then from the right, every two seconds.",
      "Sit centred between the speakers.",
    ],
    offWrong: [
      "Ping comes from the wrong side: swap the speaker cables at the amp or at the speakers.",
      "Only one side ever plays: check the Channels page for a muted channel, or a dead driver.",
    ],
    patch: { mode: 7, freq_hz: 600, amp_db: -20, pan: 0 },
  },
  {
    id: "polarity",
    title: "Polarity check",
    checks: "Both drivers should push air in the same direction at the same time.",
    listen: [
      "Use a stereo pair of speakers, not headphones.",
      "Every 3 s the tone switches between in-phase (focused, solid) and anti-phase (hollow, vague, wide).",
    ],
    offWrong: [
      "The two halves sound the same: one speaker may have + and − reversed.",
      "If Mono mixdown is on in Channels, anti-phase can cancel — disable mono to hear the spatial change.",
    ],
    patch: { mode: 8, freq_hz: 120, amp_db: -16, pan: 0 },
  },
  {
    id: "rattle",
    title: "Rattle hunt",
    checks: "Finds loose grilles, cabinet buzz, and sympathetic rattles in the room.",
    listen: [
      "The tone glides slowly from bass toward treble over half a minute.",
      "Sit still; a buzz that appears only at certain pitches is mechanical, not the driver.",
    ],
    offWrong: [
      "Note the pitch where it buzzes — low rattles are often panels or windows; mid rattles are often trim or shelf items.",
    ],
    patch: { mode: 6, freq_hz: 440, amp_db: -12, pan: 0, sweep_lo_hz: 20, sweep_hi_hz: 5000, sweep_s: 30 },
  },
  {
    id: "sub",
    title: "Sub-bass excursion",
    checks: "Whether the woofer can track deep bass without bottoming out.",
    listen: [
      "Watch the woofer cone if you can — smooth motion is good.",
      "Listen for click-thump or flapping at the bottom of the sweep.",
    ],
    offWrong: [
      "Hard clicks: lower the level by 6 dB or reduce bass EQ.",
      "Sustained distortion: port chuffing or worn surround.",
    ],
    patch: { mode: 6, freq_hz: 440, amp_db: -10, pan: 0, sweep_lo_hz: 20, sweep_hi_hz: 80, sweep_s: 20 },
  },
  {
    id: "tweeter",
    title: "Tweeter check",
    checks: "High-frequency drivers are alive and roughly matched.",
    listen: [
      "A thin steady tone at 8 kHz. Cup an ear toward each speaker in turn.",
    ],
    offWrong: [
      "Silence on one side at this pitch: blown tweeter is the most common failure.",
      "Crackle or scratch: damaged voice coil.",
    ],
    patch: { mode: 0, freq_hz: 8000, amp_db: -30, pan: 0 },
  },
  {
    id: "room",
    title: "Room mode finder",
    checks: "Where bass piles up or cancels in your room.",
    listen: [
      "Very slow sweep from 30 Hz to 200 Hz over one minute.",
      "Walk slowly around the room while it plays.",
    ],
    offWrong: [
      "Bass doubles in one corner and vanishes a few metres away: that is a room mode, not a fault.",
    ],
    patch: { mode: 6, freq_hz: 440, amp_db: -16, pan: 0, sweep_lo_hz: 30, sweep_hi_hz: 200, sweep_s: 60 },
  },
  {
    id: "phantom",
    title: "Phantom centre",
    checks: "Stereo imaging — a centred mono tone should appear between the speakers.",
    listen: [
      "Sit in the listening sweet spot. The 1 kHz tone should sound like one source between the speakers.",
    ],
    offWrong: [
      "Image pulled to one side: level mismatch (Channels) or polarity (run Polarity check).",
      "Image smeared or very wide: strong side-wall reflections.",
    ],
    patch: { mode: 0, freq_hz: 1000, amp_db: -20, pan: 0 },
  },
  {
    id: "hiss",
    title: "Hiss / noise floor",
    checks: "Noise the amplifier makes with no digital signal.",
    listen: [
      "Get within 30 cm of a tweeter. You should hear only quiet hiss or line hum.",
    ],
    offWrong: [
      "Ticking with Wi-Fi traffic: cable picking up RF.",
      "Loud hum at 50/100 Hz: ground loop — try another outlet or shorter signal path.",
    ],
    patch: { mode: 10, freq_hz: 440, amp_db: -60, pan: 0 },
  },
  {
    id: "pinkref",
    title: "Reference pink",
    checks: "Standard 1/f noise for measurement microphones (REW, Smaart, etc.).",
    listen: [
      "Even full-spectrum hiss — not tinny, not boomy, in a treated room.",
    ],
    offWrong: [
      "Heavy bass at the seat: room mode.",
      "Very thin: absorption eating mids near the speakers.",
    ],
    patch: { mode: 5, freq_hz: 440, amp_db: -20, pan: 0 },
  },
  {
    id: "imd",
    title: "IMD distortion check",
    checks: "Amplifier intermodulation — two high tones should not create a mid-range growl.",
    listen: [
      "You should hear only a thin high tone (or nothing if your hearing rolls off above ~16 kHz).",
      "You should not hear a low or mid tone beating underneath.",
    ],
    offWrong: [
      "Audible growl or 1 kHz beating: lower level until it disappears — that is the clean ceiling.",
    ],
    patch: { mode: 9, freq_hz: 19000, dual_f2_hz: 20000, amp_db: -12, pan: 0 },
  },
];

export default function TestTone({ api, toast }) {
  const [state, setState] = useState(null);
  const [freq, setFreq] = useState(440);
  const [amp, setAmp] = useState(-12);
  const [pan, setPan] = useState(0);
  const [mode, setMode] = useState(0);
  const [sweepS, setSweepS] = useState(10);
  const [sweepLo, setSweepLo] = useState(20);
  const [sweepHi, setSweepHi] = useState(20000);
  const [dualF2, setDualF2] = useState(19000);
  const [guidedRunning, setGuidedRunning] = useState(null);
  const [meters, setMeters] = useState({ pl: -90, pr: -90, rl: -90, rr: -90 });
  const pollRef = useRef(null);

  async function load() {
    try {
      const s = await api.getState();
      setState(s);
      const tt = s.test_tone || {};
      if (tt.freq_hz != null) setFreq(tt.freq_hz);
      if (tt.amp_db != null) setAmp(tt.amp_db);
      if (tt.pan != null) setPan(tt.pan);
      if (tt.mode != null) setMode(clamp(parseInt(String(tt.mode), 10) || 0, 0, 10));
      if (tt.sweep_s != null) setSweepS(tt.sweep_s);
      if (tt.sweep_lo_hz != null) setSweepLo(tt.sweep_lo_hz);
      if (tt.sweep_hi_hz != null) setSweepHi(tt.sweep_hi_hz);
      if (tt.dual_f2_hz != null) setDualF2(tt.dual_f2_hz);
    } catch (e) {
      toast(String(e), "err");
    }
  }

  useEffect(() => {
    load();
  }, []);

  useEffect(() => {
    if (!state || state.mode !== "test_tone") {
      if (pollRef.current) {
        clearInterval(pollRef.current);
        pollRef.current = null;
      }
      return;
    }
    const tick = async () => {
      try {
        const m = await api.getSystemMetrics();
        const o = m.audio && m.audio.out;
        if (o) {
          setMeters({
            pl: o.peak_l_db ?? -90,
            pr: o.peak_r_db ?? -90,
            rl: o.rms_l_db ?? -90,
            rr: o.rms_r_db ?? -90,
          });
        }
      } catch {
        /* ignore */
      }
    };
    tick();
    pollRef.current = setInterval(tick, 200);
    return () => {
      if (pollRef.current) clearInterval(pollRef.current);
      pollRef.current = null;
    };
  }, [state?.mode]);

  function basePatch() {
    return {
      freq_hz: freq,
      amp_db: amp,
      pan,
      mode,
      sweep_s: sweepS,
      sweep_lo_hz: sweepLo,
      sweep_hi_hz: sweepHi,
      dual_f2_hz: dualF2,
    };
  }

  async function apply(patch) {
    try {
      const r = await api.setTestTone({ ...basePatch(), ...patch });
      if (r.freq_hz != null) setFreq(r.freq_hz);
      if (r.amp_db != null) setAmp(r.amp_db);
      if (r.pan != null) setPan(r.pan);
      if (r.mode != null) setMode(clamp(parseInt(String(r.mode), 10) || 0, 0, 10));
      if (r.sweep_s != null) setSweepS(r.sweep_s);
      if (r.sweep_lo_hz != null) setSweepLo(r.sweep_lo_hz);
      if (r.sweep_hi_hz != null) setSweepHi(r.sweep_hi_hz);
      if (r.dual_f2_hz != null) setDualF2(r.dual_f2_hz);
      toast("Applied", "ok", 900);
    } catch (e) {
      toast(String(e), "err");
    }
  }

  async function runGuided(gt) {
    if (guidedRunning === gt.id) {
      setGuidedRunning(null);
      await apply({ mode: 10, amp_db: -60 });
      return;
    }
    setGuidedRunning(gt.id);
    await apply(gt.patch);
  }

  async function stopAll() {
    setGuidedRunning(null);
    await apply({ mode: 10, amp_db: -60 });
    toast("Output silenced", "ok", 1200);
  }

  if (!state) return html`<div class="stub">Loading…</div>`;

  const active = state.mode === "test_tone";
  const showFreq = [0, 1, 2, 3, 7, 8, 9].includes(mode);
  const showPan = [0, 1, 2, 3].includes(mode);
  const showSweep = mode === 6;
  const showDual = mode === 9;

  const dbToPct = (db) => clamp(((db + 60) / 60) * 100, 0, 100);

  const nowLabel = active
    ? `${MODE_LABELS[mode] || "?"} · ${Math.round(freq)} Hz · ${amp.toFixed(1)} dB`
    : "—";

  return html`
    <h1 class="page__title">Audio test bench</h1>
    <p class="page__subtitle">
      Signal generators and guided listening checks through the same DSP and amp as music.
      Select <strong>Test bench</strong> on the <a href="/mode">Mode</a> page first (device reboots).
    </p>

    ${!active
      ? html`<div class="banner banner--warn" style="margin-bottom: var(--sp-4)">
          Current mode is <code>${state.mode || "?"}</code>. Switch to Test bench under Mode to hear anything here.
        </div>`
      : ""}

    <div
      class="banner banner--warn"
      style="margin-bottom: var(--sp-4); border-left: 3px solid var(--accent-warn, #e8a849)"
    >
      <strong>Protect your ears and tweeters.</strong> Start around
      <strong>−30 dB</strong> and turn up slowly. Sweeps and pink noise can damage drivers at high level.
    </div>

    ${active
      ? html`
          <div class="card" style="margin-bottom: var(--sp-4); max-width: 640px">
            <div class="card__title">Output meters</div>
            <p class="field__hint" style="margin-top: 0; margin-bottom: var(--sp-3)">
              Peak and RMS after DSP (approx. dBFS). Useful for silence, noise, and sweeps.
            </p>
            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: var(--sp-4)">
              <div>
                <div class="field__label">Left</div>
                <div style="height: 8px; background: var(--surface-2); border-radius: 4px; overflow: hidden">
                  <div style="width: ${dbToPct(meters.pl)}%; height: 100%; background: var(--accent, #6ab0ff)"></div>
                </div>
                <div class="field__hint">Peak ${meters.pl.toFixed(1)} dB · RMS ${meters.rl.toFixed(1)} dB</div>
              </div>
              <div>
                <div class="field__label">Right</div>
                <div style="height: 8px; background: var(--surface-2); border-radius: 4px; overflow: hidden">
                  <div style="width: ${dbToPct(meters.pr)}%; height: 100%; background: var(--accent, #6ab0ff)"></div>
                </div>
                <div class="field__hint">Peak ${meters.pr.toFixed(1)} dB · RMS ${meters.rr.toFixed(1)} dB</div>
              </div>
            </div>
            <div class="field__hint" style="margin-top: var(--sp-2)">Now: ${nowLabel}</div>
          </div>
        `
      : ""}

    <div class="card" style="max-width: 720px; margin-bottom: var(--sp-5)">
      <div class="card__title">Signal generators</div>
      <p class="field__hint" style="margin-top: 0">
        Pick a waveform or noise source, tune level and pan, then apply. Square and saw alias above a few kHz — that is expected.
      </p>

      <div class="field">
        <label class="field__label">Mode</label>
        <select
          class="input"
          disabled=${!active}
          value=${String(mode)}
          onChange=${(e) => {
            const v = clamp(parseInt(e.target.value, 10) || 0, 0, 10);
            setMode(v);
            apply({
              freq_hz: freq,
              amp_db: amp,
              pan,
              mode: v,
              sweep_s: sweepS,
              sweep_lo_hz: sweepLo,
              sweep_hi_hz: sweepHi,
              dual_f2_hz: dualF2,
            });
          }}
        >
          ${MODE_LABELS.map(
            (lab, i) => html`<option value=${String(i)} title=${MODE_HELP[i]}>${i}. ${lab}</option>`
          )}
        </select>
      </div>
      <p class="field__hint" style="margin-bottom: var(--sp-3)">${MODE_HELP[mode] || ""}</p>

      ${showFreq
        ? html`<div class="field">
            <label class="field__label">Frequency (Hz)</label>
            <input
              class="input"
              type="number"
              min="20"
              max="20000"
              step="1"
              disabled=${!active}
              value=${Math.round(freq)}
              onInput=${(e) => setFreq(clamp(parseFloat(e.target.value) || 440, 20, 20000))}
              onBlur=${(e) => {
                const v = clamp(parseFloat(e.target.value) || 440, 20, 20000);
                setFreq(v);
                apply({ freq_hz: v });
              }}
            />
          </div>`
        : ""}

      ${showSweep
        ? html`
            <div class="field">
              <label class="field__label">Sweep low (Hz)</label>
              <input
                class="input"
                type="number"
                min="20"
                max="20000"
                disabled=${!active}
                value=${Math.round(sweepLo)}
                onInput=${(e) => setSweepLo(clamp(parseFloat(e.target.value) || 20, 20, 20000))}
                onBlur=${(e) => {
                  let v = clamp(parseFloat(e.target.value) || 20, 20, 20000);
                  if (v >= sweepHi) v = sweepHi - 10;
                  setSweepLo(v);
                  apply({ sweep_lo_hz: v });
                }}
              />
            </div>
            <div class="field">
              <label class="field__label">Sweep high (Hz)</label>
              <input
                class="input"
                type="number"
                min="20"
                max="20000"
                disabled=${!active}
                value=${Math.round(sweepHi)}
                onInput=${(e) => setSweepHi(clamp(parseFloat(e.target.value) || 20000, 20, 20000))}
                onBlur=${(e) => {
                  let v = clamp(parseFloat(e.target.value) || 20000, 20, 20000);
                  if (v <= sweepLo) v = sweepLo + 10;
                  setSweepHi(v);
                  apply({ sweep_hi_hz: v });
                }}
              />
            </div>
            <div class="field">
              <label class="field__label">Sweep duration (s)</label>
              <input
                class="input"
                type="number"
                min="1"
                max="60"
                step="1"
                disabled=${!active}
                value=${Math.round(sweepS)}
                onInput=${(e) => setSweepS(clamp(parseFloat(e.target.value) || 10, 1, 60))}
                onBlur=${(e) => {
                  const v = clamp(parseFloat(e.target.value) || 10, 1, 60);
                  setSweepS(v);
                  apply({ sweep_s: v });
                }}
              />
            </div>
          `
        : ""}

      ${showDual
        ? html`<div class="field">
            <label class="field__label">Second tone (Hz)</label>
            <input
              class="input"
              type="number"
              min="20"
              max="20000"
              disabled=${!active}
              value=${Math.round(dualF2)}
              onInput=${(e) => setDualF2(clamp(parseFloat(e.target.value) || 19000, 20, 20000))}
              onBlur=${(e) => {
                const v = clamp(parseFloat(e.target.value) || 19000, 20, 20000);
                setDualF2(v);
                apply({ dual_f2_hz: v });
              }}
            />
          </div>`
        : ""}

      <div class="field">
        <label class="field__label">Level (${amp.toFixed(1)} dBFS peak, approx.)</label>
        <input
          class="slider"
          type="range"
          min="-60"
          max="0"
          step="0.5"
          disabled=${!active}
          value=${amp}
          style=${`--fill:${((amp + 60) / 60) * 100}`}
          onInput=${(e) => {
            const v = clamp(parseFloat(e.target.value), -60, 0);
            setAmp(v);
          }}
          onChange=${(e) => {
            const v = clamp(parseFloat(e.target.value), -60, 0);
            setAmp(v);
            apply({ amp_db: v });
          }}
        />
      </div>

      ${showPan
        ? html`<div class="field">
            <label class="field__label">Pan (−1 = left, +1 = right)</label>
            <input
              class="slider"
              type="range"
              min="-1"
              max="1"
              step="0.01"
              disabled=${!active}
              value=${pan}
              style=${`--fill:${((pan + 1) / 2) * 100}`}
              onInput=${(e) => {
                const v = clamp(parseFloat(e.target.value), -1, 1);
                setPan(v);
              }}
              onChange=${(e) => {
                const v = clamp(parseFloat(e.target.value), -1, 1);
                setPan(v);
                apply({ pan: v });
              }}
            />
          </div>`
        : ""}
    </div>

    <h2 class="page__title" style="font-size: 1.15rem; margin-bottom: var(--sp-2)">Guided tests</h2>
    <p class="page__subtitle" style="margin-bottom: var(--sp-4)">
      One-click presets with listening notes. Each card explains what you should hear and what it means if you do not.
    </p>

    <div
      style="display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: var(--sp-4); margin-bottom: 5rem"
    >
      ${GUIDED_TESTS.map(
        (gt) => html`
          <div class="card" style="min-height: 100%">
            <div class="card__title">${gt.title}</div>
            <p class="field__hint" style="margin-top: 0"><strong>What it checks.</strong> ${gt.checks}</p>
            <p class="field__label" style="margin-bottom: var(--sp-1)">How to listen</p>
            <ul style="margin: 0 0 var(--sp-3) 1.1rem; padding: 0; font-size: 0.9rem; line-height: 1.45">
              ${gt.listen.map((t) => html`<li>${t}</li>`)}
            </ul>
            <p class="field__label" style="margin-bottom: var(--sp-1)">If something is off</p>
            <ul style="margin: 0 0 var(--sp-3) 1.1rem; padding: 0; font-size: 0.9rem; line-height: 1.45">
              ${gt.offWrong.map((t) => html`<li>${t}</li>`)}
            </ul>
            <button
              type="button"
              class="btn ${guidedRunning === gt.id ? "btn--primary" : ""}"
              disabled=${!active}
              onClick=${() => runGuided(gt)}
            >
              ${guidedRunning === gt.id ? "Stop this test" : "Run this test"}
            </button>
          </div>
        `
      )}
    </div>

    <button
      type="button"
      class="btn"
      style="position: fixed; right: 1rem; bottom: 1rem; z-index: 50; box-shadow: 0 4px 14px rgba(0,0,0,0.35)"
      disabled=${!active}
      onClick=${stopAll}
    >
      Stop output
    </button>
  `;
}
