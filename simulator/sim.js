/*
 * Ritmo Code — simulatore web del firmware (claude_stick.ino)
 *
 * Porting 1:1 di layout, palette, macchina a stati e logica (trend, heatmap,
 * soglie, sonda modelli, account) su DOM 480x320. Rete, NVS, crypto e WiFi
 * sono simulati; i dati arrivano dal pannello a destra.
 */
(() => {
'use strict';

// ---------- palette: tema "Terminale" (stile Claude Code) ----------
// Fondo terminale caldo, cornici a 1 px, un solo accento argilla; verde/ambra/rosso
// solo per lo stato. Tutto monospace (JetBrains Mono).
const C = {
  BG: '#141413', SURFACE: '#1B1A18', SURFACE2: '#23221F', TRACK: '#2C2A27', GRID: '#262421',
  BORDER: '#3A3834', TEXT: '#E8E6DF', MUTED: '#8E8B82', FAINT: '#5C5A55', ACCENT: '#D97757',
  OK: '#9BC08A', WARN: '#E0B25A', BAD: '#E06C5A', BLUE: '#7DB9D6', LILAC: '#B7A6E0',
};
const CFG = { PIN_LEN: 4, MAX_PIN_ATTEMPTS: 10, LOCKOUT_BASE_SEC: 60, ACCT_MAX: 4, FW: '3.7' };
const DEMO_PIN = '1234';

const $ = (id) => document.getElementById(id);
const scr = $('screen');

// ---------- tempo ----------
// simMs: orologio "millis()" accelerabile (poll, reset, lockout).
// realMs: tempo reale per le animazioni (blink, bob, momenti, slideshow).
let speed = 1, simMs = 0;
const epoch0 = Math.floor(Date.now() / 1000);
const millis = () => simMs;
const realMs = () => performance.now();
const nowEpoch = () => epoch0 + Math.floor(simMs / 1000);

// ---------- helpers numerici/colore ----------
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const hex2 = (h) => [1, 3, 5].map((i) => parseInt(h.substr(i, 2), 16));
const rgb = (a) => `rgb(${a.map((v) => Math.round(v)).join(',')})`;
// lv_color_mix(c1, c2, ratio): ratio 255 = c1
const mix = (c1, c2, ratio) => { const a = hex2(c1), b = hex2(c2); return rgb(a.map((v, i) => (v * ratio + b[i] * (255 - ratio)) / 255)); };
// colore per soglia (a gradini, come in un terminale): <50 ok, <80 argilla, poi rosso
function gradColor(p) {
  p = clamp(p, 0, 100);
  return p >= 80 ? C.BAD : p >= 50 ? C.ACCENT : C.OK;
}
const pad2 = (n) => String(n).padStart(2, '0');

// ---------- stato persistente (NVS simulata) ----------
const TZ_ROME = 99;
const P = { lang: 0, tz: TZ_ROME, poll: 120, slide: 0, heatm: 3, bri: 1, pinatt: 0, rstal: true, ccal: 2, night: 0, nightp: true, dim: 0, pause: false, pauseUntil: 0, clock: 0, nightclk: true, nightbri: 0 };
const TRS = (pt, en) => (P.lang ? en : pt);

// ---------- stato app ----------
const ST = { BOOT: 'BOOT', PIN: 'PIN', SETUP_PIN: 'SETUP_PIN', WIFI: 'WIFI', TOKEN: 'TOKEN', LOADING: 'LOADING',
  MAIN: 'MAIN', SETTINGS: 'SETTINGS', ACCOUNTS: 'ACCOUNTS', ACCT_NAME: 'ACCT_NAME', ABOUT: 'ABOUT', ERROR: 'ERROR',
  MODELS: 'MODELS', MODEL_EDIT: 'MODEL_EDIT' };
let state = ST.BOOT, pending = ST.BOOT, dirty = false;
const requestState = (s) => { pending = s; dirty = true; };

const G = {
  usage: { ok: false, h5: 0, d7: 0, h5Reset: 0, d7Reset: 0, statusOverall: '', error: '' },
  status: { ok: true, up: [true, true, true, true] },
  models: [
    { name: 'Haiku', defId: 'claude-haiku-4-5-20251001', id: 'claude-haiku-4-5-20251001', pr: { code: 0, ms: 0 } },
    { name: 'Sonnet', defId: 'claude-sonnet-5', id: 'claude-sonnet-5', pr: { code: 0, ms: 0 } },
    { name: 'Opus', defId: 'claude-opus-5', id: 'claude-opus-5', pr: { code: 0, ms: 0 } },
    { name: 'Fable', defId: 'claude-fable-5-1', id: 'claude-fable-5-1', pr: { code: 0, ms: 0 } },
  ],
  probeIdx: 0,
  accts: { used: [true, false, false, false], label: ['Account 1', '', '', ''], active: 0 },
  hasToken: true, onboarding: false, sessionPin: '', pendingToken: '', pendingLabel: '', tokenTargetSlot: 0,
  pinEntry: '', pinFirst: '', pinConfirming: false, lockoutUntil: 0,
  wifiConnected: true, ssid: 'Innova-Studio',
  wantRefresh: false, refreshing: false, lastFetchOk: true, lastOkMs: 0, lastPollMs: 0,
  lastTouch: -1e9, lastSlide: 0,
  hist: [], hourBurn: new Array(24).fill(0), days: [], lastH5: -1,
  thrFired: [0, 0], thrPrev: [-1, -1], thrBase: false, pendWin: -1, pendThr: 0,
  winPeak: [0, 0], winReset: [0, 0], resetSeen: [0, 0], pendPeak: 0,
  curTile: 0, wipeArmed: false, acctDelArmed: -1, renameSlot: -1,
};
const HIST_MAX = 160, NDAYS = 31, NSEG = 18, NTILES = 7;

// scenario del pannello (ciò che "risponderebbe" la API)
const API = {
  h5: 41, d7: 16, r5min: 100, r7h: 123, burn: 0, ust: 'allowed', fail: false,
  probe: [{ code: 200, ms: 900 }, { code: 200, ms: 1400 }, { code: 429, ms: 700 }, { code: 200, ms: 2100 }],
  incident: [false, false, false, false], statusOk: true,
};

// ---------- log seriale ----------
function slog(s) {
  const el = $('log'); const t = new Date((nowEpoch()) * 1000).toISOString().substr(11, 8);
  el.textContent += `[${t}] ${s}\n`; el.scrollTop = el.scrollHeight;
}

// ---------- formattazione (fmt_eta / fmt_clock / fmt_hm) ----------
// ora locale = UTC + fuso configurato (come configTime(tz*3600))
// offset (s) del fuso configurato; Roma = Europe/Rome con ora legale automatica
function tzOffsetSec(epoch) {
  if (P.tz !== TZ_ROME) return P.tz * 3600;
  const f = new Intl.DateTimeFormat('en-US', { timeZone: 'Europe/Rome', hourCycle: 'h23', year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: 'numeric' });
  const o = Object.fromEntries(f.formatToParts(new Date(epoch * 1000)).map((x) => [x.type, x.value]));
  return (Date.UTC(+o.year, o.month - 1, +o.day, +o.hour, +o.minute) / 1000) - Math.floor(epoch / 60) * 60;
}
function localParts(epoch) {
  const d = new Date((epoch + tzOffsetSec(epoch)) * 1000);
  return { h: d.getUTCHours(), m: d.getUTCMinutes(), wd: d.getUTCDay(), d: d.getUTCDate(), mo: d.getUTCMonth() + 1 };
}
function fmtEta(epoch) {
  if (!epoch) return '--';
  let d = epoch - nowEpoch();
  if (d <= 0) return TRS('ora', 'now');
  const days = Math.floor(d / 86400); d %= 86400;
  const hrs = Math.floor(d / 3600); d %= 3600;
  const mins = Math.floor(d / 60);
  if (days > 0) return `${days}d ${hrs}h`;
  if (hrs > 0) return `${hrs}h ${pad2(mins)}m`;
  return `${mins}m`;
}
const WD = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];   // strftime %a in locale C
const GG = ['Dom', 'Lun', 'Mar', 'Mer', 'Gio', 'Ven', 'Sab'];
function fmtClock(epoch) { if (!epoch) return '--:--'; const p = localParts(epoch); return `${(P.lang ? WD : GG)[p.wd]} ${pad2(p.h)}:${pad2(p.m)}`; }
function fmtHm(epoch) { if (!epoch) return '--:--'; const p = localParts(epoch); return `${pad2(p.h)}:${pad2(p.m)}`; }

// ---------- icone (LV_SYMBOL_* ridisegnati in SVG) ----------
const ICONS = {
  left: 'M15 18l-6-6 6-6', ok: 'M5 12l5 5L20 7', refresh: 'M20 12a8 8 0 1 1-2.34-5.66M20 4v5h-5',
  settings: 'M12 15.2a3.2 3.2 0 1 0 0-6.4 3.2 3.2 0 0 0 0 6.4zM19.4 13a7.6 7.6 0 0 0 0-2l2-1.6-2-3.4-2.4 1a7.4 7.4 0 0 0-1.7-1L15 3.5h-4l-.3 2.5a7.4 7.4 0 0 0-1.7 1l-2.4-1-2 3.4 2 1.6a7.6 7.6 0 0 0 0 2l-2 1.6 2 3.4 2.4-1a7.4 7.4 0 0 0 1.7 1l.3 2.5h4l.3-2.5a7.4 7.4 0 0 0 1.7-1l2.4 1 2-3.4z',
  eye: 'M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12zM12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z',
  loop: 'M17 2l4 4-4 4M3 11v-1a4 4 0 0 1 4-4h14M7 22l-4-4 4-4M21 13v1a4 4 0 0 1-4 4H3',
  gps: 'M12 22s7-7.5 7-12a7 7 0 1 0-14 0c0 4.5 7 12 7 12zM12 12.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5z',
  play: 'M7 4l13 8-13 8z', list: 'M8 6h13M8 12h13M8 18h13M3.5 6h.01M3.5 12h.01M3.5 18h.01',
  wifi: 'M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M2 9a15 15 0 0 1 20 0M12 20h.01',
  dir: 'M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z',
  keyboard: 'M3 6h18v12H3zM7 10h.01M11 10h.01M15 10h.01M7 14h10', file: 'M14 3H6v18h12V7zM14 3v4h4',
  trash: 'M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14', edit: 'M17 3l4 4L8 20H4v-4z', plus: 'M12 5v14M5 12h14',
  warning: 'M12 3l10 18H2zM12 10v5M12 18h.01', back: 'M21 5H8l-6 7 6 7h13zM12 9l6 6M18 9l-6 6',
  close: 'M18 6L6 18M6 6l12 12', right: 'M9 18l6-6-6-6', enter: 'M20 5v7H5M9 8l-4 4 4 4',
};
const icon = (name, size = 14, stroke = 2.6) =>
  `<svg class="ico" width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="${stroke}" stroke-linecap="round" stroke-linejoin="round"><path d="${ICONS[name]}"/></svg>`;

// ---------- asset brand ----------
// Clawd: path ufficiale di assets/brand/claudecode-color.svg, ritagliato come
// gen_logo_assets.py (viewBox y 5..20) -> occhi = buchi (evenodd).
const CLAWD_PATH = 'M20.998 10.949H24v3.102h-3v3.028h-1.487V20H18v-2.921h-1.487V20H15v-2.921H9V20H7.488v-2.921H6V20H4.487v-2.921H3V14.05H0V10.95h3V5h17.998v5.949zM6 10.949h1.488V8.102H6v2.847zm10.51 0H18V8.102h-1.49v2.847z';
const clawdSvg = (fill = C.ACCENT) =>
  `<svg viewBox="0 5 24 15" preserveAspectRatio="none"><path fill-rule="evenodd" fill="${fill}" d="${CLAWD_PATH}"/></svg>`;
let WORDMARK_SVG = '';
fetch('../assets/brand/claudecode-text.svg').then((r) => r.text()).then((t) => {
  WORDMARK_SVG = t.replace(/fill="currentColor"/, `fill="${C.ACCENT}"`).replace(/height="1em"/, '').replace(/<title>.*?<\/title>/, '')
    .replace('<svg ', '<svg preserveAspectRatio="none" ');
  if (state === ST.MAIN) requestState(ST.MAIN);
}).catch(() => {});
// dimensioni degli sprite generati (logo_assets.h)
const SPR = { sm: [42, 26], md: [88, 56], big: [144, 90], xl: [176, 110] };
const EYES = { md: { x: [22, 61], y: 12, w: 5, h: 10 }, xl: { x: [44, 121], y: 23, w: 11, h: 21 } };

// ============================================================
// Mini-toolkit "LVGL" su DOM
// ============================================================
function obj(parent, x, y, w, h, style = {}) {
  const d = document.createElement('div');
  d.className = 'o';
  if (x != null) d.style.left = x + 'px';
  if (y != null) d.style.top = y + 'px';
  if (w != null) d.style.width = w + 'px';
  if (h != null) d.style.height = h + 'px';
  Object.assign(d.style, style);
  parent.appendChild(d);
  return d;
}
function label(parent, html, size, color, x = null, y = null) {
  const l = obj(parent, x, y, null, null, { fontSize: size + 'px', color });
  l.classList.add('lbl');
  l.innerHTML = html;
  return l;
}
function setText(l, html, color) { if (!l) return; l.innerHTML = html; if (color) l.style.color = color; }
// allineamento stile lv_obj_align (relativo al genitore)
function align(el, how, ox = 0, oy = 0) {
  const s = el.style;
  s.left = s.right = s.top = s.bottom = ''; s.transform = '';
  const tx = [], set = (k, v) => { s[k] = v + 'px'; };
  switch (how) {
    case 'TOP_LEFT': set('left', ox); set('top', oy); break;
    case 'TOP_MID': s.left = '50%'; set('top', oy); tx.push(`translateX(calc(-50% + ${ox}px))`); break;
    case 'TOP_RIGHT': set('right', -ox); set('top', oy); break;
    case 'CENTER': s.left = '50%'; s.top = '50%'; tx.push(`translate(calc(-50% + ${ox}px), calc(-50% + ${oy}px))`); break;
    case 'BOTTOM_MID': s.left = '50%'; set('bottom', -oy); tx.push(`translateX(calc(-50% + ${ox}px))`); break;
    case 'BOTTOM_LEFT': set('left', ox); set('bottom', -oy); break;
    case 'LEFT_MID': set('left', ox); s.top = '50%'; tx.push(`translateY(calc(-50% + ${oy}px))`); break;
  }
  s.transform = tx.join(' ');
  return el;
}
function rrect(parent, x, y, w, h, r, col) { return obj(parent, x, y, w, h, { borderRadius: r + 'px', background: col }); }
function card(parent, x, y, w, h) {
  const c = obj(parent, x, y, w, h, { background: C.SURFACE, borderRadius: '18px', padding: '14px' });
  const inner = obj(c, 14, 14, w - 28, h - 28);        // area contenuto (pad 14)
  inner.style.overflow = 'visible';
  return { box: c, in: inner };
}
function button(parent, html, size, bg, fg, w, h, onClick) {
  const b = obj(parent, null, null, w, h, { background: bg, borderRadius: '10px' });
  b.classList.add('btn');
  const l = label(b, html, size, fg); align(l, 'CENTER');
  b._lbl = l;
  if (onClick) b.addEventListener('click', (e) => { if (dragGuard()) return; e.stopPropagation(); onClick(e); });
  return b;
}
function mkchip(parent, x, y) {
  const o = obj(parent, x, y, null, 24, { borderRadius: '12px', padding: '0 10px', display: 'flex', alignItems: 'center', fontSize: '12px', whiteSpace: 'nowrap' });
  return o;
}
function setChip(o, txt, col) {
  if (!o) return;
  o.style.background = mix(col, C.BG, 60);
  o.style.color = col;
  o.textContent = txt || '--';
}
// spinner testuale braille (sul firmware: una label che cambia carattere ogni 80 ms)
const BRAILLE = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏';
function spinner(parent, size = 18, color = C.ACCENT) {
  const l = label(parent, BRAILLE[0], size, color); l.classList.add('brl');
  return l;
}
setInterval(() => {
  const ch = BRAILLE[Math.floor(performance.now() / 80) % BRAILLE.length];
  document.querySelectorAll('#screen .brl').forEach((e) => { e.textContent = ch; });
}, 80);
function clawdImg(parent, kind, fill) {
  const [w, h] = SPR[kind];
  const d = obj(parent, null, null, w, h); d.classList.add('clawd');
  d.innerHTML = clawdSvg(fill);
  return d;
}
function buildClaudeMark(parent) { const m = clawdImg(parent, 'big'); m.classList.add('breathe'); return m; }

// ---------- primitive del tema Terminale ----------
const GT = `<span style="color:${C.ACCENT}">&gt;</span>`;          // prompt
const SPARK = `<span style="color:${C.ACCENT}">✻</span>`;
// riquadro a filo sottile con il titolo inciso nella cornice
function tbox(parent, x, y, w, h, legend, color = C.BORDER) {
  const b = obj(parent, x, y, w, h, { border: `1px solid ${color}`, borderRadius: '4px' });
  if (legend) {
    const l = label(b, legend, 11, C.MUTED, 8, -8);
    Object.assign(l.style, { background: C.BG, padding: '0 6px', letterSpacing: '.03em' });
    b._lg = l;
  }
  return b;
}
// pulsante: cornice 1 px, testo centrato
function tbtn(parent, html, w, h, onClick, { color = C.TEXT, size = 13, border = C.BORDER } = {}) {
  const b = obj(parent, null, null, w, h, { border: `1px solid ${border}`, borderRadius: '4px', background: C.BG, cursor: 'pointer' });
  b.classList.add('btn', 'tbtn');
  const l = label(b, html, size, color); align(l, 'CENTER'); b._lbl = l;
  if (onClick) b.addEventListener('click', (e) => { if (dragGuard()) return; e.stopPropagation(); touched(); onClick(e); });
  return b;
}
// barra a blocchi: sul firmware due label affiancate (piena + vuota) con lo stesso font mono
function blocksLabel(parent, x, y, n = 19, size = 13) {
  const l = label(parent, '', size, C.TRACK, x, y); l.style.letterSpacing = '-.5px'; l._n = n;
  return l;
}
function setBlocks(l, pct, color) {
  if (!l) return;
  let f = Math.floor(clamp(pct, 0, 100) / 100 * l._n + 0.5);
  if (pct > 0.5 && f === 0) f = 1;
  l.innerHTML = `<span style="color:${color}">${'█'.repeat(f)}</span><span style="color:${C.TRACK}">${'█'.repeat(l._n - f)}</span>`;
}
// intestazione delle schermate secondarie: ✻ titolo + [← indietro] + filo
function thead(title, backTo) {
  label(scr, `${SPARK} ${title}`, 14, C.TEXT, 13, 12);
  if (backTo !== undefined && backTo !== null) {
    const bk = tbtn(scr, `← ${TRS('indietro', 'back')}`, 89, 32, () => requestState(backTo), { color: C.MUTED });
    bk.style.left = '378px'; bk.style.top = '5px';
  }
  obj(scr, 13, 42, 454, 1, { background: C.BORDER });
}
// riga chiave/valore stile terminale (tocco = azione)
function kvRow(parent, key, val, onClick, { color = C.TEXT, valColor = C.ACCENT } = {}) {
  const b = document.createElement('div'); parent.appendChild(b); b.className = 'btn trow';
  Object.assign(b.style, { position: 'relative', flex: '0 0 42px', width: '100%', height: '42px', borderBottom: `1px dashed ${C.GRID}`, cursor: 'pointer' });
  const k = label(b, key, 14, color); align(k, 'LEFT_MID', 13, 0);
  const v = label(b, val, 13, valColor);
  Object.assign(v.style, { right: '13px', top: '50%', transform: 'translateY(-50%)' });
  b.addEventListener('click', () => { touched(); onClick && onClick(v, k); });
  return b;
}
const shortId = (id) => id.replace(/^claude-/, '').replace(/-\d{8}$/, '').replace(/-(\d+)-(\d+)$/, '-$1.$2');

// ---------- swipe / drag guard ----------
let lastDragAt = 0;
const dragGuard = () => realMs() - lastDragAt < 60;

// ============================================================
// Tastiera LVGL (tema chiaro di default) + textarea
// ============================================================
let activeTA = null;
function textarea(parent, w, h, { placeholder = '', password = false, maxLen = 64, text = '' } = {}) {
  const ta = obj(parent, null, null, w, h); ta.classList.add('ta', 'focus');
  ta._val = text; ta._ph = placeholder; ta._pw = password; ta._max = maxLen;
  ta.render = () => {
    const shown = ta._pw ? '•'.repeat(ta._val.length) : ta._val;
    ta.innerHTML = (shown ? `<span>${escapeHtml(shown)}</span>` : `<span class="ph">${escapeHtml(ta._ph)}</span>`) + '<span class="cur"></span>';
  };
  ta.insert = (s) => { if (ta._val.length < ta._max) { ta._val += s; ta.render(); } };
  ta.del = () => { ta._val = ta._val.slice(0, -1); ta.render(); };
  ta.render();
  activeTA = ta;
  return ta;
}
function escapeHtml(s) { return s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
function keyboard(parent, ta, onReady, onCancel) {
  const kb = obj(parent, 0, null, 480, 160); kb.classList.add('kb'); kb.style.bottom = '0';
  let mode = 'lower';
  const layouts = {
    lower: [['1#', ...'qwertyuiop', '⌫'], ['ABC', ...'asdfghjkl', '↵'], ['_', '-', ...'zxcvbnm', '.', ',', ':'], ['✕', '←', ' ', '→', '✓']],
    upper: [['1#', ...'QWERTYUIOP', '⌫'], ['abc', ...'ASDFGHJKL', '↵'], ['_', '-', ...'ZXCVBNM', '.', ',', ':'], ['✕', '←', ' ', '→', '✓']],
    spec: [[...'1234567890', '⌫'], ['abc', '+', '&', '/', '*', '=', '%', '!', '?', '#', '<', '>'], ['\\', '@', '$', '(', ')', '{', '}', '[', ']', ';', '"', "'"], ['✕', '←', ' ', '→', '✓']],
  };
  const draw = () => {
    kb.innerHTML = '';
    for (const row of layouts[mode]) {
      const r = document.createElement('div'); r.className = 'kr'; kb.appendChild(r);
      for (const k of row) {
        const b = document.createElement('div'); b.className = 'k'; r.appendChild(b);
        const special = ['1#', 'ABC', 'abc', '⌫', '↵', '✕', '←', '→'].includes(k);
        if (special) b.classList.add('sp');
        if (k === '✓') b.classList.add('ok');
        if (k === ' ') b.style.flex = '4';
        const iconMap = { '⌫': icon('back', 15), '↵': icon('enter', 15), '✕': icon('keyboard', 16), '←': icon('left', 14), '→': icon('right', 14), '✓': icon('ok', 15) };
        b.innerHTML = iconMap[k] || escapeHtml(k);
        b.addEventListener('pointerdown', (e) => { e.preventDefault(); e.stopPropagation(); press(k); });
      }
    }
  };
  const press = (k) => {
    touched();
    if (k === '1#') { mode = 'spec'; draw(); }
    else if (k === 'ABC') { mode = 'upper'; draw(); }
    else if (k === 'abc') { mode = 'lower'; draw(); }
    else if (k === '⌫') ta.del();
    else if (k === '✓' || k === '↵') onReady && onReady(ta._val);
    else if (k === '✕') onCancel && onCancel();
    else if (k === '←' || k === '→') { /* cursore: non simulato */ }
    else ta.insert(k);
  };
  kb._press = press;
  draw();
  return kb;
}
document.addEventListener('keydown', (e) => {
  if (e.target.closest && e.target.closest('.panel, .web-modal')) return;
  if (activeTA && activeTA.isConnected && activeTA.offsetParent !== null && !activeTA.classList.contains('hidden')) {
    const kb = activeTA._kb;
    if (e.key === 'Backspace') { activeTA.del(); e.preventDefault(); }
    else if (e.key === 'Enter') { kb && kb._press('✓'); e.preventDefault(); }
    else if (e.key === 'Escape') { kb && kb._press('✕'); }
    else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) { activeTA.insert(e.key); e.preventDefault(); }
    return;
  }
  if (state === ST.PIN || state === ST.SETUP_PIN) {
    if (/^[0-9]$/.test(e.key)) pinKey(e.key);
    else if (e.key === 'Backspace') pinKey('<');
    else if (e.key === 'Enter') pinKey('ok');
    return;
  }
  if (state === ST.MAIN && UI.tv) {
    if (e.key === 'ArrowRight') setTile(Math.min(NTILES - 1, G.curTile + 1), true);
    if (e.key === 'ArrowLeft') setTile(Math.max(0, G.curTile - 1), true);
  }
});

// ============================================================
// Schermata: PIN
// ============================================================
let pinDots = null, pinMsg = null;
function pinUpdateDots() {
  if (!pinDots) return;
  const len = G.pinEntry.length;
  pinDots.textContent = '[ ' + Array.from({ length: CFG.PIN_LEN }, (_, i) => (i < len ? '*' : '_')).join(' ') + ' ]';
}
function pinSubmit() {
  if (state === ST.SETUP_PIN) {
    if (!G.pinConfirming) {
      G.pinFirst = G.pinEntry; G.pinConfirming = true; G.pinEntry = ''; pinUpdateDots();
      setText(pinMsg, TRS('Conferma il PIN', 'Confirm the PIN')); return;
    }
    if (G.pinFirst !== G.pinEntry) {
      G.pinConfirming = false; G.pinFirst = G.pinEntry = ''; pinUpdateDots();
      setText(pinMsg, TRS('Non coincide. Reimpostalo.', "Didn't match. Set it again.")); return;
    }
    const slot = G.tokenTargetSlot;
    G.accts.used[slot] = true; G.accts.label[slot] = uniqueLabel(G.pendingLabel || `Account ${slot + 1}`, slot); G.accts.active = slot;
    G.pendingLabel = ''; G.sessionPin = G.pinEntry; G.pendingToken = '';
    G.hasToken = true; G.onboarding = false; P.pinatt = 0; G.pinConfirming = false; G.pinFirst = G.pinEntry = '';
    G.userPin = G.sessionPin;
    slog('[PIN] token cifrato e salvato');
    requestState(G.wifiConnected ? ST.LOADING : ST.WIFI);
    return;
  }
  if (G.pinEntry === (G.userPin || DEMO_PIN)) {
    P.pinatt = 0; G.sessionPin = G.pinEntry; G.pinEntry = '';
    slog('[PIN] ok, token decifrato');
    requestState(G.wifiConnected ? ST.LOADING : ST.WIFI);
  } else {
    P.pinatt++; G.pinEntry = ''; pinUpdateDots();
    if (P.pinatt >= CFG.MAX_PIN_ATTEMPTS) { slog('[PIN] limite superato -> wipe'); factoryReset(); requestState(ST.WIFI); return; }
    const wait = Math.min(3600, CFG.LOCKOUT_BASE_SEC * (1 << (P.pinatt - 1)));
    G.lockoutUntil = millis() + wait * 1000;
    setText(pinMsg, TRS(`PIN errato (${P.pinatt}/${CFG.MAX_PIN_ATTEMPTS}). Attendi ${wait}s`, `Wrong PIN (${P.pinatt}/${CFG.MAX_PIN_ATTEMPTS}). Wait ${wait}s`));
  }
}
function pinKey(k) {
  touched();
  if (millis() < G.lockoutUntil) return;
  const len = G.pinEntry.length;
  if (k === '<') { G.pinEntry = G.pinEntry.slice(0, -1); pinUpdateDots(); }
  else if (k === 'ok') { if (len === CFG.PIN_LEN) pinSubmit(); }
  else if (len < CFG.PIN_LEN) { G.pinEntry += k; pinUpdateDots(); if (len + 1 === CFG.PIN_LEN) pinSubmit(); }
}
function uiPin() {
  const title = state === ST.SETUP_PIN
    ? (G.pinConfirming ? TRS('conferma il PIN', 'confirm the PIN') : TRS('imposta un PIN', 'set a PIN'))
    : TRS('inserisci il PIN', 'enter the PIN');
  align(label(scr, `${SPARK} ${title}`, 15, C.TEXT), 'TOP_MID', 0, 14);
  pinDots = label(scr, '', 26, C.ACCENT); align(pinDots, 'TOP_MID', 0, 42); pinUpdateDots();
  const sub = state === ST.SETUP_PIN ? TRS('lo inserirai a ogni avvio', "you'll type it on every boot") : TRS('serve per sbloccare il token', 'needed to unlock the token');
  pinMsg = label(scr, sub, 12, C.MUTED); align(pinMsg, 'TOP_MID', 0, 84);
  const bm = obj(scr, 110, 112, 260, 200, { display: 'grid', gridTemplateColumns: 'repeat(3,1fr)', gap: '6px' });
  for (const k of ['1', '2', '3', '4', '5', '6', '7', '8', '9', '<', '0', 'ok']) {
    const b = document.createElement('div'); bm.appendChild(b); b.className = 'tbtn';
    const isOk = k === 'ok';
    Object.assign(b.style, { border: `1px solid ${isOk ? C.ACCENT : C.BORDER}`, borderRadius: '4px', color: isOk ? C.ACCENT : k === '<' ? C.MUTED : C.TEXT,
      display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: '20px', cursor: 'pointer' });
    b.textContent = k === '<' ? '⌫' : isOk ? '↵' : k;
    b.addEventListener('pointerdown', (e) => { e.preventDefault(); b.style.background = C.SURFACE2; pinKey(k); });
    b.addEventListener('pointerup', () => { b.style.background = ''; });
    b.addEventListener('pointerleave', () => { b.style.background = ''; });
  }
  if (millis() < G.lockoutUntil) setText(pinMsg, TRS(`attendi ${Math.ceil((G.lockoutUntil - millis()) / 1000)}s`, `wait ${Math.ceil((G.lockoutUntil - millis()) / 1000)}s`));
}

// ============================================================
// Schermata: WiFi
// ============================================================
const NETS = ['Innova-Studio', 'FASTWEB-5G-A1C3', 'TIM-29384711', 'Vodafone-WiFi', 'iPhone di Luca', 'Ospiti'];
function uiWifi() {
  const canBack = !G.onboarding && G.hasToken;
  thead(TRS('configura wifi', 'configure wifi'), canBack ? (G.usage.ok ? ST.MAIN : ST.SETTINGS) : null);
  const status = label(scr, '...', 12, C.MUTED, 12, 50);
  let sel = '';
  const list = obj(scr, 13, 72, 454, 244, { display: 'flex', flexDirection: 'column' }); list.classList.add('scroll');
  const ta = textarea(scr, 456, 40, { placeholder: TRS('password del wifi', 'wifi password'), password: true });
  align(ta, 'TOP_MID', 0, 72); ta.style.display = 'none';
  const kb = keyboard(scr, ta, (pass) => {
    setText(status, `${GT} ${TRS('connessione...', 'connecting...')}`);
    kb.style.display = 'none'; ta.style.display = 'none';
    setTimeout(() => {
      if (pass.length >= 8) {
        G.wifiConnected = true; G.ssid = sel; slog(`[WIFI] connesso a ${sel}`);
        requestState(G.onboarding ? ST.TOKEN : ST.LOADING);
      } else {
        setText(status, TRS('non riuscito: tocca di nuovo una rete', 'failed: tap a network again'), C.BAD);
        list.style.display = '';
      }
    }, 1200);
  }, () => {
    kb.style.display = 'none'; ta.style.display = 'none'; list.style.display = '';
    setText(status, TRS('tocca la tua rete', 'tap your network'), C.MUTED);
  });
  kb.style.display = 'none'; ta._kb = kb;
  const bars = ['▂▄▆█', '▂▄▆█', '▂▄▆', '▂▄▆', '▂▄', '▂'];
  const populate = () => {
    list.innerHTML = ''; setText(status, `<span class="brl" style="color:${C.ACCENT}">⠋</span> ${TRS('ricerca reti...', 'scanning networks...')}`, C.MUTED);
    setTimeout(() => {
      NETS.forEach((n, i) => {
        kvRow(list, escapeHtml(n), bars[i], () => {
          sel = n;
          setText(status, TRS(`password di "${escapeHtml(n)}":`, `password for "${escapeHtml(n)}":`), C.MUTED);
          list.style.display = 'none'; ta._val = ''; ta.render(); ta.style.display = 'flex'; kb.style.display = 'flex';
        }, { valColor: C.ACCENT });
      });
      setText(status, TRS('tocca la tua rete', 'tap your network'), C.MUTED);
    }, 700);
  };
  const rb = tbtn(scr, `↻ ${TRS('cerca', 'scan')}`, 92, 32, populate, { color: C.ACCENT });
  rb.style.left = (canBack ? 278 : 375) + 'px'; rb.style.top = '5px';
  populate();
}

// ============================================================
// Schermata: token (server web locale) + pagina web simulata
// ============================================================
let tokMsg = null;
const DEVICE_IP = '192.168.1.42';
const WEB_SPARK = `<svg class=spark viewBox='0 0 100 100'><g stroke='#D97757' stroke-width='12' stroke-linecap='round'><line x1=50 y1=9 x2=50 y2=91 /><line x1=9 y1=50 x2=91 y2=50 /><line x1=21 y1=21 x2=79 y2=79 /><line x1=79 y1=21 x2=21 y2=79 /><line x1=34 y1=11 x2=66 y2=89 /><line x1=66 y1=11 x2=34 y2=89 /></g></svg>`;
function uiToken() {
  if (!G.onboarding && G.hasToken) {
    const bk = tbtn(scr, `← ${TRS('indietro', 'back')}`, 89, 32, () => requestState(G.usage.ok ? ST.MAIN : ST.SETTINGS), { color: C.MUTED });
    bk.style.left = '378px'; bk.style.top = '5px';
  }
  align(buildClaudeMark(scr), 'TOP_MID', 0, 16);
  align(label(scr, `${GT} ${TRS('incolla il token dal browser, su:', 'paste the token from a browser, at:')}`, 13, C.MUTED), 'TOP_MID', 0, 118);
  const ip = label(scr, `http://${DEVICE_IP}`, 22, C.ACCENT); align(ip, 'TOP_MID', 0, 142);
  ip.style.cursor = 'pointer'; ip.title = 'Apri la pagina web del device (simulata)';
  ip.addEventListener('click', openWebForm);
  align(label(scr, TRS('stessa rete wifi del dispositivo', 'same wifi network as the device'), 11, C.FAINT), 'TOP_MID', 0, 174);
  const w = label(scr, '', 13, C.MUTED); align(w, 'BOTTOM_MID', 0, -24);
  w.innerHTML = `<span class="brl" style="color:${C.ACCENT}">⠋</span> <span id="tokMsg">${TRS('in attesa del token', 'waiting for the token')}</span>`;
  tokMsg = w.querySelector('#tokMsg');
  slog(`[WEB] servidor em http://${DEVICE_IP}`);
}
function openWebForm() {
  $('webUrl').textContent = `http://${DEVICE_IP}/`;
  $('webPage').innerHTML = `<div class=card><h1>${WEB_SPARK} Ritmo Code</h1>
    <p>Incolla il tuo token OAuth di Claude (<code>sk-ant-oat01-...</code>) e tocca <b>Salva</b>. Il dispositivo <b>verificherà</b> il token e chiederà un PIN sullo schermo.</p>
    <form id=tf><input name=label maxlength=16 placeholder='etichetta account (es.: Personale, Lavoro)' autocomplete=off>
    <textarea name=token placeholder='sk-ant-oat01-...' autocomplete=off autofocus></textarea>
    <button type=submit>Salva e verifica</button></form></div>`;
  $('webModal').hidden = false;
  $('tf').addEventListener('submit', (e) => {
    e.preventDefault();
    const fd = new FormData(e.target);
    handleTokenPost(String(fd.get('label') || '').trim(), String(fd.get('token') || '').trim());
  });
}
function webResult(ok, msg) {
  $('webUrl').textContent = `http://${DEVICE_IP}/token`;
  $('webPage').innerHTML = ok
    ? `<div class=card><h1>${WEB_SPARK} Token verificato</h1><p>Token accettato dalla API. Ora <b>imposta un PIN di 4 cifre</b> sullo schermo del dispositivo per completare. Puoi chiudere questa pagina.</p></div>`
    : `<div class=card><h1>${WEB_SPARK} Token rifiutato</h1><p>${msg}</p><p><a href='#' id=again>Torna indietro e riprova</a></p></div>`;
  const a = $('again'); if (a) a.addEventListener('click', (e) => { e.preventDefault(); openWebForm(); });
}
function handleTokenPost(lbl, tok) {
  if (state !== ST.TOKEN) return;
  G.pendingLabel = lbl.slice(0, 16);
  if (tok.length < 8) { setText(tokMsg, TRS('token vuoto', 'empty token')); webResult(false, 'Token vuoto o troppo corto.'); return; }
  setText(tokMsg, TRS('verifica del token...', 'validating token...'));
  setTimeout(() => {
    const ok = tok.startsWith('sk-ant-') && !API.fail;
    if (ok) {
      G.pendingToken = tok; G.usage = apiFetch();
      G.pinConfirming = false; G.pinFirst = G.pinEntry = '';
      setText(tokMsg, TRS('token OK! imposta il PIN', 'token OK! set the PIN'));
      webResult(true);
      if (G.onboarding || !G.sessionPin) requestState(ST.SETUP_PIN); else finalizePendingToken();
    } else {
      setText(tokMsg, TRS('token rifiutato, riprova', 'token rejected, try again'));
      webResult(false, 'La API ha rifiutato il token (auth_failed). Controllalo e incollalo di nuovo.');
    }
  }, 900);
}
$('webClose').addEventListener('click', () => { $('webModal').hidden = true; });

// ============================================================
// Loading / messaggi / boot
// ============================================================
function uiMessage(title, sub, color) {
  align(label(scr, `✕ ${title.toLowerCase()}`, 20, color), 'CENTER', 0, -14);
  if (sub) align(label(scr, `${GT} ${escapeHtml(sub)}`, 13, C.MUTED), 'CENTER', 0, 18);
}
function uiLoading(sub) {
  align(buildClaudeMark(scr), 'CENTER', 0, -52);
  align(label(scr, `<span class="brl" style="color:${C.ACCENT}">⠋</span> ${TRS('caricamento utilizzo', 'loading usage')}`, 15, C.TEXT), 'CENTER', 0, 26);
  if (sub) align(label(scr, escapeHtml(sub), 12, C.FAINT), 'CENTER', 0, 50);
}
let bootSub = null;
function bootSplash(sub) {
  clearScreen();
  align(buildClaudeMark(scr), 'CENTER', 0, -52);
  align(label(scr, `<span class="brl" style="color:${C.ACCENT}">⠋</span> ritmo code`, 15, C.TEXT), 'CENTER', 0, 26);
  bootSub = label(scr, sub, 12, C.FAINT); align(bootSub, 'CENTER', 0, 50);
}

// ============================================================
// Storico / heatmap
// ============================================================
function histPush(h5, d7) {
  G.hist.push({ t: nowEpoch(), h5: Math.round(h5), d7: Math.round(d7) });
  if (G.hist.length > HIST_MAX) G.hist.shift();
}
const dayKey = (ep = nowEpoch()) => Math.floor((ep + tzOffsetSec(ep)) / 86400);
function daySlot(dk) {
  let d = G.days.find((x) => x.day === dk);
  if (d) return d;
  if (G.days.length === NDAYS) G.days.shift();
  d = { day: dk, burn: new Array(24).fill(0) }; G.days.push(d);
  return d;
}
function accumulateHeat(h5) {
  if (G.lastH5 >= 0) {
    const d = h5 - G.lastH5;
    if (d > 0 && d < 100) {
      const hr = localParts(nowEpoch()).h;
      G.hourBurn[hr] += d; daySlot(dayKey()).burn[hr] += d;
    }
  }
  G.lastH5 = h5;
}
function heatModeData(mode) {
  if (mode === 3) return G.hourBurn.slice();
  const out = new Array(24).fill(0), today = dayKey();
  const minDay = mode === 0 ? today : mode === 1 ? today - 6 : today - 29;
  for (const d of G.days) if (d.day >= minDay && d.day <= today) for (let h = 0; h < 24; h++) out[h] += d.burn[h];
  return out;
}
// dati di partenza: 30 giorni di "ritmo" + storico della finestra 5h corrente
function seedHistory() {
  G.hist = []; G.days = []; G.hourBurn = new Array(24).fill(0); G.lastH5 = -1;
  const prof = [0, 0, 0, 0, 0, 0, 1, 2, 5, 9, 12, 10, 4, 6, 11, 14, 12, 8, 5, 3, 2, 1, 0, 0];
  const today = dayKey();
  for (let k = 29; k >= 0; k--) {
    const d = { day: today - k, burn: prof.map((v) => Math.max(0, v * (0.4 + Math.random() * 1.1) * (k % 7 >= 5 ? 0.25 : 1))) };
    if (k === 0) { const h = localParts(nowEpoch()).h; d.burn = d.burn.map((v, i) => (i <= h ? v : 0)); }
    G.days.push(d);
    d.burn.forEach((v, i) => { G.hourBurn[i] += v; });
  }
  for (let i = 0; i < 24; i++) G.hourBurn[i] *= 3.2;              // "tutto" > 30 giorni
  const we = nowEpoch() + API.r5min * 60, ws = we - 5 * 3600, now = nowEpoch();
  const start = Math.max(ws, now - 3 * 3600);
  const from = Math.max(0, API.h5 - 30);
  for (let t = start; t < now; t += 120) {
    const p = (t - start) / (now - start);
    G.hist.push({ t, h5: Math.round(from + (API.h5 - from) * Math.pow(p, 1.3)), d7: API.d7 });
  }
  if (G.hist.length > HIST_MAX) G.hist = G.hist.slice(-HIST_MAX);
  G.lastH5 = API.h5;
}

// ============================================================
// "Rete": fetchUsage / probeModel / fetchModelStatus simulati
// ============================================================
let apiResetBase = null;
function apiFetch() {
  const now = nowEpoch();
  if (!apiResetBase) apiResetBase = { h5: now + API.r5min * 60, d7: now + API.r7h * 3600 };
  // la finestra "scade" nel tempo simulato -> nuova finestra, uso azzerato
  if (now >= apiResetBase.h5) { apiResetBase.h5 += 5 * 3600; API.h5 = 0; syncPanel(); slog('[API] finestra 5h azzerata'); }
  if (now >= apiResetBase.d7) { apiResetBase.d7 += 7 * 86400; API.d7 = 0; syncPanel(); }
  if (API.fail) return { ok: false, error: 'auth_failed' };
  const u = { ok: true, h5: API.h5, d7: API.d7, h5Reset: apiResetBase.h5, d7Reset: apiResetBase.d7, statusOverall: API.ust, error: '' };
  slog(`[API] 5h:${u.h5.toFixed(0)}%  7d:${u.d7.toFixed(0)}%  overall:${u.statusOverall}`);
  return u;
}
// ID che la API "conosce" nel simulatore: qualunque altro risponde 404 (-> chip "ID?")
const KNOWN_MODELS = new Set(['claude-haiku-4-5-20251001', 'claude-sonnet-5', 'claude-opus-5', 'claude-fable-5-1',
  'claude-opus-4-8', 'claude-sonnet-4-5', 'claude-haiku-4-5']);
function probeNextModel() {
  const i = G.probeIdx % 4; G.probeIdx++;
  const s = KNOWN_MODELS.has(G.models[i].id) ? API.probe[i] : { code: 404, ms: 300 };
  G.models[i].pr = { code: s.code, ms: s.code > 0 ? Math.round(s.ms * (0.8 + Math.random() * 0.4)) : 0 };
  // ultime 7 latenze per la mini-linea (sul firmware: 7 byte per modello)
  const lh = G.models[i].lh || (G.models[i].lh = []);
  if (s.code > 0) { lh.push(G.models[i].pr.ms); if (lh.length > 7) lh.shift(); }
  slog(`[PROBE] ${G.models[i].id} -> HTTP ${s.code} (${G.models[i].pr.ms}ms)`);
}
// stessa validazione del firmware (model_id_valid / model_set_id)
function modelSetId(i, raw) {
  const v = String(raw).replace(/\s+/g, '').toLowerCase();
  const m = G.models[i];
  let next;
  if (!v || v === m.defId) next = m.defId;
  else if (/^[a-z0-9._-]{3,47}$/.test(v)) next = v;
  else return false;
  if (next !== m.id) { m.id = next; m.pr = { code: 0, ms: 0 }; slog(`[MODEL] ${m.name} -> ${m.id}`); }
  return true;
}
function fetchModelStatus() {
  if (!API.statusOk) return;
  G.status = { ok: true, up: API.incident.map((x) => !x) };
}

// ============================================================
// Dashboard
// ============================================================
let UI = {};
let hdrStatus = null;
const masc = [];

const statusColor = (s) => (!s ? C.MUTED : ['rejected', 'rate_limited', 'exceeded'].includes(s) ? C.BAD : s.includes('warning') ? C.WARN : C.OK);
function overallLabel(s) {
  if (!s) return '--';
  if (s === 'allowed') return 'OK';
  if (s.includes('warning')) return TRS('ATTENZIONE', 'WARNING');
  if (s === 'rejected') return TRS('BLOCCATO', 'BLOCKED');
  return s;
}
function modelMood(i) {
  const c = G.models[i].pr.code;
  if (!G.status.up[i]) return 3;
  if (c === 0) return 0; if (c === 200) return 1; if (c === 429) return 2; if (c === 404) return 4;
  return 3;
}
function modelChip(i) {
  const { code: c, ms } = G.models[i].pr;
  if (c === 0) return ['--', C.MUTED];
  if (c === 200) return [`OK ${(ms / 1000).toFixed(1)}s`, C.OK];
  if (c === 429) return [TRS('LIMITATO', 'LIMITED'), C.WARN];
  if (c === 404) return ['ID?', C.WARN];                 // modello inesistente: ID da aggiornare
  if (c === 401 || c === 403) return ['AUTH', C.BAD];
  if (c < 0) return [TRS('RETE', 'NET'), C.BAD];
  return [TRS(`ERRORE ${c}`, `ERR ${c}`), C.BAD];
}
function statusWord(s) {
  if (!s) return ['--', C.MUTED];
  if (s === 'allowed') return ['ok', C.OK];
  if (s.includes('warning')) return [TRS('attenzione', 'warning'), C.WARN];
  if (s === 'rejected') return [TRS('bloccato', 'blocked'), C.BAD];
  return [s, C.MUTED];
}
function modelStat(i) {
  const { code: c } = G.models[i].pr;
  if (!G.status.up[i]) return [TRS('incid.', 'incid.'), C.BAD];
  if (c === 0) return ['--', C.FAINT];
  if (c === 200) return ['ok', C.OK];
  if (c === 429) return ['429', C.WARN];
  if (c === 404) return ['id?', C.WARN];
  if (c === 401 || c === 403) return ['auth', C.BAD];
  if (c < 0) return [TRS('rete', 'net'), C.BAD];
  return [String(c), C.BAD];
}
const SPK = '▁▂▃▄▅▆▇█';
function sparkText(arr) {
  const s = (arr || []).map((v) => SPK[clamp(Math.floor(v / 4000 * 8), 0, 7)]).join('');
  return '·'.repeat(7 - s.length) + s;
}

// ---- tile 0: ora ----
function buildWin(t, x, legend, key) {
  const b = tbox(t, x, 14, 223, 197, legend);
  const pct = label(b, '', 54, C.OK, 14, 17); pct.classList.add('pctl'); UI['pct' + key] = pct;
  UI['blk' + key] = blocksLabel(b, 14, 95);
  const at = label(b, '', 11, C.MUTED, 14, 124);
  Object.assign(at.style, { width: '195px', display: 'flex', justifyContent: 'space-between' });
  UI['at' + key] = at;
  UI['cd' + key] = label(b, '', 22, C.TEXT, 14, 145); UI['cd' + key].style.fontWeight = '500';
}
function buildTileAgora(t) {
  buildWin(t, 13, TRS('finestra 5h', '5h window'), 5);
  buildWin(t, 244, TRS('settimana', 'week'), 7);
  UI.prompt = label(t, '', 12, C.MUTED, 13, 232);
  // ritmo settimanale: % usata meno % di settimana trascorsa
  UI.pace = label(t, '', 12, C.MUTED, 199, 232); Object.assign(UI.pace.style, { width: '268px', textAlign: 'right' });
  UI.paceMark = obj(UI.blk7, 0, -3, 2, 24, { background: C.TEXT, display: 'none' });
}
function weekElapsed() {
  if (!G.usage.d7Reset) return -1;
  let re = G.usage.d7Reset;                                   // reset passato, dati non ancora aggiornati
  while (re <= nowEpoch()) re += 7 * 86400;
  return clamp(1 - (re - nowEpoch()) / (7 * 86400), 0, 1);
}
function paceUpdate() {
  if (!UI.pace) return;
  const f = weekElapsed();
  if (f < 0 || !G.usage.ok) { setText(UI.pace, ''); UI.paceMark.style.display = 'none'; return; }
  const used = G.usage.d7Reset <= nowEpoch() ? 0 : G.usage.d7;
  const diff = Math.round(used - f * 100);
  const txt = diff === 0 ? TRS('ritmo sett. in linea', 'week pace on track')
    : `${TRS('ritmo sett.', 'week pace')} ${diff > 0 ? '+' : ''}${diff}%`;
  let out = txt, col = diff <= 0 ? C.OK : diff <= 15 ? C.WARN : C.BAD;
  if (Math.floor(realMs() / 6000) % 2 === 1 && f > 0.02) {       // ogni 6 s: dove arrivi a questo ritmo
    const hIn = f * 168, hLeft = (1 - f) * 168, rate = used / hIn;
    if (used >= 99.5) { out = TRS('quota settimanale esaurita', 'weekly quota used up'); col = C.BAD; }
    else if (rate > 0 && (100 - used) / rate < hLeft) {
      out = `${TRS('a questo ritmo finisce', 'at this pace ends')} ${fmtClock(nowEpoch() + (100 - used) / rate * 3600).toLowerCase()}`; col = C.BAD;
    } else {
      const at = Math.min(100, Math.round(used + rate * hLeft));
      out = TRS(`al reset arrivi al ~${at}%`, `at reset you reach ~${at}%`); col = at >= 90 ? C.WARN : C.OK;
    }
  }
  setText(UI.pace, out, col);
  Object.assign(UI.paceMark.style, { display: 'block', left: `${(f * 100).toFixed(2)}%` });
}

// ---- tile 1: modelli ----
function aggregateMood() {
  const moods = [0, 1, 2, 3].map(modelMood);
  if (moods.includes(3)) return 3;
  if (moods.includes(2)) return 2;
  if (moods.every((m) => m === 0)) return 0;
  if (moods.includes(4)) return 4;
  return 1;
}
function buildMascot(parent, x, y, mood) {
  const c = obj(parent, x, y, 88, 64);
  const fill = mood === 3 ? mix('#6A6A74', C.ACCENT, 190) : mood === 4 ? mix('#6A6A74', C.ACCENT, 170) : mood === 0 ? mix(C.ACCENT, C.BG, 140) : C.ACCENT;
  const img = clawdImg(c, 'md', fill); img.style.left = '0px'; img.style.top = (mood === 3 ? 6 : 2) + 'px';
  const E = EYES.md, ey = E.y + 2;
  const m = { cont: c, baseY: y, mood, lid: [], drop: null };
  if (mood === 1) {
    for (const ex of E.x) { const l = rrect(c, ex - 1, ey - 1, E.w + 2, E.h + 2, 0, C.ACCENT); l.style.display = 'none'; m.lid.push(l); }
  } else if (mood === 2) {
    m.drop = rrect(c, 72, 6, 6, 10, 3, C.BLUE);
  } else if (mood === 3) {
    xEyes(c, E, 2, 3, C.BAD, 2, 2, 6);
  } else if (mood === 4) {
    for (const ex of E.x) m.lid.push(rrect(c, ex - 1, ey + E.h / 2, E.w + 2, E.h / 2 + 1, 0, mix('#8A8A94', C.BG, 180)));
  }
  masc.push(m);
}
function buildTileModels(t) {
  buildMascot(t, 12, 4, aggregateMood());
  UI.mSum = label(t, '', 12, C.TEXT, 112, 12);
  UI.mInc = label(t, '', 12, C.MUTED, 112, 32);
  UI.mRows = G.models.map((m, i) => {
    const r = obj(t, 12, 74 + i * 38, 456, 38, { borderBottom: i < 3 ? `1px dashed ${C.GRID}` : 'none' });
    const dot = obj(r, 4, 15, 8, 8, { background: C.FAINT });
    const nm = label(r, '', 13, C.TEXT, 22, 11);
    const sp = label(r, '', 12, C.FAINT, 180, 12); sp.style.letterSpacing = '-1px';
    const lat = label(r, '', 13, C.MUTED, 0, 11); Object.assign(lat.style, { left: 'auto', right: '84px', textAlign: 'right' });
    const st = label(r, '', 13, C.OK, 0, 11); Object.assign(st.style, { left: 'auto', right: '6px', textAlign: 'right' });
    return { dot, nm, sp, lat, st };
  });
}

// didascalia in fondo ai riquadri grafici, sotto un filo (come box_caption del firmware)
function boxCaption(b, txt) {
  obj(b, 13, 190, 426, 1, { background: C.TRACK });
  const l = label(b, txt || '', 12, C.MUTED, 13, 197);
  Object.assign(l.style, { width: '426px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' });
  return l;
}
// ---- tile 2: finestra 5h ----
const TR = { X0: 36, Y0: 13, W: 404, H: 144 };
const trX = (tt, ws, we) => (we <= ws ? TR.X0 : TR.X0 + clamp(Math.floor((tt - ws) * TR.W / (we - ws)), 0, TR.W));
const trY = (p) => TR.Y0 + TR.H - Math.floor(clamp(p, 0, 100) * TR.H / 100);
function buildTileTrend(t) {
  const b = tbox(t, 13, 14, 454, 230, TRS('finestra 5h · uso + proiezione', '5h window · usage + projection'));
  for (const p of [25, 50, 75]) obj(b, TR.X0, trY(p), TR.W, 0, { borderTop: `1px dashed ${C.GRID}` });
  obj(b, TR.X0, trY(0), TR.W, 1, { background: C.BORDER });
  for (const p of [0, 50, 100]) { const l = label(b, String(p), 10, C.FAINT, 0, trY(p) - 7); Object.assign(l.style, { width: '28px', textAlign: 'right' }); }
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  Object.assign(svg.style, { position: 'absolute', left: 0, top: 0, width: '454px', height: '230px', overflow: 'visible' });
  svg.innerHTML = `<polyline fill="none" stroke="${C.ACCENT}" stroke-width="2" stroke-linejoin="miter"/>
    <line stroke="${C.ACCENT}" stroke-width="2" stroke-dasharray="4 4"/>`;
  b.appendChild(svg);
  UI.trHist = svg.querySelector('polyline'); UI.trProj = svg.querySelector('line');
  UI.trDot = rrect(b, 0, 0, 7, 7, 0, C.TEXT); UI.trDot.style.display = 'none';
  UI.trT0 = label(b, '', 11, C.FAINT, TR.X0, TR.Y0 + TR.H + 8);
  UI.trT1 = label(b, '', 11, C.FAINT, 0, TR.Y0 + TR.H + 8); Object.assign(UI.trT1.style, { left: 'auto', right: '14px' });
  UI.trCap = boxCaption(b);
}
function trendCap(txt, color) { setText(UI.trCap, `${GT} ${txt}`, color); }
function trendRedraw() {
  if (!UI.trHist) return;
  const now = nowEpoch(), we = G.usage.h5Reset;
  const hideProj = () => UI.trProj.setAttribute('visibility', 'hidden');
  if (!we) {
    UI.trHist.setAttribute('points', ''); hideProj(); UI.trDot.style.display = 'none';
    trendCap(TRS('in attesa dei dati della finestra...', 'waiting for window data...'), C.MUTED); return;
  }
  const ws = we - 5 * 3600;
  setText(UI.trT0, fmtHm(ws)); setText(UI.trT1, fmtHm(we));
  const pts = [];
  for (const s of G.hist) { if (!s.t || s.t < ws || s.t > now) continue; pts.push([trX(s.t, ws, we), trY(s.h5)]); }
  const nowC = Math.min(now, we);
  pts.push([trX(nowC, ws, we), trY(G.usage.h5)]);
  UI.trHist.setAttribute('points', pts.map((q) => q.join(',')).join(' '));
  const cx = trX(nowC, ws, we), cy = trY(G.usage.h5);
  UI.trDot.style.left = (cx - 3) + 'px'; UI.trDot.style.top = (cy - 3) + 'px'; UI.trDot.style.display = '';
  if (pts.length < 3) { hideProj(); trendCap(TRS('raccolta dati... (~qualche minuto)', 'collecting data... (~a few minutes)'), C.MUTED); return; }
  let rate = 0;
  const first = G.hist.find((q) => q.t && q.t >= ws && q.t >= now - 2700);
  if (first && now > first.t + 300) rate = (G.usage.h5 - first.h5) / ((now - first.t) / 60);
  const setProj = (x2, y2) => { UI.trProj.setAttribute('x1', cx); UI.trProj.setAttribute('y1', cy); UI.trProj.setAttribute('x2', x2); UI.trProj.setAttribute('y2', y2); UI.trProj.setAttribute('visibility', 'visible'); };
  if (G.usage.h5 >= 99.5) {
    hideProj(); trendCap(TRS(`finestra esaurita · reset tra ${fmtEta(we)}`, `window exhausted · resets in ${fmtEta(we)}`), C.BAD);
  } else if (rate > 0.02) {
    const minsLeft = (100 - G.usage.h5) / rate, etaT = now + Math.floor(minsLeft * 60);
    if (etaT <= we) {
      setProj(trX(etaT, ws, we), trY(100));
      const h = Math.floor(minsLeft / 60), m = pad2(Math.floor(minsLeft % 60));
      trendCap(TRS(`a questo ritmo finisce alle ${fmtHm(etaT)} (tra ${h}h${m}m)`, `at this pace it runs out at ${fmtHm(etaT)} (in ${h}h${m}m)`), minsLeft < 60 ? C.BAD : C.WARN);
    } else {
      const endPct = G.usage.h5 + rate * ((we - now) / 60);
      setProj(trX(we, ws, we), trY(endPct));
      trendCap(TRS(`a questo ritmo NON finisce prima del reset (~${Math.round(endPct)}%)`, `at this pace it does NOT run out before reset (~${Math.round(endPct)}%)`), C.OK);
    }
  } else {
    hideProj(); trendCap(TRS('uso stabile · nessun rischio ora', 'stable usage · no risk right now'), C.OK);
  }
}

// ---- tile 3: ritmo orario ----
function heatBtnStyle() {
  const names = [TRS('oggi', 'today'), TRS('7g', '7d'), TRS('30g', '30d'), TRS('tutto', 'all')];
  (UI.heatBtn || []).forEach((b, i) => {
    const on = i === P.heatm;
    b.style.borderColor = on ? C.ACCENT : C.BORDER;
    b.style.background = on ? mix(C.ACCENT, C.BG, 40) : C.BG;
    setText(b._lbl, names[i], on ? C.ACCENT : C.MUTED);
  });
}
function buildTileHeat(t) {
  const b = tbox(t, 13, 14, 454, 230, TRS('ritmo orario', 'hourly rhythm'));
  UI.heatBtn = [0, 1, 2, 3].map((i) => {
    const btn = tbtn(b, '', 60, 26, () => { if (i === P.heatm) return; P.heatm = i; heatBtnStyle(); heatRedraw(); }, { size: 12 });
    btn.style.left = (180 + i * 66) + 'px'; btn.style.top = '12px';
    return btn;
  });
  heatBtnStyle();
  UI.heat = Array.from({ length: 24 }, (_, h) => obj(b, 14 + h * 18, 166, 14, 2, { background: C.ACCENT }));
  obj(b, 14, 168, 428, 1, { background: C.BORDER });
  for (const h of [0, 6, 12, 18, 23]) label(b, `${h}h`, 10, C.FAINT, 12 + h * 18, 174);
  boxCaption(b, `${GT} ${TRS('quota 5h consumata per ora locale', '5h quota burned per local hour')}`);
}
function heatRedraw() {
  if (!UI.heat) return;
  const data = heatModeData(P.heatm);
  const mx = Math.max(1, ...data), cur = localParts(nowEpoch()).h;
  UI.heat.forEach((bar, h) => {
    const r = clamp(data[h] / mx, 0, 1), hgt = 2 + Math.floor(r * 116);
    bar.style.height = hgt + 'px'; bar.style.top = (168 - hgt) + 'px';
    // colore gia' fuso con lo sfondo (niente trasparenza da calcolare)
    bar.style.background = h === cur ? C.TEXT : mix(C.ACCENT, C.BG, 70 + Math.floor(r * 185));
  });
}

// ---- tileview (swipe) + intestazione ----
// ---- tile 4: picco settimanale ----
function seedWeeks() {
  const re = G.usage.d7Reset || (nowEpoch() + API.r7h * 3600);
  const peaks = [48, 71, 55, 93, 62, 80, 58];
  G.weeks = peaks.map((p, i) => ({ reset: re - (peaks.length - i) * 7 * 86400, peak: p }));
  G.weeks.push({ reset: re, peak: Math.round(API.d7) });
}
function weekRecord(d7, reset) {
  if (!reset) return;
  if (!G.weeks) G.weeks = [];
  const last = G.weeks[G.weeks.length - 1], p = Math.round(clamp(d7, 0, 100));
  if (last && Math.abs(reset - last.reset) < 3600) { last.peak = Math.max(last.peak, p); return; }
  if (last && reset < last.reset) return;
  G.weeks.push({ reset, peak: p }); if (G.weeks.length > 12) G.weeks.shift();
}
function buildTileWeeks(t) {
  const b = tbox(t, 13, 14, 454, 230, TRS('picco settimanale · ultime 8', 'weekly peak · last 8'));
  obj(b, 14, 156, 428, 1, { background: C.BORDER });
  UI.wk = Array.from({ length: 8 }, (_, i) => {
    const x = 16 + i * 54;
    const bar = obj(b, x, 154, 40, 2, { background: C.TRACK });
    const val = label(b, '', 11, C.MUTED, x - 6, 134); Object.assign(val.style, { width: '52px', textAlign: 'center' });
    const date = label(b, '', 11, C.FAINT, x - 6, 162); Object.assign(date.style, { width: '52px', textAlign: 'center' });
    return { bar, val, date };
  });
  UI.wkCap = boxCaption(b);
}
function weeksRedraw() {
  if (!UI.wk) return;
  const w = (G.weeks || []).slice(-8), off = 8 - w.length;
  let sum = 0, mx = 0;
  UI.wk.forEach((u, i) => {
    const r = w[i - off];
    if (!r) { Object.assign(u.bar.style, { height: '2px', top: '154px', background: C.TRACK }); setText(u.val, ''); setText(u.date, ''); return; }
    const cur = i === 7, h = 2 + Math.floor(r.peak * 110 / 100);
    Object.assign(u.bar.style, { height: h + 'px', top: (156 - h) + 'px', background: gradColor(r.peak), opacity: cur ? 1 : 0.7 });
    u.val.style.top = (156 - h - 18) + 'px';
    setText(u.val, `${r.peak}%`, cur ? C.TEXT : C.MUTED);
    const p = localParts(r.reset - 7 * 86400);
    setText(u.date, cur ? TRS('ora', 'now') : `${pad2(p.d)}/${pad2(p.mo)}`, cur ? C.TEXT : C.FAINT);
    if (!cur) { sum += r.peak; mx = Math.max(mx, r.peak); }
  });
  const n = w.length;
  setText(UI.wkCap, `${GT} ${n < 2 ? TRS('raccolta dati: ogni settimana conclusa aggiunge una barra', 'collecting data: each finished week adds a bar')
    : TRS(`media ${Math.round(sum / (n - 1))}% · massimo ${mx}% (settimane concluse)`, `average ${Math.round(sum / (n - 1))}% · max ${mx}% (finished weeks)`)}`);
}

// ---- tile 0: home (ora, meteo, riepilogo Claude, PC) ----
const CLOCK_MIN = [0, 2, 5, 10];
// dati di esempio: sul dispositivo arrivano da Open-Meteo e da SmallTV Monitor sul PC
const WX = { ok: true, city: 'Milano', temp: 22, code: 2, day: true, tmax: 23, tmin: 18, tmax2: 22, tmin2: 18, code2: 80,
  sunrise: '07:03', sunset: '19:30', rainH0: 13, rain: [0, 3, 3, 3, 3, 5, 18, 45, 55, 30, 23, 25] };
const PC = { ok: true, host: '192.168.1.10:8765', cpu: 12, cpuT: 56, ram: 61, gpu: 4, gpuT: 35, disk: 57 };
function wxGroup(c) { if (c === 0) return 0; if (c <= 2) return 1; if (c === 3) return 2; if (c === 45 || c === 48) return 3;
  if ((c >= 71 && c <= 77) || c === 85 || c === 86) return 5; if (c >= 95) return 6; if (c >= 51) return 4; return 2; }
function wxDesc(c) {
  const g = wxGroup(c);
  return [TRS('sereno', 'clear'), TRS('poco nuvoloso', 'partly cloudy'), TRS('nuvoloso', 'cloudy'), TRS('nebbia', 'fog'),
    c >= 80 ? TRS('rovesci', 'showers') : c < 60 ? TRS('pioviggine', 'drizzle') : TRS('pioggia', 'rain'), TRS('neve', 'snow'), TRS('temporale', 'storm')][g];
}
function wxIcon(b, code, day) {
  b.innerHTML = '';
  const cloud = (x, y, col) => { rrect(b, x + 2, y + 12, 34, 14, 7, col); rrect(b, x + 8, y + 3, 17, 17, 8, col); rrect(b, x + 19, y + 7, 14, 14, 7, col); };
  const g = wxGroup(code);
  if (g === 0) {
    if (day) { rrect(b, 12, 10, 20, 20, 10, C.WARN); rrect(b, 21, 1, 2, 6, 0, C.WARN); rrect(b, 21, 33, 2, 6, 0, C.WARN); rrect(b, 3, 19, 6, 2, 0, C.WARN); rrect(b, 35, 19, 6, 2, 0, C.WARN); }
    else { rrect(b, 11, 8, 24, 24, 12, C.TEXT); rrect(b, 19, 3, 22, 22, 11, C.BG); }
    return;
  }
  if (g === 1) { if (day) rrect(b, 2, 0, 18, 18, 9, C.WARN); else { rrect(b, 2, 0, 18, 18, 9, C.TEXT); rrect(b, 8, -3, 16, 16, 8, C.BG); } cloud(6, 8, C.MUTED); return; }
  if (g === 3) { rrect(b, 4, 10, 36, 3, 1, C.MUTED); rrect(b, 8, 18, 32, 3, 1, C.MUTED); rrect(b, 4, 26, 36, 3, 1, C.MUTED); return; }
  cloud(3, 0, g === 2 ? C.MUTED : C.FAINT);
  if (g === 4) [11, 20, 29].forEach((x, i) => rrect(b, x, i === 1 ? 32 : 30, 2, 8, 1, C.BLUE));
  if (g === 5) [10, 20, 30].forEach((x, i) => rrect(b, x, i === 1 ? 35 : 32, 4, 4, 2, C.TEXT));
  if (g === 6) b.insertAdjacentHTML('beforeend', `<svg style="position:absolute;left:0;top:0" width="44" height="44"><polyline points="20,26 14,34 22,34 16,42" fill="none" stroke="${C.WARN}" stroke-width="3"/></svg>`);
}
function buildTileHome(t) {
  UI.hm = {};
  const h = UI.hm;
  h.time = label(t, '', 96, C.TEXT, 7, -6); Object.assign(h.time.style, { fontWeight: '500', letterSpacing: '-3px', lineHeight: '1' });
  h.time.style.cursor = 'pointer';   // tocca l'ora: timer e pomodoro
  h.time.addEventListener('click', (e) => { if (realMs() - lastDragAt < 300) return; e.stopPropagation(); tmMenuOpen(); });
  // pomodoro disegnato accanto alla riga sotto l'ora: anche lei apre timer e pomodoro
  const tom = obj(t, 13, 85, 14, 16, { cursor: 'pointer' });
  rrect(tom, 0, 3, 14, 13, 6, C.BAD); rrect(tom, 3, 1, 8, 3, 1, C.OK); rrect(tom, 6, 0, 2, 3, 0, C.OK);
  h.date = label(t, '', 14, C.MUTED, 33, 86); Object.assign(h.date.style, { width: '264px', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', cursor: 'pointer' });
  for (const o of [tom, h.date]) o.addEventListener('click', (e) => { if (realMs() - lastDragAt < 300) return; e.stopPropagation(); tmMenuOpen(); });
  h.icon = obj(t, 310, 9, 44, 44);
  h.temp = label(t, '', 54, C.TEXT, 362, -1); h.temp.classList.add('pctl');
  h.desc = label(t, '', 12, C.MUTED, 310, 56);
  h.rain = label(t, '', 12, C.MUTED, 310, 72);
  h.sun = label(t, '', 12, C.FAINT, 310, 88);
  const b = tbox(t, 13, 112, 454, 75, 'claude');
  h.row = [0, 1].map((i) => {
    const y = 13 + i * 30;
    label(b, i ? TRS('sett.', 'week') : '5h', 14, C.MUTED, 13, y);
    const blk = blocksLabel(b, 57, y, 10, 14);
    const pct = label(b, '', 14, C.TEXT, 149, y);
    const info = label(b, '', 12, C.MUTED, 199, y + 2);
    const right = label(b, '', 12, C.MUTED, 303, y + 2); Object.assign(right.style, { width: '138px', textAlign: 'right' });
    return { blk, pct, info, right };
  });
  h.pcBox = tbox(t, 13, 206, 454, 40, 'pc');
  h.pc = label(h.pcBox, '', 14, C.TEXT, 13, 9);
  homeRedraw();
}
function homeTick() {
  const h = UI.hm; if (!h) return;
  const p = localParts(nowEpoch());
  setText(h.time, `${pad2(p.h)}:${pad2(p.m)}`);
  const GI = ['domenica', 'lunedì', 'martedì', 'mercoledì', 'giovedì', 'venerdì', 'sabato'], GE = ['sunday', 'monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday'];
  const MI = ['gennaio', 'febbraio', 'marzo', 'aprile', 'maggio', 'giugno', 'luglio', 'agosto', 'settembre', 'ottobre', 'novembre', 'dicembre'];
  const ME = ['jan', 'feb', 'mar', 'apr', 'may', 'jun', 'jul', 'aug', 'sep', 'oct', 'nov', 'dec'];
  if (TM.mode) {   // timer in corso: al posto della data, fase e ora di fine (il conto alla rovescia e' nella testata)
    setText(h.date, TRS(`${tmLabel()} · fine ${fmtHm(nowEpoch() + tmLeft())}`, `${tmLabel()} · ends ${fmtHm(nowEpoch() + tmLeft())}`), tmColor());
  } else setText(h.date, `${P.lang ? GE[p.wd] : GI[p.wd]} ${p.d} ${(P.lang ? ME : MI)[p.mo - 1]} · ${WX.city.toLowerCase()}`, C.MUTED);
  setText(h.desc, Math.floor(realMs() / 6000) % 2 === 0 ? `${wxDesc(WX.code)} · ${WX.tmax}°/${WX.tmin}°`
    : `${TRS('domani', 'tomorrow')} ${WX.tmax2}°/${WX.tmin2}° ${wxDesc(WX.code2)}`);
}
function homeRedraw() {
  const h = UI.hm; if (!h) return;
  if (h.code !== WX.code * 2 + WX.day) { h.code = WX.code * 2 + WX.day; wxIcon(h.icon, WX.code, WX.day); }
  setText(h.temp, `${WX.temp}°`);
  const first = WX.rain.findIndex((v) => v >= 40), best = Math.max(...WX.rain);
  setText(h.rain, first >= 0 ? TRS(`pioggia ${WX.rain[first]}% alle ${pad2((WX.rainH0 + first) % 24)}`, `rain ${WX.rain[first]}% at ${pad2((WX.rainH0 + first) % 24)}`)
    : best >= 20 ? TRS(`pioggia possibile ${best}%`, `rain possible ${best}%`) : TRS('niente pioggia nelle 12h', 'no rain in 12h'), first >= 0 ? C.BLUE : C.MUTED);
  setText(h.sun, TRS(`sole ${WX.sunrise}-${WX.sunset}`, `sun ${WX.sunrise}-${WX.sunset}`));
  [[G.usage.h5, G.usage.h5Reset], [G.usage.d7, G.usage.d7Reset]].forEach(([v, re], i) => {
    const r = h.row[i], col = gradColor(v || 0);
    setBlocks(r.blk, G.usage.ok ? v : 0, col);
    setText(r.pct, G.usage.ok ? `${Math.round(v)}%` : '--', col);
    setText(r.info, !G.usage.ok ? '' : i === 0 ? `reset ${fmtHm(re)}` : TRS(`reset tra ${fmtEta(re)}`, `reset in ${fmtEta(re)}`));
  });
  const [w, wc] = statusWord(G.usage.statusOverall);
  setText(h.row[0].right, `${TRS('stato', 'status')} ${w}`, wc);
  const ok = [0, 1, 2, 3].filter((i) => modelMood(i) === 1).length;
  if (P.pause) setText(h.row[1].right, TRS('richieste in pausa', 'requests paused'), C.WARN);
  else setText(h.row[1].right, TRS(`modelli ${ok}/4 ok`, `models ${ok}/4 ok`), ok === 4 ? C.MUTED : C.WARN);
  h.pcBox._lg.textContent = `pc · ${PC.host}`;
  const k = (s) => `<span style="color:${C.MUTED};font-size:12px">${s}</span>`;
  setText(h.pc, `${k('cpu ')}${PC.cpu}% ${PC.cpuT}°&nbsp;&nbsp;&nbsp;${k('ram ')}${PC.ram}%&nbsp;&nbsp;&nbsp;${k('gpu ')}${PC.gpu}% ${PC.gpuT}°&nbsp;&nbsp;&nbsp;${k(TRS('disco ', 'disk '))}${PC.disk}%`);
}

// ---- tile 6: pc (dati di esempio; sul dispositivo arrivano da SmallTV Monitor 1.1) ----
Object.assign(PC, { cpuName: 'amd ryzen 7 9700x', gpuName: 'rtx 3070 ti', mhz: 4730, cpuW: 47, gpuW: 11, ramUsed: 18.4, ramTot: 31.2,
  vramUsed: 1.0, vramTot: 8, down: 2.4 * 1048576, up: 180 * 1024, rd: 235 * 1024, wr: 492 * 1024, uptime: 103639, claude: 7,
  disks: [['C', 57, 199], ['D', 15, 398], ['E', 30, 1300], ['F', 16, 2357], ['G', 59, 190]], fans: [['cpu fan', 1180], ['sys fan 1', 820], ['pump', 2400], ['gpu fan 1', 0]],
  coreMax: 33, volt: 1.18, hot: 45, memT: 40, gpuMhz: 210, memLoad: 12, kwh: 0.30,
  board: [['vrm mos', 46], ['chipset', 51], ['system', 36]], ramT: [38, 37],
  drives: [['samsung ssd 990 pro 2tb', 41, 98], ['wd black sn850x 4tb', 39, 100], ['st2000dm008', 34, null]] });
const PCH = [[], [], []];
(function seedPcHist() {
  for (let i = 0; i < 240; i++) {
    PCH[0].push(Math.round(8 + 6 * Math.sin(i / 7) + (i % 23 === 0 ? 30 : 0) + Math.random() * 5));
    PCH[1].push(Math.round(6 + 4 * Math.sin(i / 11) + (i > 90 && i < 100 ? 45 : 0)));
    PCH[2].push(Math.round(57 + 3 * Math.sin(i / 30)));
  }
})();
function buildTilePc(t) {
  const U = UI.pc = {};
  const sparkBox = (x, y, w, h, sy, sh, col, withSub) => {
    const b = tbox(t, x, y, w, h, ' ');
    const main = label(b, '', 14, C.TEXT, 13, 6);
    const sub = withSub ? label(b, '', 12, C.MUTED, 13, 25) : null;
    obj(b, 13, sy + sh, 197, 1, { background: C.TRACK });
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    Object.assign(svg.style, { position: 'absolute', left: '13px', top: sy + 'px', overflow: 'visible' });
    svg.setAttribute('width', 197); svg.setAttribute('height', sh);
    b.appendChild(svg);
    return { b, main, sub, svg, sh, col };
  };
  U.cpu = sparkBox(13, 20, 223, 89, 46, 34, C.ACCENT, true);
  U.gpu = sparkBox(244, 20, 223, 89, 46, 34, C.OK, true);
  U.ram = sparkBox(13, 130, 223, 55, 10, 35, C.BLUE, false);
  U.net = tbox(t, 244, 130, 223, 55, TRS('rete · disco', 'network · disk'));
  U.netMain = label(U.net, '', 14, C.TEXT, 13, 6);
  U.netSub = label(U.net, '', 12, C.MUTED, 13, 29);
  U.sys = tbox(t, 13, 206, 454, 40, TRS('sistema', 'system'));
  U.sysTxt = label(U.sys, '', 12, C.MUTED, 13, 11);
  pcRedraw();
}
function pcRedraw() {
  const U = UI.pc; if (!U) return;
  const rate = (b) => (b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB/s` : b >= 1024 ? `${Math.round(b / 1024)} KB/s` : `${Math.round(b)} B/s`);
  const spark = (s, arr) => {
    const n = arr.length, pts = arr.map((v, k) => `${(197 - (n - 1 - k) * 197 / 239).toFixed(1)},${(s.sh - v * s.sh / 100).toFixed(1)}`).join(' ');
    s.svg.innerHTML = `<polyline points="${pts}" fill="none" stroke="${s.col}" stroke-width="2"/>`;
  };
  U.cpu.b._lg.textContent = `cpu · ${PC.cpuName}`;
  setText(U.cpu.main, `${PC.cpu}%&nbsp; ${(PC.mhz / 1000).toFixed(2)} GHz&nbsp; ${PC.cpuT}°&nbsp; ${PC.cpuW}W`);
  setText(U.cpu.sub, `core max ${PC.coreMax}% · ${PC.volt.toFixed(2)} V`);
  U.gpu.b._lg.textContent = `gpu · ${PC.gpuName}`;
  setText(U.gpu.main, `${PC.gpu}%&nbsp; ${PC.gpuT}°&nbsp; ${PC.gpuW}W`);
  setText(U.gpu.sub, `vram ${PC.vramUsed.toFixed(1)} / ${PC.vramTot} GB · hot ${PC.hot}°`);
  U.ram.b._lg.textContent = `ram ${PC.ram}% · ${PC.ramUsed.toFixed(1)} / ${Math.round(PC.ramTot)} GB · ${Math.max(...PC.ramT)}°`;
  spark(U.cpu, PCH[0]); spark(U.gpu, PCH[1]); spark(U.ram, PCH[2]);
  setText(U.netMain, `↓ ${rate(PC.down)}&nbsp; ↑ ${rate(PC.up)}`);
  setText(U.netSub, TRS(`disco r ${rate(PC.rd)} w ${rate(PC.wr)}`, `disk r ${rate(PC.rd)} w ${rate(PC.wr)}`));
  const up = PC.uptime, upTxt = up >= 86400 ? TRS(`acceso ${Math.floor(up / 86400)}g ${Math.floor(up % 86400 / 3600)}h`, `up ${Math.floor(up / 86400)}d ${Math.floor(up % 86400 / 3600)}h`)
    : TRS(`acceso ${Math.floor(up / 3600)}h ${Math.floor(up % 3600 / 60)}m`, `up ${Math.floor(up / 3600)}h ${Math.floor(up % 3600 / 60)}m`);
  const w = PC.cpuW + PC.gpuW, eur = (w * 24 / 1000 * PC.kwh).toFixed(2).replace('.', ',');
  const dt = Math.max(...PC.drives.map((d) => d[1])), life = Math.min(...PC.drives.filter((d) => d[2] != null).map((d) => d[2]));
  setText(U.sysTxt, `${upTxt} · claude ${PC.claude} · ${w} W ~${eur} €/${TRS('g', 'd')} · ${TRS('dischi', 'disks')} ${dt}°/${life}%`);
}

// ---- menu della pausa a tempo (tenendo premuto il tasto pausa) ----
let PMENU = null;
function pauseSet(on, until) {
  P.pause = on; P.pauseUntil = on ? until : 0;
  if (!on && millis() - G.lastPollMs > P.poll * 1000) G.wantRefresh = true;
  if (UI.pauseStyle) UI.pauseStyle();
  homeRedraw();
  if (hdrStatus) { hdrStatus._t = null; setHdrStatus(); }
}
function pauseMenuClose() { if (PMENU) { PMENU.remove(); PMENU = null; } }
function pauseMenuOpen() {
  pauseMenuClose();
  const s = obj(scr, 0, 0, 480, 320, { background: 'rgba(20,20,19,.8)', zIndex: 60 });
  s.addEventListener('click', (e) => { e.stopPropagation(); pauseMenuClose(); });
  PMENU = s;
  const b = tbox(s, 90, 60, 300, 200, TRS('pausa richieste', 'pause requests'), C.WARN);
  b.style.background = C.BG; b._lg.style.color = C.WARN;
  b.addEventListener('click', (e) => e.stopPropagation());
  const now = nowEpoch();
  const until7 = () => { const p = localParts(now); let d = now - p.h * 3600 - p.m * 60 - (now % 60) + 7 * 3600; if (p.h >= 7) d += 86400; return d; };
  const opts = [['30 min', now + 1800], [TRS('1 ora', '1 hour'), now + 3600], [TRS('fino alle 7:00', 'until 7:00'), until7()], [TRS('senza limite', 'no limit'), 0]];
  opts.forEach(([n, u], i) => {
    const bt = tbtn(b, n, 124, 42, () => { pauseMenuClose(); pauseSet(true, u); }, { color: i === 3 ? C.WARN : C.TEXT, size: 14 });
    bt.style.left = (20 + (i % 2) * 134) + 'px'; bt.style.top = (22 + Math.floor(i / 2) * 56) + 'px';
  });
  const cancel = tbtn(b, TRS('annulla', 'cancel'), 258, 38, () => pauseMenuClose(), { color: C.MUTED, size: 14 });
  cancel.style.left = '20px'; cancel.style.top = '136px';
}

const TAB_PATH = ['/home', '/usage', '/models', '/window', '/rhythm', '/weeks', '/pc'];
function hdrIdentity() {
  if (!UI.hdrId) return;
  const badge = accountCount() > 1 ? ` <span style="color:${C.ACCENT}">@${escapeHtml(G.accts.label[G.accts.active].slice(0, 10))}</span>` : '';
  UI.hdrId.innerHTML = `${SPARK} ritmo-code <span style="color:${C.FAINT}">${TAB_PATH[G.curTile]}</span>${badge}`;
}
function setTile(i, anim) {
  G.curTile = i;
  UI.track.classList.toggle('anim', !!anim);
  UI.track.style.transform = `translateX(${-i * 480}px)`;
  (UI.tabs || []).forEach((tb, k) => {
    const on = k === i;
    tb.style.color = on ? C.TEXT : C.FAINT;
    tb.textContent = on ? `[${tb.dataset.name}]` : tb.dataset.name;
  });
  hdrIdentity();
}
function attachSwipe(tv) {
  let x0 = null, dx = 0, dragging = false, pid = null;
  tv.addEventListener('pointerdown', (e) => { x0 = e.clientX; dx = 0; dragging = false; pid = e.pointerId; touched(); });
  tv.addEventListener('pointermove', (e) => {
    if (x0 === null || e.pointerId !== pid) return;
    const z = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--zoom')) || 1;
    dx = (e.clientX - x0) / z;
    if (!dragging && Math.abs(dx) > 8) { dragging = true; try { tv.setPointerCapture(pid); } catch (_) {} }
    if (dragging) {
      let off = -G.curTile * 480 + dx;
      off = clamp(off, -(NTILES - 1) * 480 - 40, 40);
      UI.track.classList.remove('anim'); UI.track.style.transform = `translateX(${off}px)`;
    }
  });
  const end = () => {
    if (x0 === null) return;
    if (dragging) {
      lastDragAt = realMs();
      let i = G.curTile;
      if (dx < -60) i = Math.min(NTILES - 1, i + 1); else if (dx > 60) i = Math.max(0, i - 1);
      setTile(i, true);
    }
    x0 = null; dragging = false; touched();
  };
  tv.addEventListener('pointerup', end); tv.addEventListener('pointercancel', end);
}
function setHdrStatus() {
  if (!hdrStatus) return;
  let txt, col;
  if (G.refreshing) { txt = `<span class="brl" style="color:${C.ACCENT}">⠋</span> ${TRS('aggiornamento', 'updating')}`; col = C.ACCENT; }
  else if (TM.mode) { txt = tmLabel(); col = tmColor(); }
  else if (P.pause && P.pauseUntil) { txt = TRS(`pausa fino ${fmtHm(P.pauseUntil)}`, `paused until ${fmtHm(P.pauseUntil)}`); col = C.WARN; }
  else if (P.pause) { txt = TRS('in pausa', 'paused'); col = C.WARN; }
  else if (nightPaused()) { txt = TRS('notte · in pausa', 'night · paused'); col = C.FAINT; }
  else if (!G.lastFetchOk) { txt = TRS('non aggiornato', 'update failed'); col = C.BAD; }
  else {
    const s = Math.floor((millis() - G.lastOkMs) / 1000);
    txt = s < 60 ? TRS('aggiornato ora', 'updated just now') : TRS(`aggiornato ${Math.floor(s / 60)}m fa`, `updated ${Math.floor(s / 60)}m ago`);
    col = C.MUTED;
  }
  if (hdrStatus._t !== txt) { hdrStatus._t = txt; setText(hdrStatus, txt, col); }
}
let logoClick = 0, demoIdx = 0;
function uiMain() {
  masc.length = 0;
  UI.hdrId = label(scr, '', 13, C.TEXT, 13, 13);
  // doppio tocco su "✻ ritmo-code" = demo dei momenti
  const spot = obj(scr, 4, 2, 200, 36, { cursor: 'pointer' });
  spot.addEventListener('click', () => {
    const now = realMs();
    if (now - logoClick < 450) { const T = [25, 50, 70, 100, 0]; G.pendPeak = 0; showMoment(Math.floor(demoIdx / 5) % 2, T[demoIdx % 5]); demoIdx++; logoClick = 0; }
    else logoClick = now;
  });
  hdrStatus = label(scr, '', 12, C.MUTED); Object.assign(hdrStatus.style, { right: '192px', top: '13px', textAlign: 'right' });
  let longT = null, longFired = false;
  const pause = tbtn(scr, '', 52, 32, () => { if (longFired) { longFired = false; return; } pauseSet(!P.pause, 0); });
  pause.addEventListener('pointerdown', () => { longFired = false; longT = setTimeout(() => { longFired = true; pauseMenuOpen(); }, 600); });
  ['pointerup', 'pointerleave'].forEach((ev) => pause.addEventListener(ev, () => clearTimeout(longT)));
  pause.style.left = '301px'; pause.style.top = '5px';
  const pauseStyle = () => {
    pause.style.borderColor = P.pause ? C.WARN : C.BORDER;
    setText(pause._lbl, P.pause ? '▶' : '❚❚', P.pause ? C.WARN : C.TEXT);
  };
  pauseStyle(); UI.pauseStyle = pauseStyle;
  const ref = tbtn(scr, '↻', 52, 32, () => { G.wantRefresh = true; }, { color: C.ACCENT, size: 17 });
  ref.style.left = '358px'; ref.style.top = '5px';
  const gear = tbtn(scr, '≡', 52, 32, () => requestState(ST.SETTINGS), { size: 18 });
  gear.style.left = '415px'; gear.style.top = '5px';
  // filo sotto l'intestazione: la parte argilla e' il conto alla rovescia del refresh
  // filo solo sotto nome e stato, a fianco dei pulsanti: a meta' tra il titolo e i contenuti
  const line = obj(scr, 13, 38, 275, 1, { background: C.BORDER, pointerEvents: 'none' });
  UI.refBar = obj(line, 0, 0, 275, 1, { background: C.ACCENT });

  const tv = obj(scr, 0, 43, 480, 251); tv.classList.add('tv');
  const track = obj(tv, 0, 0, 480 * NTILES, 251); track.classList.add('track');
  UI.tv = tv; UI.track = track;
  const tiles = Array.from({ length: NTILES }, () => { const t = document.createElement('div'); t.className = 'tile'; track.appendChild(t); return t; });
  buildTileHome(tiles[0]); buildTileAgora(tiles[1]); buildTileModels(tiles[2]); buildTileTrend(tiles[3]); buildTileHeat(tiles[4]); buildTileWeeks(tiles[5]); buildTilePc(tiles[6]);

  // schede in basso: anche toccabili
  const names = ['home', TRS('ora', 'now'), TRS('modelli', 'models'), '5h', TRS('ritmo', 'rhythm'), TRS('settimane', 'weeks'), 'pc'];
  const bar = obj(scr, 0, 294, 480, 21, { display: 'flex', justifyContent: 'center', alignItems: 'center', gap: '4px', fontSize: '12px' });
  UI.tabs = names.map((n, i) => {
    const tb = document.createElement('div'); bar.appendChild(tb); tb.className = 'tab';
    tb.dataset.name = n;
    Object.assign(tb.style, { padding: '2px 8px', cursor: 'pointer', color: C.FAINT });
    tb.addEventListener('click', () => { touched(); setTile(i, true); });
    return tb;
  });
  attachSwipe(tv);
  refreshUiValues();
  setTile(clamp(G.curTile, 0, NTILES - 1), false);
}
function dashTick() {
  if (state !== ST.MAIN || !UI.cd5) return;
  resetWatch();
  const at = (epoch) => `<span>${TRS('reset tra', 'resets in')}</span><span>${fmtClock(epoch).toLowerCase()}</span>`;
  setText(UI.cd5, fmtEta(G.usage.h5Reset)); setText(UI.at5, at(G.usage.h5Reset));
  setText(UI.cd7, fmtEta(G.usage.d7Reset)); setText(UI.at7, at(G.usage.d7Reset));
  paceUpdate();
  setHdrStatus();
}
function refreshUiValues() {
  if (state !== ST.MAIN || !UI.pct5) return;
  for (const [k, v] of [['5', G.usage.h5], ['7', G.usage.d7]]) {
    const col = gradColor(v);
    setText(UI['pct' + k], `${Math.round(v)}<small>%</small>`, col);
    setBlocks(UI['blk' + k], v, col);
  }
  const [w, wc] = statusWord(G.usage.statusOverall);
  setText(UI.prompt, `${GT} ${TRS('stato', 'status')} <span style="color:${wc}">${w}</span><span class="cur">▌</span>`);
  if (UI.mRows) {
    let nOk = 0, nLim = 0;
    UI.mRows.forEach((r, i) => {
      const m = G.models[i], [txt, col] = modelStat(i);
      if (txt === 'ok') nOk++; if (txt === '429') nLim++;
      r.dot.style.background = col;
      setText(r.nm, escapeHtml(shortId(m.id)), m.pr.code === 0 ? C.MUTED : C.TEXT);
      setText(r.sp, sparkText(m.lh));
      setText(r.lat, m.pr.code > 0 ? `${(m.pr.ms / 1000).toFixed(1)}s` : '--');
      setText(r.st, txt, col);
    });
    setText(UI.mSum, TRS(`${nOk} disponibili · ${nLim} limitati`, `${nOk} available · ${nLim} limited`));
    const any = G.status.up.some((u) => !u);
    setText(UI.mInc, !G.status.ok ? TRS('status.claude.com: nessun dato', 'status.claude.com: no data')
      : any ? TRS('incidente attivo su status.claude.com', 'active incident on status.claude.com')
        : TRS('status.claude.com: nessun incidente', 'status.claude.com: no incidents'), !G.status.ok ? C.MUTED : any ? C.WARN : C.OK);
  }
  trendRedraw(); heatRedraw(); weeksRedraw(); homeRedraw(); pcRedraw(); dashTick();
}

// ============================================================
// Momenti (soglie 25/50/70/100)
// ============================================================
const THR = [25, 50, 70, 100];
let MO = null;
// avviso di reset: segnala quando una finestra che aveva superato l'80% torna disponibile
function resetFire(w) {
  G.resetSeen[w] = G.winReset[w];
  if (P.rstal && G.winPeak[w] >= 80) { G.pendWin = w; G.pendThr = 0; G.pendPeak = Math.round(G.winPeak[w]); }
  G.winPeak[w] = 0;
}
function resetWatch() {
  const now = nowEpoch();
  for (let w = 0; w < 2; w++) if (G.winReset[w] && now >= G.winReset[w] && G.resetSeen[w] !== G.winReset[w]) resetFire(w);
}
function checkThresholds() {
  const c = [G.usage.h5, G.usage.d7], re = [G.usage.h5Reset, G.usage.d7Reset];
  for (let w = 0; w < 2; w++) {
    if (G.thrBase && G.thrPrev[w] - c[w] > 15 && G.resetSeen[w] !== G.winReset[w]) resetFire(w);
    if (re[w]) G.winReset[w] = re[w];
    if (c[w] > G.winPeak[w]) G.winPeak[w] = c[w];
    if (!G.thrBase || G.thrPrev[w] - c[w] > 15) {
      G.thrFired[w] = 0;
      THR.forEach((t, i) => { if (c[w] >= t) G.thrFired[w] |= 1 << i; });
    } else {
      let hit = -1;
      THR.forEach((t, i) => { if (c[w] >= t && !(G.thrFired[w] & (1 << i))) { G.thrFired[w] |= 1 << i; hit = i; } });
      if (hit >= 0) { G.pendWin = w; G.pendThr = THR[hit]; }
    }
    G.thrPrev[w] = c[w];
  }
  G.thrBase = true;
}
function momentClose() { if (MO) { MO.scrim.remove(); MO = null; } }
function showMoment(win, thr) {
  momentClose();
  const col = thr === 100 ? C.BAD : thr === 70 ? C.WARN : thr === 50 ? C.ACCENT : C.OK;
  const fromPct = thr === 0 ? (G.pendPeak || 90) : thr === 25 ? 0 : thr === 50 ? 25 : thr === 70 ? 50 : 70;
  const s = obj(scr, 0, 0, 480, 320, { background: C.BG, zIndex: 50, cursor: 'pointer' });
  MO = { scrim: s, win, thr, col, fromPct, t0: realMs(), boxY: 104, drop: [] };
  s.addEventListener('click', (e) => { e.stopPropagation(); momentClose(); });
  const kind = thr === 0 ? 'reset' : TRS('avviso', 'alert');
  const frame = tbox(s, 10, 14, 460, 296, TRS(`${kind} · finestra ${win ? '7 giorni' : '5 ore'}`, `${kind} · ${win ? '7-day' : '5-hour'} window`), col);
  frame._lg.style.color = col;
  MO.frame = frame;
  const bx = obj(s, 30, MO.boxY - 40, 176, 116); MO.box = bx;
  const fill = thr === 100 ? mix('#6A6A74', C.ACCENT, 190) : C.ACCENT;
  const img = clawdImg(bx, 'xl', fill); img.style.left = '0px'; img.style.top = '0px';
  const E = EYES.xl;
  if (thr === 50) {
    for (const ex of E.x) rrect(bx, ex - 1, E.y - 1, E.w + 2, Math.floor(E.h / 2) + 2, 0, C.ACCENT);
    MO.drop[0] = rrect(bx, 150, 6, 8, 12, 4, C.BLUE);
  } else if (thr === 70) {
    for (const ex of E.x) rrect(bx, ex - 3, E.y - 4, E.w + 6, E.h + 8, 0, C.BG);
    MO.drop[0] = rrect(bx, 150, 6, 8, 12, 4, C.BLUE); MO.drop[1] = rrect(bx, 18, 12, 8, 12, 4, C.BLUE);
  } else if (thr === 100) {
    xEyes(bx, E, 0, 4, C.BAD, 2, -1, 1);
  }
  label(s, win === 0 ? TRS('finestra 5 ore', '5-hour window') : TRS('finestra 7 giorni', '7-day window'), 13, C.MUTED, 234, 48);
  MO.pct = label(s, '', 54, col, 232, 68); MO.pct.classList.add('pctl');
  const MSG = [TRS('si parte: ritmo tranquillo', 'just starting: easy pace'), TRS("meta' finestra consumata", 'half the window used'),
    TRS('attenzione: uso elevato', 'heads up: heavy usage'), TRS('limite raggiunto: attendi il reset', 'limit reached: wait for the reset')];
  const msgTxt = thr === 0 ? TRS('di nuovo disponibile: si riparte', 'available again: back to work') : MSG[THR.indexOf(thr)];
  const msg = label(s, `${GT} ${msgTxt}`, 14, C.TEXT, 234, 140); msg.classList.add('wrap'); msg.style.width = '222px';
  MO.blk = blocksLabel(s, 234, 196, 19, 12);
  const e = fmtEta(win === 0 ? G.usage.h5Reset : G.usage.d7Reset);
  label(s, thr === 0 ? TRS(`picco della finestra: ${fromPct}%`, `window peak: ${fromPct}%`) : TRS(`reset tra ${e}`, `resets in ${e}`), 12, C.MUTED, 234, 220);
  label(s, TRS('[ tocca per chiudere ]', '[ tap to close ]'), 11, C.FAINT, 292, 284);
  momentTick();
}
function momentTick() {
  if (!MO) return;
  const t = realMs() - MO.t0;
  let y = MO.boxY, x = 30;
  if (t < 450) { const p = t / 450; y = MO.boxY - Math.floor((1 - p) * (1 - p) * 60); }
  else if (MO.thr <= 50) y = MO.boxY + Math.trunc(4 * Math.sin((t - 450) / 260));
  else if (MO.thr === 70) x = 30 + (Math.floor(t / 70) % 2 ? 2 : -2);
  else if (MO.thr === 100) y = MO.boxY + 6;
  MO.box.style.left = x + 'px'; MO.box.style.top = y + 'px';
  const p = t < 200 ? 0 : t > 1100 ? 1 : (t - 200) / 900;
  const v = MO.fromPct + (MO.thr - MO.fromPct) * p;
  setText(MO.pct, `${Math.round(v)}<small>%</small>`);
  setBlocks(MO.blk, v, MO.col);
  MO.drop.forEach((d, i) => { if (!d) return; const c = (t + i * 450) % 900; d.style.top = ((i ? 12 : 6) + Math.floor(c * 34 / 900)) + 'px'; d.style.opacity = (255 - c * 190 / 900) / 255; });
  if (MO.thr === 100) MO.frame.style.borderColor = Math.floor(t / 350) % 2 ? C.BAD : C.BORDER;
  if (t > 4600) momentClose();
}

// ============================================================
// Avviso a schermo intero con Clawd: Claude Code, timer e pomodoro (come sul dispositivo)
// ============================================================
const NT_CLAUDE = 1, NT_TIMER = 2;
let NT = null;
function noticeClose() { if (NT) { NT.scrim.remove(); NT = null; } }
function noticeShow(n, t0) {
  noticeClose();
  const s = obj(scr, 0, 0, 480, 320, { background: C.BG, zIndex: 49, cursor: 'pointer' });
  NT = { scrim: s, kind: n.kind, col: n.col, hop: n.hop, maxMs: n.maxMs, t0: t0 || realMs() };
  s.addEventListener('click', (ev) => { ev.stopPropagation(); noticeClose(); });
  const frame = tbox(s, 10, 14, 460, 296, escapeHtml(n.legend), n.col);
  frame._lg.style.color = n.col; NT.frame = frame;
  const bx = obj(s, 30, 64, 176, 116); NT.box = bx;
  const img = clawdImg(bx, 'xl', C.ACCENT); img.style.left = '0px'; img.style.top = '0px';
  if (n.ask) NT.ask = label(bx, '?', 22, C.TEXT, 154, 0);
  label(s, n.top, 13, C.MUTED, 234, 48);
  if (n.big >= 0) { const b = label(s, `${n.big}<small>${n.unit}</small>`, 54, n.col, 232, 68); b.classList.add('pctl'); }
  else label(s, n.word, 22, n.col, 234, 92);
  const m = label(s, `${GT} ${n.msg}`, 14, C.TEXT, 234, 140); m.classList.add('wrap'); m.style.width = '222px';
  if (n.foot) label(s, n.foot, 12, C.MUTED, 234, 220);
  label(s, TRS('[ tocca per chiudere ]', '[ tap to close ]'), 11, C.FAINT, 292, 284);
  noticeTick();
}
// animazione piena nei primi 20 s, poi un richiamo ogni 15 s
function noticeTick() {
  if (!NT) return;
  const t = realMs() - NT.t0;
  if (t * speed > NT.maxMs) { noticeClose(); return; }
  const ph = t < 20000 ? t : (t - 20000) % 15000, live = t < 20000 || ph < 1600;
  let y = 104;
  if (t < 450) { const p = t / 450; y = 104 - Math.floor((1 - p) * (1 - p) * 60); }
  else if (live && NT.hop) { const h = (t - 450) % 1600; if (h < 400) y = 104 - Math.trunc(16 * Math.sin(Math.PI * h / 400)); }
  else if (live) { y = 104 + Math.trunc(3 * Math.sin(t / 400)); if (NT.ask) NT.ask.style.top = Math.trunc(3 - 3 * Math.sin(t / 300)) + 'px'; }
  NT.box.style.top = y + 'px';
  if (!NT.hop) NT.frame.style.borderColor = live && Math.floor(t / 600) % 2 ? C.BORDER : NT.col;
}

// ---- Claude Code: fine lavoro / permesso (hook -> PC Monitor -> POST /claude) ----
const CC_MIN_S = [0, 0, 60, 300];
function ccEvent(ev, proj, dur) {
  if (ev === 'busy') { if (G.ntPend === NT_CLAUDE) G.ntPend = 0; if (NT && NT.kind === NT_CLAUDE) noticeClose(); return; }
  if (!P.ccal) return;
  if (ev === 'done' && dur >= 0 && dur < CC_MIN_S[P.ccal]) { slog(`[CLAUDE] fine lavoro dopo ${dur} s: sotto la soglia, nessun avviso`); return; }
  G.cc = { ev, proj, dur };
  if (G.ntPend !== NT_TIMER) { G.ntPend = NT_CLAUDE; G.ntT0 = 0; }
}
function ccShow(t0) {
  const e = G.cc, done = e.ev === 'done', ask = e.ev === 'ask';
  noticeShow({
    kind: NT_CLAUDE, col: done ? C.OK : C.ACCENT, maxMs: 30 * 60000, hop: done, ask: !done,
    legend: e.proj ? `claude code · ${e.proj}` : 'claude code',
    top: done ? TRS('claude ha finito', 'claude is done') : TRS('claude ti aspetta', 'claude needs you'),
    big: done && e.dur >= 0 ? (e.dur >= 60 ? Math.round(e.dur / 60) : e.dur) : -1, unit: e.dur >= 60 ? ' min' : ' s',
    word: done ? TRS('fatto', 'done') : ask ? TRS('una domanda', 'a question') : TRS('un permesso', 'a permission'),
    msg: done ? TRS('tocca a te: rivedi e continua', 'your turn: review and continue')
      : ask ? TRS('ha una domanda per te', 'has a question for you') : TRS('serve un tuo permesso per continuare', 'needs your permission to continue'),
    foot: (done ? TRS('alle ', 'at ') : TRS('dalle ', 'since ')) + fmtHm(nowEpoch()),
  }, t0);
}

// ---- Timer e pomodoro (tocca l'ora nella home) ----
// Pomodoro: 25 min di focus e 5 di pausa, pausa lunga di 15 dopo il quarto, poi si ferma.
const TM_OFF = 0, TM_TIMER = 1, TM_FOCUS = 2, TM_BREAK = 3, POMO = { FOCUS: 25 * 60, SHORT: 5 * 60, LONG: 15 * 60, CYCLE: 4 };
// tre impostazioni pronte: focus / pausa / pausa lunga dopo il quarto (minuti)
const POMO_PRESETS = [[25, 5, 15], [50, 10, 20], [15, 3, 10]];
function pomoPreset(i) { TM.pre = i; const [f, b, l] = POMO_PRESETS[i]; Object.assign(POMO, { FOCUS: f * 60, SHORT: b * 60, LONG: l * 60 }); }
const TM = { mode: TM_OFF, endMs: 0, lenS: 0, n: 0, today: 0, day: '', notice: '', pre: 0 };
const localDay = () => { const p = localParts(nowEpoch()); return `${p.mo}-${p.d}`; };
const pomoWord = (n) => (n === 1 ? 'pomodoro' : TRS('pomodori', 'pomodoros'));
const pomoToday = () => (TM.day === localDay() ? TM.today : 0);
function pomoCount() { if (TM.day !== localDay()) { TM.day = localDay(); TM.today = 0; } TM.today++; }
const tmLeft = () => Math.max(0, Math.ceil((TM.endMs - millis()) / 1000));
function tmStart(mode, secs) { TM.mode = mode; TM.lenS = secs; TM.endMs = millis() + secs * 1000; slog(`[TIMER] ${['', 'timer', 'focus', 'pausa'][mode]} ${secs} s`); }
function tmLabel() {
  const l = tmLeft(), c = l >= 3600 ? `${Math.floor(l / 3600)}:${pad2(Math.floor(l % 3600 / 60))}:${pad2(l % 60)}` : `${pad2(Math.floor(l / 60))}:${pad2(l % 60)}`;
  return TM.mode === TM_FOCUS ? `focus ${TM.n + 1}/${POMO.CYCLE} ${c}` : TM.mode === TM_BREAK ? TRS(`pausa ${c}`, `break ${c}`) : `timer ${c}`;
}
const tmColor = () => (TM.mode === TM_FOCUS ? C.ACCENT : TM.mode === TM_BREAK ? C.OK : C.TEXT);
function tmShow(t0) {
  const today = pomoToday(), foot = TRS(`oggi: ${today} ${pomoWord(today)}`, `today: ${today} ${pomoWord(today)}`);
  const n = { kind: NT_TIMER, hop: true, maxMs: 2 * 60000, unit: ' min', foot };
  if (TM.notice === 'timer') Object.assign(n, { col: C.WARN, maxMs: 10 * 60000, legend: 'timer', top: TRS('tempo scaduto', "time's up"),
    big: TM.lenS >= 60 ? Math.floor(TM.lenS / 60) : TM.lenS, unit: TM.lenS >= 60 ? ' min' : ' s',
    msg: TRS(`timer di ${Math.floor(TM.lenS / 60)} min finito`, `${Math.floor(TM.lenS / 60)} min timer finished`), foot: TRS('alle ', 'at ') + fmtHm(nowEpoch()) });
  else if (TM.notice === 'cycle') Object.assign(n, { col: C.OK, maxMs: 10 * 60000, legend: 'pomodoro', top: TRS('ciclo completato', 'cycle complete'),
    big: POMO.CYCLE, unit: TRS(' pomodori', ' pomodoros'), msg: TRS('ottimo lavoro: fai una pausa vera', 'great work: take a real break') });
  else if (TM.notice === 'break') Object.assign(n, { col: C.OK, legend: `pomodoro ${TM.n}/${POMO.CYCLE}`, top: TRS('pausa!', 'break time'),
    big: Math.floor(TM.lenS / 60), msg: TRS('pomodoro fatto: alzati e respira', 'pomodoro done: stand up and breathe') });
  else Object.assign(n, { col: C.ACCENT, hop: false, legend: `pomodoro ${TM.n + 1}/${POMO.CYCLE}`, top: TRS('si riparte', 'back to focus'),
    big: Math.floor(TM.lenS / 60), msg: TRS('di concentrazione: una cosa sola', 'of focus: one thing only') });
  noticeShow(n, t0);
}
function tmChanged() { if (hdrStatus) { hdrStatus._t = null; setHdrStatus(); } homeTick(); }
function tmTick() {
  if (!TM.mode || millis() < TM.endMs) return;
  if (TM.mode === TM_TIMER) { TM.notice = 'timer'; TM.mode = TM_OFF; }
  else if (TM.mode === TM_FOCUS) { TM.n++; pomoCount(); TM.notice = 'break'; tmStart(TM_BREAK, TM.n >= POMO.CYCLE ? POMO.LONG : POMO.SHORT); }
  else if (TM.n >= POMO.CYCLE) { TM.notice = 'cycle'; TM.mode = TM_OFF; TM.n = 0; }
  else { TM.notice = 'focus'; tmStart(TM_FOCUS, POMO.FOCUS); }
  G.ntPend = NT_TIMER; G.ntT0 = 0;
  tmChanged();
}
let TMENU = null;
function tmMenuClose() { if (TMENU) { TMENU.remove(); TMENU = null; } }
function tmMenuOpen() {
  tmMenuClose();
  const s = obj(scr, 0, 0, 480, 320, { background: 'rgba(20,20,19,.8)', zIndex: 60 });
  s.addEventListener('click', (e) => { e.stopPropagation(); tmMenuClose(); });
  TMENU = s;
  const today = pomoToday();
  const b = tbox(s, 90, 42, 300, 236, today ? TRS(`timer · oggi ${today} ${pomoWord(today)}`, `timer · today ${today} ${pomoWord(today)}`) : TRS('timer e pomodoro', 'timer and pomodoro'), C.ACCENT);
  b.style.background = C.BG; b._lg.style.color = C.ACCENT;
  b.addEventListener('click', (e) => e.stopPropagation());
  const btn = (txt, x, y, w, h, fn, color) => { const bt = tbtn(b, txt, w, h, () => { tmMenuClose(); if (fn) { fn(); tmChanged(); } }, { color, size: 14 }); bt.style.left = x + 'px'; bt.style.top = y + 'px'; };
  if (!TM.mode) {
    label(b, TRS('pomodoro · focus/pausa in minuti', 'pomodoro · focus/break in minutes'), 12, C.MUTED, 20, 12);
    POMO_PRESETS.forEach(([f, br], i) => btn(`${f}/${br}`, 20 + i * 90, 30, 80, 40, () => { pomoPreset(i); TM.n = 0; tmStart(TM_FOCUS, POMO.FOCUS); }, C.ACCENT));
    label(b, 'timer', 12, C.MUTED, 20, 82);
    [5, 10, 15, 30].forEach((m, i) => btn(`${m} min`, 20 + i * 67, 100, 58, 40, () => tmStart(TM_TIMER, m * 60), C.TEXT));
    btn(TRS('annulla', 'cancel'), 20, 158, 258, 38, null, C.MUTED);
  } else {
    label(b, tmLabel(), 22, tmColor(), 20, 30);
    if (TM.mode !== TM_TIMER) { const [f, br, l] = POMO_PRESETS[TM.pre]; label(b, TRS(`pomodoro ${f}/${br}, pausa lunga ${l}`, `pomodoro ${f}/${br}, long break ${l}`), 12, C.MUTED, 20, 64); }
    btn(TRS('ferma', 'stop'), 20, 100, 124, 40, () => { TM.mode = TM_OFF; TM.n = 0; slog('[TIMER] fermato'); }, C.BAD);
    if (TM.mode === TM_TIMER) btn('+5 min', 154, 100, 124, 40, () => { TM.endMs += 5 * 60000; TM.lenS += 300; }, C.TEXT);
    else btn(TRS('salta fase', 'skip phase'), 154, 100, 124, 40, () => { TM.endMs = millis(); }, C.TEXT);
    btn(TRS('annulla', 'cancel'), 20, 158, 258, 38, null, C.MUTED);
  }
}

// ============================================================
// Impostazioni / account / about
// ============================================================
const POLL_OPTS = [30, 60, 120, 300, 600, 900, 1800], TZ_OPTS = [TZ_ROME, 0, 1, 2, 3, 4, 5, -1, -2, -3, -4, -5, -6, -7, -8], SL = [0, 5, 10, 15, 30];
const accountCount = () => G.accts.used.filter(Boolean).length;
const accountFirstFree = () => G.accts.used.indexOf(false);
function uniqueLabel(lbl, slot) {
  lbl = lbl.replace(/[^\x20-\x7E]|["\\<>]/g, '').trimEnd().slice(0, 16) || `Account ${slot + 1}`;
  const base = lbl;
  for (let n = 2; n <= CFG.ACCT_MAX + 1; n++) {
    if (!G.accts.label.some((l, i) => i !== slot && G.accts.used[i] && l.toLowerCase() === lbl.toLowerCase())) return lbl;
    lbl = `${base.slice(0, 14)} ${n}`;
  }
  return lbl;
}
function uiSettings() {
  G.wipeArmed = false;
  thead(TRS('impostazioni', 'settings'), G.usage.ok ? ST.MAIN : ST.SETTINGS);
  const lst = obj(scr, 13, 47, 454, 273, { display: 'flex', flexDirection: 'column' }); lst.classList.add('scroll');
  G.setList = lst; requestAnimationFrame(() => { lst.scrollTop = G.setScroll || 0; });
  const briN = () => [TRS('bassa', 'low'), TRS('media', 'medium'), TRS('alta', 'high')][P.bri];
  const pollVal = () => (P.poll < 60 ? `${P.poll}s` : `${P.poll / 60}min`);
  const slideVal = () => (P.slide ? `${P.slide}s` : TRS('spento', 'off'));
  const sg = (n) => (n >= 0 ? '+' : '') + n;
  const tzVal = () => (P.tz === TZ_ROME ? TRS('roma (auto)', 'rome (auto)') : `gmt${sg(P.tz)}`);
  kvRow(lst, TRS('aggiorna ora', 'refresh now'), '↵', () => requestState(ST.LOADING));
  kvRow(lst, TRS('intervallo', 'interval'), pollVal(), (v) => { P.poll = POLL_OPTS[(POLL_OPTS.indexOf(P.poll) + 1) % POLL_OPTS.length]; setText(v, pollVal()); });
  kvRow(lst, 'slideshow', slideVal(), (v) => { P.slide = SL[(SL.indexOf(P.slide) + 1) % SL.length]; setText(v, slideVal()); });
  kvRow(lst, TRS('lingua', 'language'), TRS('italiano', 'english'), () => { P.lang ^= 1; requestState(ST.SETTINGS); });
  kvRow(lst, TRS('fuso orario', 'timezone'), tzVal(), (v) => { const i = TZ_OPTS.indexOf(P.tz); P.tz = TZ_OPTS[(i + 1) % TZ_OPTS.length]; setText(v, tzVal()); });
  kvRow(lst, TRS('avviso reset', 'reset alert'), P.rstal ? TRS('sopra 80%', 'above 80%') : TRS('spento', 'off'), () => { P.rstal = !P.rstal; requestState(ST.SETTINGS); });
  const CCL = [TRS('spento', 'off'), TRS('sempre', 'always'), TRS('oltre 1 min', 'over 1 min'), TRS('oltre 5 min', 'over 5 min')];
  kvRow(lst, TRS('avvisi claude code', 'claude code alerts'), CCL[P.ccal], () => { P.ccal = (P.ccal + 1) % 4; requestState(ST.SETTINGS); });
  kvRow(lst, TRS("luminosita'", 'brightness'), briN(), (v) => { P.bri = (P.bri + 1) % 3; applyBrightness(); setText(v, briN()); });
  const NL = ['', '22:00-07:00', '23:00-07:00', '00:00-07:00'];
  kvRow(lst, TRS('notte', 'night'), P.night ? NL[P.night] : TRS('spento', 'off'), () => { P.night = (P.night + 1) % 4; requestState(ST.SETTINGS); });
  if (P.night) kvRow(lst, TRS('schermo di notte', 'screen at night'), P.nightclk ? TRS('orologio', 'clock') : TRS('spento', 'off'), () => { P.nightclk = !P.nightclk; requestState(ST.SETTINGS); });
  if (P.night && P.nightclk) kvRow(lst, TRS("luminosità notte", 'night brightness'), P.nightbri ? TRS('molto tenue', 'very dim') : TRS('tenue', 'dim'), () => { P.nightbri ^= 1; requestState(ST.SETTINGS); });
  if (P.night) kvRow(lst, TRS('aggiornamenti di notte', 'updates at night'), P.nightp ? TRS('in pausa', 'paused') : TRS('attivi', 'active'), () => { P.nightp = !P.nightp; requestState(ST.SETTINGS); });
  const PCI = [1, 3, 5, 60];
  kvRow(lst, TRS('intervallo pc', 'pc interval'), PCI[P.pcint || 0] >= 60 ? '1min' : `${PCI[P.pcint || 0]}s`, () => { P.pcint = ((P.pcint || 0) + 1) % 4; requestState(ST.SETTINGS); });
  kvRow(lst, TRS('home dopo', 'home after'), P.clock ? `${CLOCK_MIN[P.clock]} min` : TRS('spento', 'off'), () => { P.clock = (P.clock + 1) % 4; requestState(ST.SETTINGS); });
  kvRow(lst, TRS('attenua dopo', 'dim after'), P.dim ? `${DIM_MIN[P.dim]} min` : TRS('spento', 'off'), () => { P.dim = (P.dim + 1) % 4; requestState(ST.SETTINGS); });
  kvRow(lst, 'wifi', escapeHtml(G.wifiConnected ? G.ssid : '--'), () => { G.onboarding = false; requestState(ST.WIFI); });
  kvRow(lst, TRS('rete locale', 'local network'), G.wifiConnected ? `http://${DEVICE_IP}` : TRS('non connesso', 'not connected'), null);
  kvRow(lst, TRS('nome in rete', 'network name'), 'ritmo-code.local', null, { color: C.MUTED, valColor: C.MUTED });
  kvRow(lst, 'account', `${escapeHtml(G.accts.label[G.accts.active])} (${accountCount()}/4)`, () => { G.acctDelArmed = -1; requestState(ST.ACCOUNTS); });
  kvRow(lst, TRS('modelli', 'models'), TRS('modifica id', 'edit ids'), () => requestState(ST.MODELS));
  kvRow(lst, 'token', TRS('cambia', 'change'), () => { G.tokenTargetSlot = G.accts.active; G.pendingLabel = ''; requestState(ST.TOKEN); });
  kvRow(lst, TRS('contatore fps', 'fps counter'), P.perf ? TRS('acceso', 'on') : TRS('spento', 'off'), (v) => { P.perf = !P.perf; setText(v, P.perf ? TRS('acceso', 'on') : TRS('spento', 'off')); });
  kvRow(lst, 'info', `v${CFG.FW}`, () => requestState(ST.ABOUT));
  kvRow(lst, TRS('cancella tutto', 'erase everything'), '', (v, k) => {
    if (!G.wipeArmed) { G.wipeArmed = true; setText(v, TRS('tocca ancora', 'tap again'), C.BAD); }
    else { G.wipeArmed = false; factoryReset(); requestState(ST.WIFI); }
  }, { color: C.BAD, valColor: C.BAD });
}
function uiAccounts() {
  thead(TRS('account', 'accounts'), ST.SETTINGS);
  const lst = obj(scr, 13, 50, 454, 236, { display: 'flex', flexDirection: 'column', gap: '6px' }); lst.classList.add('scroll');
  const canDelete = accountCount() > 1;
  for (let i = 0; i < CFG.ACCT_MAX; i++) {
    if (!G.accts.used[i]) continue;
    const active = i === G.accts.active;
    const r = document.createElement('div'); lst.appendChild(r);
    Object.assign(r.style, { position: 'relative', flex: '0 0 40px', width: '456px', height: '40px' });
    const b = obj(r, 0, 0, canDelete ? 348 : 402, 40, { border: `1px solid ${active ? C.ACCENT : C.BORDER}`, borderRadius: '4px', cursor: 'pointer' }); b.classList.add('btn');
    align(label(b, `${active ? GT + ' ' : '  '}${escapeHtml(G.accts.label[i])}${active ? `<span style="color:${C.FAINT}">  ${TRS('attivo', 'active')}</span>` : ''}`, 14, active ? C.ACCENT : C.TEXT), 'LEFT_MID', 10, 0);
    b.addEventListener('click', () => { G.acctDelArmed = -1; if (!active) switchAccount(i); });
    const ed = tbtn(r, '✎', 48, 40, () => { G.renameSlot = i; G.acctDelArmed = -1; requestState(ST.ACCT_NAME); }, { color: C.MUTED, size: 16 });
    ed.style.left = (canDelete ? 354 : 408) + 'px'; ed.style.top = '0px';
    if (canDelete) {
      const armed = G.acctDelArmed === i;
      const d = tbtn(r, armed ? '!' : '✕', 48, 40, () => {
        if (G.acctDelArmed !== i) { G.acctDelArmed = i; requestState(ST.ACCOUNTS); return; }
        G.acctDelArmed = -1;
        const wasActive = i === G.accts.active;
        G.accts.used[i] = false; G.accts.label[i] = '';
        if (wasActive) { G.accts.active = G.accts.used.indexOf(true); G.usage = { ok: false }; seedHistory(); requestState(ST.LOADING); return; }
        requestState(ST.ACCOUNTS);
      }, { color: armed ? C.BAD : C.MUTED, border: armed ? C.BAD : C.BORDER, size: 15 });
      d.style.left = '408px'; d.style.top = '0px';
    }
  }
  if (accountFirstFree() >= 0) {
    const add = document.createElement('div'); lst.appendChild(add); add.className = 'btn';
    Object.assign(add.style, { position: 'relative', flex: '0 0 40px', width: '456px', height: '40px', border: `1px dashed ${C.BORDER}`, borderRadius: '4px', cursor: 'pointer' });
    align(label(add, `+ ${TRS('aggiungi account', 'add account')}`, 14, C.ACCENT), 'LEFT_MID', 10, 0);
    add.addEventListener('click', () => { G.tokenTargetSlot = accountFirstFree(); G.pendingLabel = ''; requestState(ST.TOKEN); });
  }
  const hint = label(scr, TRS("solo l'account attivo interroga la API", 'only the active account is polled'), 11, C.FAINT);
  align(hint, 'BOTTOM_LEFT', 12, -10);
}
function uiAccountName() {
  const slot = G.renameSlot;
  if (slot < 0 || !G.accts.used[slot]) { requestState(ST.ACCOUNTS); return; }
  thead(TRS('rinomina account', 'rename account'), ST.ACCOUNTS);
  const ta = textarea(scr, 456, 40, { placeholder: TRS('etichetta (es.: Personale, Lavoro)', 'label (e.g. Personal, Work)'), maxLen: 16, text: G.accts.label[slot] });
  align(ta, 'TOP_MID', 0, 54);
  ta._kb = keyboard(scr, ta, (v) => { G.accts.label[slot] = uniqueLabel(v, slot); requestState(ST.ACCOUNTS); }, () => requestState(ST.ACCOUNTS));
}
function uiModels() {
  thead(TRS('modelli', 'models'), ST.SETTINGS);
  const lst = obj(scr, 13, 47, 454, 200, { display: 'flex', flexDirection: 'column' });
  G.models.forEach((m, i) => {
    kvRow(lst, m.name.toLowerCase(), escapeHtml(m.id), () => { G.modelEdit = i; requestState(ST.MODEL_EDIT); },
      { valColor: m.id !== m.defId ? C.ACCENT : C.MUTED });
  });
  const hint = label(scr, `${GT} ${TRS(`piu' comodo dal browser: http://${DEVICE_IP}/models`, `easier from a browser: http://${DEVICE_IP}/models`)}`, 11, C.MUTED);
  align(hint, 'BOTTOM_LEFT', 12, -12); hint.style.cursor = 'pointer'; hint.title = 'Apri la pagina web del device (simulata)';
  hint.addEventListener('click', () => openModelsPage());
}
function uiModelEdit() {
  const i = G.modelEdit, m = G.models[i];
  if (!m) { requestState(ST.MODELS); return; }
  thead(TRS(`modello: ${m.name.toLowerCase()}`, `model: ${m.name.toLowerCase()}`), ST.MODELS);
  const ta = textarea(scr, 456, 40, { placeholder: m.defId, maxLen: 47, text: m.id });
  align(ta, 'TOP_MID', 0, 52);
  const err = label(scr, TRS(`vuoto = predefinito (${m.defId})`, `empty = default (${m.defId})`), 11, C.FAINT, 12, 100);
  ta._kb = keyboard(scr, ta, (v) => {
    if (modelSetId(i, v)) requestState(ST.MODELS);
    else setText(err, TRS('id non valido: solo a-z 0-9 . - _ (3-47 caratteri)', 'invalid id: only a-z 0-9 . - _ (3-47 chars)'), C.BAD);
  }, () => requestState(ST.MODELS));
}
// pagina web /models del device (simulata nel riquadro "browser")
function openModelsPage(msg = '', ok = true) {
  $('webUrl').textContent = `http://${DEVICE_IP}/models`;
  $('webPage').innerHTML = `<div class=card><h1>${WEB_SPARK} Modelli sondati</h1>
    <p>Il dispositivo controlla un modello per ciclo con l'ID indicato qui. Se Anthropic cambia i nomi, aggiornali copiandoli dalla <a href='https://docs.anthropic.com/en/docs/about-claude/models' target=_blank>documentazione</a>. Lascia vuoto per tornare al predefinito.</p>
    ${msg ? `<div style="padding:10px 12px;border-radius:10px;margin:0 0 12px;font-size:14px;background:${ok ? '#4ADE8022' : '#F8717122'};color:${ok ? '#4ADE80' : '#F87171'}">${msg}</div>` : ''}
    <form id=mf>${G.models.map((m, i) => `<label style="display:block;font-size:13px;color:#8C8C98;margin:12px 0 4px" for=m${i}>${m.name}</label>
      <input id=m${i} name=m${i} maxlength=47 autocomplete=off spellcheck=false style="margin-bottom:0;font-family:ui-monospace,monospace" value='${escapeHtml(m.id)}' placeholder='${m.defId}'>`).join('')}
    <button type=submit>Salva</button></form></div>`;
  $('webModal').hidden = false;
  $('mf').addEventListener('submit', (e) => {
    e.preventDefault();
    const fd = new FormData(e.target), bad = [];
    G.models.forEach((m, i) => { if (!modelSetId(i, fd.get('m' + i) || '')) bad.push(m.name); });
    if (state === ST.MAIN || state === ST.MODELS) requestState(state);
    if (bad.length) openModelsPage(`ID non valido per ${bad.join(', ')}: usa solo a-z, 0-9, punto, trattino e underscore (3-47 caratteri).`, false);
    else openModelsPage('Salvato. Il controllo usa i nuovi ID dal prossimo giro, senza richieste extra.', true);
  });
}
function uiAbout() {
  thead('info', ST.SETTINGS);
  align(buildClaudeMark(scr), 'TOP_MID', 0, 52);
  align(label(scr, 'ritmo code', 18, C.TEXT), 'TOP_MID', 0, 152);
  align(label(scr, `v${CFG.FW} · esp32-s3 · lvgl 9.2`, 11, C.FAINT), 'TOP_MID', 0, 178);
  const d = label(scr, TRS("uso di claude code in tempo reale: finestre 5h e settimanale lette dalla API anthropic", 'real-time claude code usage: 5h and weekly windows read from the anthropic api'), 12, C.MUTED);
  d.classList.add('wrap'); Object.assign(d.style, { width: '400px', textAlign: 'center' }); align(d, 'TOP_MID', 0, 198);
  align(label(scr, 'guition jc4832w535 · ips 3.5" 480x320 · axs15231b', 11, C.FAINT), 'TOP_MID', 0, 236);
  align(label(scr, `${TRS('sviluppato da', 'developed by')} Luca Marullo · Innova Design Studio`, 12, C.TEXT), 'TOP_MID', 0, 256);
  align(label(scr, 'innovadesignstudio.it · info@innovadesignstudio.it', 11, C.ACCENT), 'TOP_MID', 0, 274);
  align(label(scr, TRS('basato su claude-usage-stick di Benevid Felix', 'based on claude-usage-stick by Benevid Felix'), 11, C.FAINT), 'TOP_MID', 0, 296);
}
function switchAccount(slot) {
  if (!G.accts.used[slot] || slot === G.accts.active) return;
  G.accts.active = slot; G.usage = { ok: false };
  seedHistory();
  slog(`[ACCT] account attivo -> slot ${slot} (${G.accts.label[slot]})`);
  requestState(ST.LOADING);
}
function finalizePendingToken() {
  const slot = G.tokenTargetSlot, switching = slot !== G.accts.active;
  const lbl = G.pendingLabel || (G.accts.used[slot] ? G.accts.label[slot] : '');
  G.accts.used[slot] = true; G.accts.label[slot] = uniqueLabel(lbl || `Account ${slot + 1}`, slot);
  if (switching) { G.accts.active = slot; seedHistory(); }
  G.pendingToken = ''; G.pendingLabel = ''; G.hasToken = true;
  G.lastOkMs = G.lastPollMs = millis(); G.lastFetchOk = true;
  histPush(G.usage.h5, G.usage.d7); accumulateHeat(G.usage.h5);
  slog(`[ACCT] token salvato nello slot ${slot} (${G.accts.label[slot]})`);
  requestState(ST.MAIN);
}
function factoryReset() {
  Object.assign(P, { pinatt: 0 });
  G.accts = { used: [false, false, false, false], label: ['', '', '', ''], active: 0 };
  G.sessionPin = ''; G.userPin = ''; G.pendingLabel = ''; G.tokenTargetSlot = 0; G.hasToken = false; G.onboarding = true;
  G.wifiConnected = false; G.usage = { ok: false };
  G.hist = []; G.days = []; G.hourBurn = new Array(24).fill(0); G.lastH5 = -1;
  G.models.forEach((m) => { m.pr = { code: 0, ms: 0 }; m.id = m.defId; }); G.probeIdx = 0; G.thrBase = false;
  slog('[RESET] tutto cancellato');
}
function applyBrightness() {
  const b = G.screenMode === 2 ? 0 : G.screenMode === 3 ? [0.35, 0.15][P.nightbri] : G.screenMode === 1 ? 0.08 : [0.45, 0.75, 1][P.bri];
  scr.style.filter = `brightness(${b})`;
}
// ---- notte / attenuazione (come screen_tick del firmware; i tempi seguono la velocita' della simulazione)
const NIGHT_FROM = [0, 22, 23, 0], DIM_MIN = [0, 1, 5, 10];
function nightActive() {
  if (!P.night) return false;
  const h = localParts(nowEpoch()).h, from = NIGHT_FROM[P.night];
  return from === 0 ? h < 7 : (h >= from || h < 7);
}
const nightPaused = () => P.nightp && nightActive();
const pollingPaused = () => P.pause || nightPaused();      // pausa manuale (tasto in testata) o notte
// orologio notturno a tutto schermo (variante B), come night_clock_* del firmware
let NC = null;
function nightClockClose() { if (NC) { NC.scrim.remove(); NC = null; } }
function nightClockUpdate() {
  if (!NC) return;
  const p = localParts(nowEpoch());
  setText(NC.time, `${pad2(p.h)}:${pad2(p.m)}`);
  const GI = ['domenica', 'lunedì', 'martedì', 'mercoledì', 'giovedì', 'venerdì', 'sabato'], GE = ['sunday', 'monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday'];
  const MI = ['gennaio', 'febbraio', 'marzo', 'aprile', 'maggio', 'giugno', 'luglio', 'agosto', 'settembre', 'ottobre', 'novembre', 'dicembre'];
  const ME = ['january', 'february', 'march', 'april', 'may', 'june', 'july', 'august', 'september', 'october', 'november', 'december'];
  setText(NC.date, P.lang ? `${GE[p.wd]}, ${ME[p.mo - 1]} ${p.d}` : `${GI[p.wd]} ${p.d} ${MI[p.mo - 1]}`);
  const parts = [];
  if (G.usage.ok) parts.push(TRS(`5h ${Math.round(G.usage.h5)}% · sett. ${Math.round(G.usage.d7)}%`, `5h ${Math.round(G.usage.h5)}% · week ${Math.round(G.usage.d7)}%`));
  const rain = WX.rain.findIndex((v) => v >= 40);
  parts.push(`${WX.temp}°` + (rain >= 0 ? TRS(` pioggia alle ${pad2((WX.rainH0 + rain) % 24)}`, ` rain at ${pad2((WX.rainH0 + rain) % 24)}`) : ''));
  setText(NC.info, parts.join(' · '));
}
function nightClockShow() {
  nightClockClose();
  const s = obj(scr, 0, 0, 480, 320, { background: '#000', zIndex: 70 });
  const time = label(s, '', 150, '#A8A49A'); Object.assign(time.style, { width: '480px', textAlign: 'center', top: '37px', fontWeight: '500', letterSpacing: '-6px', lineHeight: '1' });
  const date = label(s, '', 14, '#5C5A55'); Object.assign(date.style, { width: '480px', textAlign: 'center', top: '197px' });
  const info = label(s, '', 12, '#5C5A55'); Object.assign(info.style, { width: '480px', textAlign: 'center', top: '283px' });
  NC = { scrim: s, time, date, info };
  nightClockUpdate();
}
function screenTick() {
  const idle = (realMs() - G.lastTouch) * speed;
  let want = 0;
  if (state === ST.MAIN) {
    if (nightActive() && idle > 30000) want = P.nightclk ? 3 : 2;
    else if (P.dim && idle > DIM_MIN[P.dim] * 60000) want = 1;
  }
  if (want !== G.screenMode) { G.screenMode = want; applyBrightness(); }
  if (G.screenMode === 3 && !NC) nightClockShow();
  else if (G.screenMode !== 3 && NC) nightClockClose();
  if (NC && realMs() - (NC.at || 0) > 1000) { NC.at = realMs(); nightClockUpdate(); }
  if (NC && NC.bri !== P.nightbri) { NC.bri = P.nightbri; applyBrightness(); }
}
// il tocco che risveglia lo schermo non preme nulla
for (const ev of ['pointerdown', 'click']) {
  scr.addEventListener(ev, (e) => {
    if (ev === 'pointerdown' && G.screenMode) { G.swallow = true; G.screenMode = 0; touched(); applyBrightness(); }
    if (G.swallow) { e.stopPropagation(); e.preventDefault(); if (ev === 'click') G.swallow = false; }
  }, true);
}

// ============================================================
// Render / ciclo dati
// ============================================================
function clearScreen() {
  momentClose(); PMENU = null; NC = null;
  // l'avviso a schermo intero sopravvive ai rebuild del dashboard (stesso inizio)
  if (NT) { if (pending === ST.MAIN) { G.ntPend = NT.kind; G.ntT0 = NT.t0; } noticeClose(); }
  tmMenuClose();
  scr.innerHTML = '';
  UI = {}; hdrStatus = null; pinDots = pinMsg = tokMsg = null; activeTA = null;
}
function renderState() {
  // la pagina impostazioni ridisegnata da se' stessa mantiene lo scorrimento
  const keepScroll = state === ST.SETTINGS && pending === ST.SETTINGS && G.setList ? G.setList.scrollTop : 0;
  G.setList = null;
  state = pending;
  clearScreen();
  if (state !== ST.TOKEN && !$('webUrl').textContent.endsWith('/models')) $('webModal').hidden = true;
  G.setScroll = keepScroll;
  switch (state) {
    case ST.PIN: case ST.SETUP_PIN: uiPin(); break;
    case ST.WIFI: uiWifi(); break;
    case ST.TOKEN: uiToken(); break;
    case ST.LOADING: uiLoading(G.wifiConnected ? G.ssid : TRS('connessione WiFi', 'connecting WiFi')); break;
    case ST.MAIN: uiMain(); break;
    case ST.SETTINGS: uiSettings(); break;
    case ST.ACCOUNTS: uiAccounts(); break;
    case ST.ACCT_NAME: uiAccountName(); break;
    case ST.ABOUT: uiAbout(); break;
    case ST.MODELS: uiModels(); break;
    case ST.MODEL_EDIT: uiModelEdit(); break;
    case ST.ERROR: uiMessage(TRS('Errore', 'Failed'), G.usage.error || TRS('nessun dato', 'no data'), C.BAD); break;
  }
}
function doRefresh() {        // primo load (schermata di caricamento)
  setTimeout(() => {
    if (state !== ST.LOADING) return;
    const u = apiFetch();
    if (u.ok) {
      G.usage = u; fetchModelStatus(); G.lastOkMs = millis(); G.lastFetchOk = true;
      histPush(u.h5, u.d7); accumulateHeat(u.h5); checkThresholds(); probeNextModel();
    } else { G.usage = u; G.lastFetchOk = false; }
    G.lastPollMs = millis();
    requestState(u.ok ? ST.MAIN : ST.ERROR);
  }, 1200);
}
function bgRefresh() {        // refresh in background (header "aggiornamento...")
  G.refreshing = true; setHdrStatus();
  setTimeout(() => {
    const u = apiFetch();
    let rebuild = false;
    if (u.ok) {
      G.usage = u; G.lastOkMs = millis(); G.lastFetchOk = true; weekRecord(u.d7, u.d7Reset);
      histPush(u.h5, u.d7); accumulateHeat(u.h5); checkThresholds();
      const before = [0, 1, 2, 3].map(modelMood);
      fetchModelStatus(); probeNextModel();
      rebuild = [0, 1, 2, 3].some((i) => before[i] !== modelMood(i));
    } else G.lastFetchOk = false;
    G.refreshing = false; G.lastPollMs = millis();
    if (state !== ST.MAIN) return;
    if (rebuild) requestState(ST.MAIN); else refreshUiValues();
  }, 800);
}
function touched() { G.lastTouch = realMs(); }
scr.addEventListener('pointerdown', touched);

// ---------- loop principale ----------
let lastReal = realMs(), lastTick = 0, lastBob = 0, blinkAt = 0, blinkClosed = false, lastBurn = 0;
function loop() {
  const r = realMs(), dt = r - lastReal; lastReal = r;
  simMs += dt * speed;

  // consumo automatico: fa salire l'uso nel tempo simulato
  if (API.burn > 0 && simMs - lastBurn > 60000) {
    const mins = (simMs - lastBurn) / 60000; lastBurn = simMs;
    API.h5 = Math.min(100, API.h5 + API.burn * mins); API.d7 = Math.min(100, API.d7 + API.burn * mins / 12);
    syncPanel();
  } else if (API.burn === 0) lastBurn = simMs;

  if (dirty) {
    dirty = false; renderState();
    if (state === ST.LOADING) doRefresh();
  }
  screenTick();
  tmTick();
  if (state === ST.MAIN && !G.refreshing && (G.wantRefresh || (!pollingPaused() && millis() - G.lastPollMs > P.poll * 1000))) {
    G.wantRefresh = false; bgRefresh();
  }
  if (state === ST.MAIN) {
    if (r - lastTick > 1000 / Math.min(speed, 4)) { lastTick = r; dashTick(); homeTick(); if (TM.mode) setHdrStatus(); }
    if (UI.refBar) {
      let v = 1;
      if (!G.refreshing && pollingPaused()) v = 0;
      else if (!G.refreshing) { const el = millis() - G.lastPollMs, per = P.poll * 1000; v = el >= per ? 0 : 1 - el / per; }
      UI.refBar.style.width = (275 * v) + 'px';
    }
    if (r - lastBob > 80) {
      lastBob = r; const ph = r / 600;
      masc.forEach((m, i) => {
        if (m.mood === 1) m.cont.style.top = (m.baseY + Math.trunc(2 * Math.sin(ph + i * 0.9) - 1)) + 'px';
        else if (m.mood === 2) {
          m.cont.style.top = (m.baseY + Math.trunc(1.2 * Math.sin(ph * 0.6 + i))) + 'px';
          if (m.drop) { const cyc = (r + i * 300) % 900; m.drop.style.top = (24 + Math.floor(cyc * 22 / 900)) + 'px'; m.drop.style.opacity = (255 - cyc * 190 / 900) / 255; }
        }
      });
    }
    if (r - blinkAt > (blinkClosed ? 150 : 3000)) {
      blinkAt = r; blinkClosed = !blinkClosed;
      masc.forEach((m) => { if (m.mood === 1) m.lid.forEach((l) => { l.style.display = blinkClosed ? '' : 'none'; }); });
    }
    if (P.slide > 0 && UI.tv && !G.refreshing && !MO && !NT && !TMENU && G.screenMode < 2 && r - G.lastTouch > 10000 && r - G.lastSlide > P.slide * 1000) {
      G.lastSlide = r; setTile((G.curTile + 1) % NTILES, true);
    }
    if (P.pause && P.pauseUntil && nowEpoch() >= P.pauseUntil) pauseSet(false, 0);
    // torna alla home dopo N minuti senza tocchi (una volta per periodo di inattivita')
    if (P.clock && UI.tv && G.curTile !== 0 && !MO && !NT && !TMENU && !PMENU && G.screenMode < 2 && G.homedFor !== G.lastTouch &&
        (r - G.lastTouch) * speed > CLOCK_MIN[P.clock] * 60000) { G.homedFor = G.lastTouch; setTile(0, true); }
    if (G.pendWin >= 0 && !MO && !G.refreshing && G.screenMode < 2) { showMoment(G.pendWin, G.pendThr); G.pendWin = -1; }
    if (MO) momentTick();
    // di notte gli avvisi di Claude Code non compaiono; quelli del timer si' (e riaccendono lo schermo)
    if (G.ntPend === NT_CLAUDE && G.screenMode >= 2 && !G.ntT0) G.ntPend = 0;
    if (G.ntPend && !MO && !G.refreshing) {
      const k = G.ntPend, t0 = G.ntT0; G.ntPend = 0; G.ntT0 = 0;
      if (!t0) { touched(); if (G.screenMode) { G.screenMode = 0; applyBrightness(); } }
      if (k === NT_CLAUDE) ccShow(t0); else tmShow(t0);
    }
    if (NT) noticeTick();
  }
  if ((state === ST.PIN || state === ST.SETUP_PIN) && pinMsg && G.lockoutUntil > 0) {
    if (millis() < G.lockoutUntil) setText(pinMsg, TRS(`Attendi ${Math.ceil((G.lockoutUntil - millis()) / 1000)}s`, `Wait ${Math.ceil((G.lockoutUntil - millis()) / 1000)}s`));
    else { G.lockoutUntil = 0; setText(pinMsg, TRS('Serve per sbloccare il token.', 'Needed to unlock the token.')); }
  }
  requestAnimationFrame(loop);
}

// ---------- boot ----------
let bootTimer = [];
function boot(withToken) {
  bootTimer.forEach(clearTimeout); bootTimer = [];
  $('webModal').hidden = true;
  state = ST.BOOT; dirty = false;
  apiResetBase = null;
  G.models.forEach((m) => { m.pr = { code: 0, ms: 0 }; }); G.probeIdx = 0;
  G.thrBase = false; G.pendWin = -1; G.usage = { ok: false }; G.lockoutUntil = 0; G.pinEntry = '';
  if (withToken) {
    G.accts = { used: [true, false, false, false], label: ['Account 1', '', '', ''], active: 0 };
    G.hasToken = true; G.onboarding = false; G.wifiConnected = true; G.ssid = 'Innova-Studio'; G.userPin = '';
    seedHistory(); seedWeeks();
  } else {
    factoryReset();
  }
  bootSplash(TRS('Avvio...', 'Starting...'));
  slog('=== Ritmo Code (touch) ===');
  bootTimer.push(setTimeout(() => setText(bootSub, TRS('Connessione al WiFi...', 'Connecting to WiFi...')), 900));
  bootTimer.push(setTimeout(() => setText(bootSub, withToken ? G.ssid : TRS('Connessione al WiFi...', 'Connecting to WiFi...')), 1500));
  bootTimer.push(setTimeout(() => requestState(withToken ? ST.PIN : ST.WIFI), 2300));
}

// ============================================================
// Pannello di controllo
// ============================================================
function syncPanel() {
  $('h5').value = Math.round(API.h5); $('h5v').textContent = Math.round(API.h5) + '%';
  $('d7').value = Math.round(API.d7); $('d7v').textContent = Math.round(API.d7) + '%';
  const r5 = apiResetBase ? Math.max(1, Math.round((apiResetBase.h5 - nowEpoch()) / 60)) : API.r5min;
  const r7 = apiResetBase ? Math.max(1, Math.round((apiResetBase.d7 - nowEpoch()) / 3600)) : API.r7h;
  $('r5').value = r5; $('r5v').textContent = `${Math.floor(r5 / 60)}h ${pad2(r5 % 60)}m`;
  $('r7').value = r7; $('r7v').textContent = `${Math.floor(r7 / 24)}g ${r7 % 24}h`;
}
function initPanel() {
  syncPanel();
  $('h5').addEventListener('input', (e) => { API.h5 = +e.target.value; syncPanel(); });
  $('d7').addEventListener('input', (e) => { API.d7 = +e.target.value; syncPanel(); });
  $('r5').addEventListener('input', (e) => { API.r5min = +e.target.value; if (apiResetBase) apiResetBase.h5 = nowEpoch() + API.r5min * 60; syncPanel(); });
  $('r7').addEventListener('input', (e) => { API.r7h = +e.target.value; if (apiResetBase) apiResetBase.d7 = nowEpoch() + API.r7h * 3600; syncPanel(); });
  $('burn').addEventListener('change', (e) => { API.burn = +e.target.value; });
  $('ust').addEventListener('change', (e) => { API.ust = e.target.value; });
  $('apiFail').addEventListener('change', (e) => { API.fail = e.target.checked; });
  $('statusOk').addEventListener('change', (e) => { API.statusOk = e.target.checked; if (!API.statusOk) G.status.ok = false; });
  $('speed').addEventListener('change', (e) => { speed = +e.target.value; });
  const setZoom = (z) => document.documentElement.style.setProperty('--zoom', z);
  $('zoom').addEventListener('change', (e) => setZoom(e.target.value));
  const fit = () => { const w = document.querySelector('.stage').clientWidth - 80; const z = Math.max(1, Math.min(2.5, Math.floor((w / 480) * 2) / 2)); $('zoom').value = String(z); setZoom(z); };
  fit();

  const codes = [[200, 'OK 200'], [429, 'Limitato 429'], [404, 'N/D 404'], [500, 'Errore 500'], [401, 'Auth 401'], [-1, 'Rete (-1)']];
  $('modelRows').innerHTML = G.models.map((m, i) => `<div class="mrow"><span>${m.name}</span>
    <select data-m="${i}">${codes.map(([c, n]) => `<option value="${c}" ${API.probe[i].code === c ? 'selected' : ''}>${n}</option>`).join('')}</select>
    <input type="number" data-ms="${i}" value="${API.probe[i].ms}" step="100" title="latenza ms">
    <input type="checkbox" data-inc="${i}" title="incidente su status.claude.com"></div>`).join('')
    + '<p class="note">colonne: risposta sonda · latenza ms · incidente attivo</p>';
  $('modelRows').addEventListener('change', (e) => {
    const t = e.target;
    if (t.dataset.m) API.probe[+t.dataset.m].code = +t.value;
    if (t.dataset.ms) API.probe[+t.dataset.ms].ms = +t.value;
    if (t.dataset.inc) API.incident[+t.dataset.inc] = t.checked;
  });
  $('momentBtns').innerHTML = [0, 1].flatMap((w) => THR.map((t) => `<button data-w="${w}" data-t="${t}">${w ? 'Sett.' : '5h'} ${t}%</button>`)).join('');
  $('momentBtns').addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    if (state !== ST.MAIN) { slog('[SIM] i momenti si vedono sul dashboard'); return; }
    showMoment(+b.dataset.w, +b.dataset.t);
  });
  $('ccBtns').addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    if (state !== ST.MAIN) { slog('[SIM] gli avvisi si vedono sul dashboard'); return; }
    ccEvent(b.dataset.ev, 'ritmo-code', +b.dataset.dur);
  });
  document.querySelector('.panel').addEventListener('click', (e) => {
    const act = e.target.dataset && e.target.dataset.act; if (!act) return;
    if (act === 'boot-token') boot(true);
    if (act === 'boot-onboard') boot(false);
    if (act === 'refresh') { if (state === ST.MAIN) G.wantRefresh = true; else slog('[SIM] refresh disponibile sul dashboard'); }
    if (act === 'models-page') { openModelsPage(); return; }
    if (act === 'probe-all') {
      if (state !== ST.MAIN) { slog('[SIM] entra nel dashboard prima'); return; }
      for (let k = 0; k < 4; k++) probeNextModel();
      fetchModelStatus(); requestState(ST.MAIN);
    }
    if (act === 'add-acct') {
      const s = accountFirstFree(); if (s < 0) { slog('[SIM] 4/4 account già usati'); return; }
      const names = ['Personale', 'Lavoro', 'Universita', 'Cliente'];
      G.accts.used[s] = true; G.accts.label[s] = uniqueLabel(names[s], s);
      slog(`[SIM] account "${G.accts.label[s]}" aggiunto nello slot ${s}`);
      if ([ST.MAIN, ST.SETTINGS, ST.ACCOUNTS].includes(state)) requestState(state);
    }
  });
}

// accesso per tools/capture_screens.js (immagini del README)
window.__sim = { API, G, ST, P, boot, requestState, setTile, refreshUiValues, dashTick, showMoment, momentClose, ccEvent, noticeClose, tmMenuOpen, tmStart, TM, TM_TIMER, TM_FOCUS, TM_BREAK, pauseMenuOpen, pauseMenuClose, nightClockShow, nightClockClose };
initPanel();
applyBrightness();
boot(true);
requestAnimationFrame(loop);
})();
