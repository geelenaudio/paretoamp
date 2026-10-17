// Shared 24×24 stroke icons. All inherit `currentColor` so they follow the
// theme (and Reset-row tone colours) instead of colour-emoji palettes.

import { html } from "/vendor/htm-preact.mjs";

export function IcoHome({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M4 11 L12 4 L20 11" />
    <path d="M7 10.5 V20 H17 V10.5" />
    <path d="M10 20 V14.5 H14 V20" />
  </svg>`;
}

export function IcoMode({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <circle cx="12" cy="12" r="8" />
    <circle cx="12" cy="12" r="2.4" fill="currentColor" stroke="none" />
  </svg>`;
}

export function IcoKnob({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <circle cx="12" cy="12" r="8" />
    <circle cx="12" cy="12" r="2" fill="currentColor" stroke="none" />
    <line x1="12" y1="12" x2="16.5" y2="7.5" />
  </svg>`;
}

export function IcoNote({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <ellipse cx="8" cy="18" rx="3.6" ry="2.6" fill="currentColor" stroke="none" />
    <path d="M11.6 18 V5.5" />
    <path d="M11.6 5.5 C16 7 19 9.2 19 13" />
  </svg>`;
}

export function IcoFlask({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M9 3 h6 v4.5 L19 20 H5 L9 7.5 Z" />
    <line x1="8" y1="14" x2="16" y2="14" />
    <circle cx="14" cy="17.2" r="0.9" fill="currentColor" stroke="none" />
  </svg>`;
}

export function IcoWifi({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M5 10.5 a9.5 9.5 0 0 1 14 0" />
    <path d="M8 13.8 a5.5 5.5 0 0 1 8 0" />
    <circle cx="12" cy="18.2" r="1.3" fill="currentColor" stroke="none" />
  </svg>`;
}

export function IcoRefresh({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M21 12 a9 9 0 1 1-2.6-6.3" />
    <polyline points="21 3 21 9 15 9" />
  </svg>`;
}

export function IcoLogs({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M8 6 H20" />
    <path d="M8 12 H20" />
    <path d="M8 18 H16" />
    <circle cx="5" cy="6" r="1.15" fill="currentColor" stroke="none" />
    <circle cx="5" cy="12" r="1.15" fill="currentColor" stroke="none" />
    <circle cx="5" cy="18" r="1.15" fill="currentColor" stroke="none" />
  </svg>`;
}

export function IcoInfo({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <circle cx="12" cy="12" r="8" />
    <path d="M12 11 V16" />
    <circle cx="12" cy="8" r="1.1" fill="currentColor" stroke="none" />
  </svg>`;
}

export function IcoHeadphones({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M4 14 a8 8 0 0 1 16 0" />
    <rect x="3" y="14" width="4.2" height="6.5" rx="1.6" />
    <rect x="16.8" y="14" width="4.2" height="6.5" rx="1.6" />
  </svg>`;
}

export function IcoCast({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M3 8 V6.5 A2 2 0 0 1 5 4.5 H19 A2 2 0 0 1 21 6.5 V17.5 A2 2 0 0 1 19 19.5 H13" />
    <path d="M3 12.2 a7 7 0 0 1 7 7" />
    <path d="M3 16 a3.2 3.2 0 0 1 3.2 3.2" />
    <circle cx="3.4" cy="19.4" r="1.15" fill="currentColor" stroke="none" />
  </svg>`;
}

export function IcoUsb({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M12 3 V14" />
    <circle cx="12" cy="16.5" r="2.2" />
    <path d="M12 18.7 V21" />
    <path d="M12 8 H8.5 A1.5 1.5 0 0 1 8.5 5 H12" />
    <path d="M12 11 H16.2" />
    <circle cx="17.4" cy="11" r="1.3" />
  </svg>`;
}

export function IcoTv({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <rect x="3" y="5" width="18" height="12" rx="2" />
    <path d="M8 21 H16" />
    <path d="M12 17 V21" />
  </svg>`;
}

export function IcoSliders({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M7 5 V19" />
    <path d="M12 5 V19" />
    <path d="M17 5 V19" />
    <circle cx="7" cy="10" r="2.1" fill="currentColor" stroke="none" />
    <circle cx="12" cy="15" r="2.1" fill="currentColor" stroke="none" />
    <circle cx="17" cy="8" r="2.1" fill="currentColor" stroke="none" />
  </svg>`;
}

export function IcoWarn({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M12 3.5 L21.5 20 H2.5 Z" />
    <path d="M12 9.5 V14.5" />
    <circle cx="12" cy="17.2" r="1.05" fill="currentColor" stroke="none" />
  </svg>`;
}

export function IcoSun({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <circle cx="12" cy="12" r="3.6" />
    <path d="M12 3 V5.4" />
    <path d="M12 18.6 V21" />
    <path d="M3 12 H5.4" />
    <path d="M18.6 12 H21" />
    <path d="M5.6 5.6 L7.3 7.3" />
    <path d="M16.7 16.7 L18.4 18.4" />
    <path d="M18.4 5.6 L16.7 7.3" />
    <path d="M7.3 16.7 L5.6 18.4" />
  </svg>`;
}

export function IcoMoon({ class: cls = "ico" } = {}) {
  return html`<svg class=${cls} viewBox="0 0 24 24" fill="none"
    stroke="currentColor" stroke-width="2" stroke-linecap="round"
    stroke-linejoin="round" aria-hidden="true">
    <path d="M20 13.4 A8 8 0 1 1 10.6 4 6.2 6.2 0 0 0 20 13.4 Z" />
  </svg>`;
}
