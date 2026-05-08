// Thin JSON wrappers around the firmware REST API. Every call returns a
// resolved JSON body or throws; component code handles the throw via the toast
// system.
//
// Phase 4 adds a push-based state channel (`/api/state/stream`, server-sent
// events). `api.subscribeState(cb)` opens the stream lazily, fans out to
// every subscriber, and replays the latest snapshot to late joiners so a
// page mounting after the first event still gets state immediately. Pages
// that previously called `api.getState()` once on mount can switch to a
// subscription with no additional polling — mutations on any device or tab
// land in the local view within ~250 ms (the firmware-side debounce).
// Pages that need the one-shot semantics keep using `api.getState()`.

async function j(method, url, body) {
  const init = { method, headers: {} };
  if (body !== undefined) {
    init.headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(body);
  }
  const res = await fetch(url, init);
  const text = await res.text();
  let json = null;
  if (text) {
    try { json = JSON.parse(text); } catch { /* non-JSON fallback */ }
  }
  if (!res.ok) {
    const msg = (json && (json.error || json.message)) || text || res.statusText;
    throw new Error(`${res.status} ${res.statusText}: ${msg}`);
  }
  return json ?? {};
}

export const api = {
  get: (url) => j("GET", url),
  post: (url, body) => j("POST", url, body ?? {}),
  delete: (url) => j("DELETE", url),

  // Convenience wrappers (stable SPA surface — implementations may swap
  // Phase 0 compat endpoints for Phase 1 JSON endpoints without page churn).

  async getState() {
    // Phase 1 replaces this with /api/state. For now, synthesize a view over
    // /api/status so the SPA can render something real during Phase 0.
    try {
      const s = await j("GET", "/api/state");
      return s;
    } catch (_) {
      const legacy = await j("GET", "/api/status");
      return {
        mode: legacy.source === 0 ? "bluetooth" : "snapclient",
        volume: { l: legacy.volume ?? 80, r: legacy.volume ?? 80, muted: false },
        channels: { stereo_mono: "stereo", l_on: true, r_on: true },
        snapcast: { host: legacy.snap_host || "", port: legacy.snap_port || 1704 },
        device_name: "Geelen Audio",
        firmware: "dev",
        voice_feedback: true,
        _legacy: true,
      };
    }
  },

  async getSystem() {
    try { return await j("GET", "/api/system"); }
    catch (_) { return { _unimplemented: true }; }
  },

  async getSystemMetrics() {
    try { return await j("GET", "/api/system/metrics"); }
    catch (_) { return { _unimplemented: true }; }
  },

  async getSystemIdentity() {
    try { return await j("GET", "/api/system/identity"); }
    catch (_) { return { _unimplemented: true }; }
  },

  async getTestTone() {
    return await j("GET", "/api/test-tone");
  },

  async setTestTone(patch) {
    return await j("POST", "/api/test-tone", patch);
  },

  async setVolume(percentL, percentR, muted) {
    try {
      return await j("POST", "/api/volume", { l: percentL, r: percentR, muted: !!muted });
    } catch (_) {
      // Phase 0 fallback: single scalar `v=`.
      const v = Math.round((percentL + percentR) / 2);
      return await j("GET", `/api/volume?v=${v}`);
    }
  },

  async setMode(mode) {
    try {
      return await j("POST", "/api/mode", { mode });
    } catch (_) {
      const map = { bluetooth: 0, snapclient: 1, dlna: 2, test_tone: 3 };
      const s = map[mode] ?? 0;
      return await j("POST", `/api/source?s=${s}`);
    }
  },

  async setSnapcast(host, port) {
    return await j("POST", "/api/snapcast", { host, port });
  },

  async setChannels(patch) {
    return await j("POST", "/api/channels", patch);
  },

  async setDlna(patch) {
    return await j("POST", "/api/dlna", patch);
  },

  async setVoiceFeedback(enabled) {
    return await j("POST", "/api/voice-feedback", { enabled: !!enabled });
  },

  async setDeviceName(name) {
    return await j("POST", "/api/device-name", { name });
  },

  async setEqLayer(patch) {
    return await j("POST", "/api/eq", patch);
  },

  async setHeadroom(patch) {
    return await j("POST", "/api/eq/headroom", patch);
  },

  async getPresets() { return await j("GET", "/api/eq/presets"); },
  async savePreset(idx, name) { return await j("POST", `/api/eq/presets/${idx}/save`, { name }); },
  async loadPreset(idx)       { return await j("POST", `/api/eq/presets/${idx}/load`); },
  async renamePreset(idx, name) { return await j("POST", `/api/eq/presets/${idx}/rename`, { name }); },
  async deletePreset(idx)     { return await j("DELETE", `/api/eq/presets/${idx}`); },

  async reset(scope) { return await j("POST", "/api/reset", { scope }); },

  async setWifi(ssid, pass) {
    const body = `ssid=${encodeURIComponent(ssid)}&pass=${encodeURIComponent(pass)}`;
    const res = await fetch("/api/wifi", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body,
    });
    if (!res.ok) throw new Error(res.statusText);
    return res.json();
  },

  // Wi-Fi onboarding helpers used by the /wifi page and (in AP-only mode)
  // by the inline captive setup form. /scan is a synchronous active scan
  // on the firmware side and can take ~3 s.
  async getWifiScan() {
    return await j("GET", "/api/wifi/scan");
  },

  async getWifiState() {
    return await j("GET", "/api/wifi/state");
  },

  // Wi-Fi power-save selector (Phase 5). Persisted in NVS, applied on
  // next boot — the firmware deliberately does NOT hot-apply because
  // flipping `esp_wifi_set_ps` mid-stream interacts poorly with active
  // BT coex. Callers should surface "applies after reboot" in the UI.
  async setWifiPowerSave(mode) {
    return await j("POST", "/api/wifi/power-save", { mode });
  },

  // Subscribe to push-based device state updates. The SSE source is shared
  // across every subscriber so opening five pages still uses one socket
  // (also cheaper for the firmware: it tops out at six concurrent SSE
  // sockets total — see kMaxSubscribers in idf_state_stream.cpp).
  //
  // Returns an `unsubscribe()` function. Closing the last subscription
  // closes the EventSource so a long-idled tab does not hold the firmware
  // socket forever; the next `subscribeState` reopens it on demand.
  //
  // The callback fires:
  //   - immediately if a snapshot is already cached (late joiner)
  //   - on every fresh `event: snapshot` from the server
  //   - never on heartbeats (`event: ping` is silently consumed)
  //
  // If the stream fails for any reason the helper falls back to a single
  // `GET /api/state` so the SPA at least gets one snapshot and can render.
  subscribeState(cb) {
    return _stateBus.subscribe(cb);
  },
};

// Internal singleton: lazily-opened EventSource + Set of callbacks +
// last-snapshot cache. Hidden from the api.* surface deliberately —
// callers should reach for it via `subscribeState`.
const _stateBus = (() => {
  let es = null;
  let lastSnapshot = null;
  const subs = new Set();
  let fallbackTried = false;

  function open() {
    if (es) return;
    try {
      es = new EventSource("/api/state/stream");
    } catch (e) {
      tryFallback();
      return;
    }
    es.addEventListener("snapshot", (ev) => {
      try {
        const data = JSON.parse(ev.data);
        lastSnapshot = data;
        for (const cb of subs) {
          try { cb(data); } catch { /* swallow component errors */ }
        }
      } catch {
        // Malformed JSON — ignore one frame; next one will likely be fine.
      }
    });
    es.addEventListener("ping", () => { /* heartbeat */ });
    es.addEventListener("error", () => {
      // The browser auto-reconnects EventSource on transient network blips;
      // we only fall back to REST if we've never seen a snapshot at all.
      if (!lastSnapshot) tryFallback();
    });
  }

  function close() {
    if (es) {
      es.close();
      es = null;
    }
  }

  function tryFallback() {
    if (fallbackTried || lastSnapshot) return;
    fallbackTried = true;
    api.getState().then(
      (s) => {
        lastSnapshot = s;
        for (const cb of subs) {
          try { cb(s); } catch {}
        }
      },
      () => { /* surfaced on the page that called subscribeState */ },
    );
  }

  return {
    subscribe(cb) {
      subs.add(cb);
      open();
      // Replay the latest snapshot synchronously so a page mounting
      // after the first frame doesn't have to wait for the next change.
      if (lastSnapshot) {
        try { cb(lastSnapshot); } catch {}
      }
      return () => {
        subs.delete(cb);
        if (subs.size === 0) close();
      };
    },
  };
})();
