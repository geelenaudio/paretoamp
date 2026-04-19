// Live log viewer page. Lazy-loaded by the router, so the EventSource
// connection to /api/logs/stream only opens when the user actually looks
// at this page. Leaving the page kills the connection via the cleanup
// in useEffect.
//
// The firmware ships raw ESP-IDF log lines over SSE as
// `{ t: <ms_since_boot>, line: "<raw log line>" }`. The formats we see are
// (with color on):
//
//   "\x1b[0;32mI (1234) TAG: message\x1b[0m"
//
// and with color off:
//
//   "I (1234) TAG: message"
//
// We strip ANSI escapes, then regex-parse level/tag/message so we can
// filter by level and colourise per-level without hurting the firmware
// side. A broken parse falls back to `level=?, tag=-, msg=<raw>`.

import { html, useEffect, useRef, useState } from "/vendor/htm-preact.mjs";

const LEVELS = [
  { id: "E", label: "Error",   color: "#ff6a6a" },
  { id: "W", label: "Warn",    color: "#e6b566" },
  { id: "I", label: "Info",    color: "#9fd4ff" },
  { id: "D", label: "Debug",   color: "#c8a2ff" },
  { id: "V", label: "Verbose", color: "#6fd1b7" },
];
const LEVEL_RANK = { E: 0, W: 1, I: 2, D: 3, V: 4, "?": 5 };
const MAX_BUFFER = 2000;

const ANSI_RE = /\x1b\[[0-9;]*m/g;
const ESP_LOG_RE = /^([EWIDV])\s*\((\d+)\)\s+([^:]+):\s*(.*)$/;

function parseLine(raw) {
  const clean = String(raw || "").replace(ANSI_RE, "").trim();
  const m = clean.match(ESP_LOG_RE);
  if (!m) return { level: "?", tag: "-", ts: null, msg: clean };
  return { level: m[1], ts: parseInt(m[2], 10), tag: m[3].trim(), msg: m[4] };
}

function formatWallClock(ms) {
  const d = new Date(ms);
  return d.toLocaleTimeString(undefined, { hour12: false }) +
         "." + String(d.getMilliseconds()).padStart(3, "0");
}

export default function Logs({ toast }) {
  const [entries, setEntries] = useState([]);
  const [minLevel, setMinLevel] = useState("I");
  const [tagFilter, setTagFilter] = useState("");
  const [autoScroll, setAutoScroll] = useState(true);
  const [connected, setConnected] = useState(false);
  const [paused, setPaused] = useState(false);

  const listRef = useRef(null);
  const bufferRef = useRef([]); // full history, trimmed to MAX_BUFFER
  const seqRef = useRef(0);
  const esRef = useRef(null);

  // Open / tear down the SSE connection together with the component.
  useEffect(() => {
    const es = new EventSource("/api/logs/stream");
    esRef.current = es;
    es.onopen = () => setConnected(true);
    es.onerror = () => setConnected(false);
    es.onmessage = (ev) => {
      try {
        const data = JSON.parse(ev.data);
        const parsed = parseLine(data.line);
        const entry = { id: ++seqRef.current, wall: Date.now(), boot_ms: data.t, ...parsed };
        bufferRef.current.push(entry);
        if (bufferRef.current.length > MAX_BUFFER) {
          bufferRef.current.splice(0, bufferRef.current.length - MAX_BUFFER);
        }
        if (!paused) setEntries(bufferRef.current.slice());
      } catch {
        // Drop malformed frame silently; the SSE heartbeat comments end up here too.
      }
    };
    return () => {
      es.close();
      esRef.current = null;
    };
  }, [paused]);

  // Auto-scroll to bottom whenever new entries arrive (if enabled).
  useEffect(() => {
    if (!autoScroll || !listRef.current) return;
    listRef.current.scrollTop = listRef.current.scrollHeight;
  }, [entries, autoScroll]);

  function clear() {
    bufferRef.current = [];
    setEntries([]);
  }

  async function copyAll() {
    const text = entries.map((e) =>
      `${formatWallClock(e.wall)} ${e.level} ${e.tag}: ${e.msg}`
    ).join("\n");
    try {
      await navigator.clipboard.writeText(text);
      toast(`Copied ${entries.length} lines`, "ok");
    } catch (e) {
      toast("Clipboard denied", "err");
    }
  }

  const minRank = LEVEL_RANK[minLevel] ?? 2;
  const tagQuery = tagFilter.trim().toLowerCase();
  const filtered = entries.filter((e) => {
    if ((LEVEL_RANK[e.level] ?? 5) > minRank) return false;
    if (tagQuery && !e.tag.toLowerCase().includes(tagQuery)) return false;
    return true;
  });

  return html`
    <h1 class="page__title">Logs</h1>
    <p class="page__subtitle">
      Live stream from the device's ESP-IDF log. Filtering is local — all
      levels are captured.
    </p>

    <div class="card" style="padding: var(--sp-3); margin-bottom: var(--sp-3)">
      <div class="logs-toolbar">
        <div class="logs-toolbar__group">
          <span class=${"pill " + (connected ? "pill--ok" : "pill--err")}>
            ${connected ? "connected" : "disconnected"}
          </span>
          <span style="color:var(--fg-2); font-family: var(--font-mono); font-size: 12px;">
            ${filtered.length} / ${entries.length} lines
          </span>
        </div>

        <label class="field" style="margin: 0">
          <span class="field__label">Min level</span>
          <select class="input" value=${minLevel}
                  onChange=${(e) => setMinLevel(e.target.value)}>
            ${LEVELS.map((l) => html`<option key=${l.id} value=${l.id}>${l.id} — ${l.label}</option>`)}
          </select>
        </label>

        <label class="field" style="margin: 0; min-width: 200px;">
          <span class="field__label">Tag contains</span>
          <input class="input" type="text" placeholder="e.g. wifi, snap"
                 value=${tagFilter}
                 onInput=${(e) => setTagFilter(e.target.value)} />
        </label>

        <label style="display:flex; align-items:center; gap: var(--sp-2); color:var(--fg-1); font-size: 13px;">
          <input type="checkbox" checked=${autoScroll}
                 onChange=${(e) => setAutoScroll(e.target.checked)} />
          Autoscroll
        </label>
        <label style="display:flex; align-items:center; gap: var(--sp-2); color:var(--fg-1); font-size: 13px;">
          <input type="checkbox" checked=${paused}
                 onChange=${(e) => setPaused(e.target.checked)} />
          Pause
        </label>

        <div class="logs-toolbar__group logs-toolbar__group--end">
          <button class="btn" onClick=${copyAll}>Copy</button>
          <button class="btn" onClick=${clear}>Clear</button>
        </div>
      </div>
    </div>

    <div class="logs" ref=${listRef}>
      ${filtered.length === 0
        ? html`<div class="logs__empty">No matching lines yet.</div>`
        : filtered.map((e) => {
            const col = LEVELS.find((l) => l.id === e.level)?.color || "var(--fg-2)";
            return html`
              <div key=${e.id} class="logs__row">
                <span class="logs__time">${formatWallClock(e.wall)}</span>
                <span class="logs__level" style=${"color:" + col}>${e.level}</span>
                <span class="logs__tag">${e.tag}</span>
                <span class="logs__msg">${e.msg}</span>
              </div>
            `;
          })}
    </div>
  `;
}
