// Audio page: master volume, per-channel balance, stereo/mono, channel on-off.
//
// The firmware actually tracks per-channel gain (vol_l / vol_r), a boolean
// mute, left/right channel enables, and a stereo vs mono flag. The UI
// mirrors that by giving the user two rendering choices:
//
//   Ganged slider  : one big knob that moves L and R together, plus a
//                    balance slider below (-100..+100 => all-left..all-right
//                    trim applied on top of the master). Simple, matches
//                    the "single volume" mental model.
//   Split sliders  : two sliders, L and R independent. The saved config is
//                    still per-channel under the hood, so switching between
//                    ganged and split is just a UI concern — no round-trip
//                    to the server.
//
// Debouncing: the sliders fire `input` at ~60 Hz while dragging. We commit
// on `change` (mouse-up) only, to avoid hammering the device with HTTP
// requests. A tiny 120 ms trailing-edge timer picks up the last value if
// the user lets go between frames on a flaky browser.

import { html, useEffect, useState, useRef } from "/vendor/htm-preact.mjs";

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

// Given a master level and a balance (-100..+100), compute per-channel
// levels. Balance -100 means "full left, silent right"; +100 is the
// mirror. Centre is 0.
function gangedToLR(master, balance) {
  const b = clamp(balance, -100, 100) / 100; // -1..+1
  const l = master * clamp(1 - Math.max(0, b), 0, 1);
  const r = master * clamp(1 - Math.max(0, -b), 0, 1);
  return [Math.round(l), Math.round(r)];
}

// Inverse mapping: pick a reasonable master + balance from an (l, r) pair
// so the ganged view has sensible defaults when the user has persisted
// split values.
function lrToGanged(l, r) {
  const master = Math.max(l, r);
  if (master === 0) return [0, 0];
  const balance = Math.round(((l - r) / master) * -100);
  return [master, clamp(balance, -100, 100)];
}

export default function Audio({ api, toast }) {
  const [state, setState] = useState(null);
  const [mode, setMode] = useState("ganged"); // "ganged" | "split"

  // Persistent (server-side) values.
  const [l, setL] = useState(80);
  const [r, setR] = useState(80);
  const [muted, setMuted] = useState(false);
  const [lon, setLon] = useState(true);
  const [ron, setRon] = useState(true);
  const [stereo, setStereo] = useState(true);

  // Derived ganged view.
  const [master, setMaster] = useState(80);
  const [balance, setBalance] = useState(0);

  // Debounce timer for trailing-edge commit.
  const commitT = useRef(null);

  useEffect(() => {
    // Seed once from the first push-based snapshot, then ignore subsequent
    // frames so a slider drag isn't clobbered by an in-flight SSE update
    // (the server-side debounce is 250 ms; the UI updates per-frame). The
    // SSE channel still keeps `state` fresh for non-interactive fields like
    // the mode badge in the page subtitle, which is why we keep updating
    // the snapshot ref but only seed L/R/mute/channels on the very first
    // delivery.
    let seeded = false;
    const stop = api.subscribeState((s) => {
      setState(s);
      if (seeded) return;
      seeded = true;
      const ll = s.volume?.l ?? 80;
      const rr = s.volume?.r ?? 80;
      setL(ll); setR(rr);
      setMuted(!!s.volume?.muted);
      setLon(s.channels?.l_on !== false);
      setRon(s.channels?.r_on !== false);
      setStereo(s.channels?.stereo_mono !== "mono");
      const [m, b] = lrToGanged(ll, rr);
      setMaster(m); setBalance(b);
      // If the split is significant (>2 pct), the user is clearly using
      // the per-channel view; default to split mode so they're not
      // surprised.
      if (Math.abs(ll - rr) > 2) setMode("split");
    });
    return stop;
  }, []);

  async function commitVolume(nextL, nextR, nextMuted) {
    clearTimeout(commitT.current);
    commitT.current = setTimeout(async () => {
      try { await api.setVolume(nextL, nextR, nextMuted); }
      catch (e) { toast(String(e), "err"); }
    }, 40);
  }
  async function commitChannels(patch) {
    try { await api.setChannels(patch); }
    catch (e) { toast(String(e), "err"); }
  }

  // Ganged slider input -> update master+balance -> recompute L/R -> commit.
  function onMasterInput(v) {
    setMaster(v);
    const [nl, nr] = gangedToLR(v, balance);
    setL(nl); setR(nr);
  }
  function onBalanceInput(v) {
    setBalance(v);
    const [nl, nr] = gangedToLR(master, v);
    setL(nl); setR(nr);
  }
  function onGangedCommit() {
    commitVolume(l, r, muted);
  }

  function onSplitInput(channel, v) {
    if (channel === "l") setL(v); else setR(v);
  }
  function onSplitCommit() {
    commitVolume(l, r, muted);
    const [m, b] = lrToGanged(l, r);
    setMaster(m); setBalance(b);
  }

  function toggleMute() {
    const next = !muted;
    setMuted(next);
    commitVolume(l, r, next);
  }
  function toggleChannel(which) {
    const nlon = which === "l" ? !lon : lon;
    const nron = which === "r" ? !ron : ron;
    setLon(nlon); setRon(nron);
    commitChannels({
      stereo_mono: stereo ? "stereo" : "mono",
      l_on: nlon, r_on: nron,
    });
  }
  function toggleStereo() {
    const next = !stereo;
    setStereo(next);
    commitChannels({
      stereo_mono: next ? "stereo" : "mono",
      l_on: lon, r_on: ron,
    });
  }

  if (!state) return html`<div class="stub">Loading…</div>`;

  // The headline volume is whatever the master knob reads in ganged mode, or
  // the louder of L/R in split mode (so the big number never drops below the
  // loudest active channel). L and R are always shown underneath for clarity.
  const headline = mode === "ganged" ? master : Math.max(l, r);

  return html`
    <h1 class="page__title">Audio</h1>
    <p class="page__subtitle">
      Volume and channel routing. ${muted ? html`<strong style="color:var(--warn)">Muted.</strong>` : ""}
    </p>

    <div class="card">
      <div class="row--between" style="margin-bottom: var(--sp-5)">
        <div>
          <div class="card__title">Volume</div>
          <div class="vol-big">
            ${headline}%
            <span class="vol-big__sub">L ${l}% · R ${r}%</span>
          </div>
        </div>
        <div class="row" style="gap: var(--sp-2); flex-wrap: wrap; justify-content: flex-end">
          <div class="pill-group">
            <button class=${"btn" + (mode === "ganged" ? " btn--primary" : "")}
                    onClick=${() => setMode("ganged")}>Ganged</button>
            <button class=${"btn" + (mode === "split" ? " btn--primary" : "")}
                    onClick=${() => setMode("split")}>Split L/R</button>
          </div>
          <button class=${"btn" + (muted ? " btn--danger" : "")} onClick=${toggleMute}>
            ${muted ? "Un-mute" : "Mute"}
          </button>
        </div>
      </div>

      ${mode === "ganged" ? html`
        <div class="field">
          <label class="field__label" for="master">Master</label>
          <div class="row" style="gap: var(--sp-3)">
            <input id="master" class="slider" type="range" min="0" max="100"
                   value=${master}
                   style=${`--fill:${master}`}
                   onInput=${(e) => onMasterInput(parseInt(e.target.value, 10))}
                   onChange=${onGangedCommit} />
            <span class="chip">${master}%</span>
          </div>
        </div>
        <div class="field" style="margin-top: var(--sp-4)">
          <label class="field__label" for="balance">Balance</label>
          <div class="row" style="gap: var(--sp-3)">
            <input id="balance" class="slider" type="range" min="-100" max="100"
                   value=${balance}
                   style=${`--fill:${(balance + 100) / 2}`}
                   onInput=${(e) => onBalanceInput(parseInt(e.target.value, 10))}
                   onChange=${onGangedCommit} />
            <span class="chip">${balance === 0 ? "centre" : balance > 0 ? `R ${balance}` : `L ${Math.abs(balance)}`}</span>
          </div>
        </div>
      ` : html`
        <div class="field">
          <label class="field__label" for="lvol">Left</label>
          <div class="row" style="gap: var(--sp-3)">
            <input id="lvol" class="slider" type="range" min="0" max="100"
                   value=${l}
                   style=${`--fill:${l}`}
                   onInput=${(e) => onSplitInput("l", parseInt(e.target.value, 10))}
                   onChange=${onSplitCommit} />
            <span class="chip">${l}%</span>
          </div>
        </div>
        <div class="field" style="margin-top: var(--sp-4)">
          <label class="field__label" for="rvol">Right</label>
          <div class="row" style="gap: var(--sp-3)">
            <input id="rvol" class="slider" type="range" min="0" max="100"
                   value=${r}
                   style=${`--fill:${r}`}
                   onInput=${(e) => onSplitInput("r", parseInt(e.target.value, 10))}
                   onChange=${onSplitCommit} />
            <span class="chip">${r}%</span>
          </div>
        </div>
      `}
    </div>

    <div class="card">
      <div class="card__title">Channel routing</div>
      <p style="color: var(--fg-1); margin-top: var(--sp-2)">
        Turn a channel off to route to a single speaker, or sum the signal
        to mono before the per-channel volume so a single subwoofer or mono
        system receives L+R mixed together.
      </p>

      <label class="toggle-row">
        <span class="switch">
          <input type="checkbox" checked=${lon} onChange=${() => toggleChannel("l")} />
          <span class="switch__slider"></span>
        </span>
        <span>
          <span class="toggle-row__label">Left channel output</span>
          <span class="toggle-row__help">Disable to route only Right to both outputs.</span>
        </span>
      </label>

      <label class="toggle-row">
        <span class="switch">
          <input type="checkbox" checked=${ron} onChange=${() => toggleChannel("r")} />
          <span class="switch__slider"></span>
        </span>
        <span>
          <span class="toggle-row__label">Right channel output</span>
          <span class="toggle-row__help">Disable to route only Left to both outputs.</span>
        </span>
      </label>

      <label class="toggle-row">
        <span class="switch">
          <input type="checkbox" checked=${!stereo} onChange=${toggleStereo} />
          <span class="switch__slider"></span>
        </span>
        <span>
          <span class="toggle-row__label">Mono sum (L + R → both outputs)</span>
          <span class="toggle-row__help">
            Useful when only one speaker is connected or for centering a
            mono-recorded source.
          </span>
        </span>
      </label>
    </div>
  `;
}
