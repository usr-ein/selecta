'use strict';
/* =====================================================================
   Selecta UI mock — sample data only. Nothing here talks to a network.
   ===================================================================== */

/* ---------- utilities ---------- */
const SR = 22050; // synthetic preview audio sample rate
const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => Array.from(el.querySelectorAll(s));
const esc = v => String(v ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const icon = (id, cls = '') => `<svg class="ic ${cls}" aria-hidden="true"><use href="#i-${id}"/></svg>`;
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const fmtDur = s => { s = Math.max(0, Math.round(s)); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; };
const fmtClock = s => { s = Math.max(0, s); const m = Math.floor(s / 60); return `${String(m).padStart(2, '0')}:${(s - m * 60).toFixed(1).padStart(4, '0')}`; };
const fmtSize = b => (b >= 1e9 ? `${(b / 1e9).toFixed(b >= 1e11 ? 0 : 1)} GB` : b >= 1e6 ? `${(b / 1e6).toFixed(b >= 1e8 ? 0 : 1)} MB` : `${Math.max(1, Math.round(b / 1e3))} KB`);
const fmtSpeed = v => (v >= 1e6 ? `${(v / 1e6).toFixed(1)} MB/s` : `${Math.round(v / 1e3)} KB/s`);
const fmtInt = n => Math.round(n).toLocaleString('en-US');
const plural = (n, one, many = one + 's') => `${fmtInt(n)} ${n === 1 ? one : many}`;
const khz = sr => (sr % 1000 ? (sr / 1000).toFixed(1) : String(sr / 1000));
function mulberry32(a) { return () => { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
function hashStr(s) { let h = 2166136261; for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); } return h >>> 0; }
const norm = s => String(s ?? '').toLowerCase().normalize('NFKD').replace(/[\u0300-\u036f]/g, '').replace(/&/g, ' and ').replace(/[^a-z0-9]+/g, ' ').trim();
const store = {
  get(k, d) { try { const v = localStorage.getItem('selecta-mock.' + k); return v == null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('selecta-mock.' + k, JSON.stringify(v)); } catch { /* storage unavailable: keep defaults */ } },
};
/* Simulated clock in ms. Batch "demo speed" makes it run faster than real time. */
const Clock = { t: 0, last: performance.now(), speed: 1, tick(now) { this.t += (now - this.last) * this.speed; this.last = now; } };
const artGradient = seed => { const r = mulberry32(seed); const h = Math.floor(r() * 360); return `linear-gradient(135deg, hsl(${h} 55% 52%), hsl(${(h + 40 + Math.floor(r() * 80)) % 360} 60% 28%))`; };

/* ---------- sample data (fictional artists, peers and files) ---------- */
const LOSSY = new Set(['MP3', 'AAC', 'OGG', 'OPUS']);
const FORMATS = ['FLAC', 'MP3', 'WAV', 'AIFF', 'ALAC', 'AAC', 'OGG', 'OPUS'];
const EXT = { FLAC: 'flac', MP3: 'mp3', WAV: 'wav', AIFF: 'aiff', ALAC: 'm4a', AAC: 'm4a', OGG: 'ogg', OPUS: 'opus' };
const RATES = [44100, 48000, 88200, 96000, 176400, 192000];
const LIMITS = { searches: 34, windowSec: 220, batchKeepFree: 8, peerConcurrent: 2, peerQueued: 10, peerSoft: 15, peerHard: 25 };

const CATALOG = [
  { artist: 'Kassia Moreau', album: 'Night Transit EP', year: 2023, genre: 'Deep Techno', bpm: 124, tracks: [['Night Transit', 'Original Mix', 408], ['Low Orbit', '', 422], ['Paper Lanterns', '', 375], ['Night Transit', 'Dub', 440]] },
  { artist: 'Okuma Drift', album: 'Neon Rain', year: 2024, genre: 'Progressive House', bpm: 122, tracks: [['Neon Rain', '', 372], ['Sodium', '', 381], ['Cold Harbour', '', 356], ['Afterlight', '', 402], ['Kinetic', '', 344], ['Glass Garden', '', 397], ['Undertow', '', 418], ['Static Bloom', '', 366], ['Last Train Home', '', 451]] },
  { artist: 'Halden & Rue', album: 'Salt Flats', year: 2022, genre: 'Deep House', bpm: 118, tracks: [['Salt Flats', '', 334], ['Tidewater', '', 362], ['Mirage Engine', '', 391], ['Heat Shimmer', '', 345], ['Dust Devil', '', 372], ['Long Shadows', '', 408], ['Brine', '', 301], ['Horizon Line', '', 433]] },
  { artist: 'Marlo Vance', album: 'Tidal Memory', year: 2021, genre: 'Techno', bpm: 126, single: true, tracks: [['Tidal Memory', 'Original Mix', 396], ['Tidal Memory', 'Ines Varga Remix', 412]] },
  { artist: 'Ines Varga', album: 'Glass Choir', year: 2025, genre: 'Melodic Techno', bpm: 125, tracks: [['Glass Choir', '', 384], ['Vesper', '', 402], ['Hollow Bells', '', 367]] },
  { artist: 'Teodor Lumen', album: 'Copper Season', year: 2020, genre: 'Downtempo', bpm: 112, tracks: [['Copper Season', '', 356], ['Fieldwork', '', 332], ['Amber Static', '', 389], ['Slow Lights', '', 415], ['Rust Belt Dreams', '', 371], ['Corridor', '', 348]] },
  { artist: 'Juno Ashgrove', album: 'Velvet Machine', year: 2024, genre: 'Tech House', bpm: 128, single: true, tracks: [['Velvet Machine', 'Extended Mix', 398], ['Velvet Machine', 'Radio Edit', 201]] },
  { artist: 'Sefa Kintu', album: 'Lagos Night Bus', year: 2023, genre: 'Afro House', bpm: 121, single: true, tracks: [['Lagos Night Bus', '', 377], ['Okada Ride', '', 352]] },
  { artist: 'Pale Monarch', album: 'Thermals', year: 2019, genre: 'Minimal', bpm: 123, tracks: [['Thermals', '', 366], ['Updraft', '', 384], ['Kestrel', '', 341], ['Cloud Street', '', 402], ['Ridge Lift', '', 359]] },
  { artist: 'Ravel Ortiz', album: 'Room 404', year: 2025, genre: 'Techno', bpm: 127, single: true, tracks: [['Room 404', '', 389], ['Room 404', 'Dub', 402]] },
];
CATALOG.forEach((rel, i) => { rel.id = i; });

const PEERS = [];
const PEER = {};
function addPeer(name, o = {}) {
  const p = { name, root: '@@' + name.replace(/[^a-z0-9]/gi, '').slice(0, 5).toLowerCase(), speed: 2e6, free: true, queue: 0, today: 0, session: 0, d30: 0, all: 0, files: 5000, last: null, backoff: null, active: 0, ...o };
  PEERS.push(p); PEER[name] = p; return p;
}
addPeer('vinyl_ghost', { speed: 4.2e6, today: 3, session: 3, d30: 18, all: 41, files: 18420, last: '19:41' });
addPeer('basement.tapes', { speed: 2.8e6, today: 2, session: 0, d30: 6, all: 12, files: 9311, last: 'Fri' });
addPeer('flac_archivist', { speed: 9.6e6, free: false, queue: 4, today: 14, session: 9, d30: 52, all: 96, files: 61250, last: '20:09' });
addPeer('dubplate_dan', { speed: 1.1e6, free: false, queue: 12, today: 25, session: 6, d30: 71, all: 133, files: 22004, last: '19:37' });
addPeer('lowend_theory', { speed: 3.4e6, d30: 2, all: 3, files: 7740, last: 'Sep 28' });
addPeer('mariposa_sound', { speed: 0.6e6, free: false, queue: 31, files: 3120 });
addPeer('nightbus_records', { speed: 6.1e6, today: 1, session: 1, d30: 4, all: 9, files: 12880, last: '20:11' });
addPeer('krautfunk', { speed: 0.35e6, files: 2210 });
addPeer('tapehiss', { speed: 1.9e6, free: false, queue: 2, today: 1, session: 1, d30: 3, all: 3, files: 4475, last: '20:06', backoff: { reason: 'Too many files', ago: '12 min ago' } });
addPeer('groove_salad', { speed: 2.2e6, files: 15880, d30: 1, all: 6, last: 'Oct 2' });
addPeer('selector_k', { speed: 5.4e6, today: 1, session: 1, files: 30115, d30: 3, all: 11, last: '20:01' });
addPeer('8bitlotus', { speed: 0.9e6, free: false, queue: 6, files: 1288 });
addPeer('cratedigger_77', { speed: 3.9e6, files: 44120, d30: 7, all: 27, last: 'Oct 12' });
addPeer('reel2reel', { speed: 1.4e6, files: 6650 });
addPeer('subharmonic', { speed: 7.2e6, files: 25500, d30: 2, all: 8, last: 'Oct 5' });
addPeer('quietstorm', { speed: 0.8e6, free: false, queue: 19, files: 3900 });
['velvet_static', 'echo_chamber', 'acid_rain_99', 'lofi_lena', 'dancefloor_dad', 'marsh_tones', 'hypnotic_hal', 'bassbin', 'strobe_kid', 'warehouse_w', 'oddfellow', 'plastic_pal', 'minimal_mo', 'delay_line', 'tape_saturator', 'modular_mira', 'offbeat_otto', 'sinewave_sue', 'kickdrum_kai', 'fm_synth_fran', 'opal_oscillator', 'peaktime_pete', 'dusty_grooves', 'afterhours_ana'].forEach(n => {
  const r = mulberry32(hashStr(n)); const free = r() > 0.35;
  addPeer(n, { speed: (0.3 + r() * 6) * 1e6, free, queue: free ? 0 : 1 + Math.floor(r() * 25), files: 800 + Math.floor(r() * 30000), d30: Math.floor(r() * 3) });
});
const capState = p => (p.today >= LIMITS.peerHard ? 'cap' : p.today >= Math.round(LIMITS.peerSoft * 0.8) ? 'near' : 'ok');

const VARIANTS = [
  { fmt: 'MP3', br: 320, vbr: false, w: 34, tag: 'MP3 320' },
  { fmt: 'MP3', br: 245, vbr: true, w: 11, tag: 'V0' },
  { fmt: 'MP3', br: 256, vbr: false, w: 7, tag: '256' },
  { fmt: 'MP3', br: 192, vbr: false, w: 9, tag: 'MP3' },
  { fmt: 'MP3', br: 128, vbr: false, w: 4, tag: 'MP3' },
  { fmt: 'FLAC', sr: 44100, bd: 16, w: 15, tag: 'FLAC' },
  { fmt: 'FLAC', sr: 96000, bd: 24, w: 5, tag: '24-96 FLAC' },
  { fmt: 'FLAC', sr: 48000, bd: 24, w: 2, tag: '24-48' },
  { fmt: 'WAV', sr: 44100, bd: 16, w: 4, tag: 'WAV' },
  { fmt: 'AIFF', sr: 44100, bd: 16, w: 5, tag: 'AIFF' },
  { fmt: 'ALAC', sr: 44100, bd: 16, w: 2, tag: 'ALAC' },
  { fmt: 'AAC', br: 256, vbr: false, w: 2, tag: 'AAC' },
  { fmt: 'OGG', br: 320, vbr: true, w: 1, tag: 'OGG' },
];
const VARIANT_W = VARIANTS.reduce((a, v) => a + v.w, 0);
function pickVariant(r) { let x = r() * VARIANT_W; for (const v of VARIANTS) { x -= v.w; if (x < 0) return v; } return VARIANTS[0]; }

/* The maintainer's collection (read-only for Selecta) */
const COLLECTION = {
  root: '~/Music/Collection', singlesDir: 'Singles', albumsDir: 'Albums',
  stats: { tracks: 8412, bytes: 214e9, singles: 2931, albums: 412 },
  formats: [['MP3', 6021], ['FLAC', 1512], ['AIFF', 640], ['WAV', 239]],
  tracks: [
    { artist: 'Kassia Moreau', title: 'Paper Lanterns', version: '', fmt: 'MP3', br: 320, path: 'Singles/Kassia Moreau - Paper Lanterns.mp3' },
    { artist: 'Marlo Vance', title: 'Tidal Memory', version: 'Original Mix', fmt: 'MP3', br: 192, path: 'Singles/Marlo Vance - Tidal Memory (Original Mix).mp3' },
    { artist: 'Halden & Rue', title: 'Tidewater', version: '', fmt: 'MP3', br: 320, path: 'Singles/Halden & Rue - Tidewater.mp3' },
    { artist: 'Pale Monarch', title: 'Kestrel', version: '', fmt: 'MP3', br: 256, path: 'Singles/Pale Monarch - Kestrel.mp3' },
    { artist: 'Ines Varga', title: 'Hollow Bells', version: 'Edit', fmt: 'MP3', br: 320, path: 'Singles/Ines Varga - Hollow Bells (Edit).mp3' },
    { artist: 'Juno Ashgrove', title: 'Velvet Machine', version: 'Club Mix', fmt: 'AIFF', sr: 44100, bd: 16, path: 'Singles/Juno Ashgrove - Velvet Machine (Club Mix).aiff' },
    ...CATALOG[5].tracks.map((t, i) => ({ artist: 'Teodor Lumen', title: t[0], version: '', fmt: 'MP3', br: 320, album: 'Copper Season', path: `Albums/Teodor Lumen - Copper Season (2020)/${String(i + 1).padStart(2, '0')} - ${t[0]}.mp3` })),
  ],
  singleAlbumPairs: [
    { single: 'Singles/Halden & Rue - Tidewater.mp3', album: 'Halden & Rue — Salt Flats (2022)', where: 'Inbox' },
    { single: 'Singles/Pale Monarch - Kestrel.mp3', album: 'Pale Monarch — Thermals (2019)', where: 'Albums' },
  ],
};

const SESSIONS = [
  { n: 15, when: 'Today', range: '19:58 – now', current: true },
  { n: 14, when: 'Fri 17 Oct', range: '21:10 – 23:42' },
  { n: 13, when: 'Sun 12 Oct', range: '15:02 – 16:30' },
  { n: 12, when: 'Sun 5 Oct', range: '20:45 – 22:10' },
];

const PRESETS = {
  dj: { name: 'DJ · MP3 320 or lossless', formats: ['MP3', 'FLAC', 'WAV', 'AIFF', 'ALAC'], brOp: '=', brVal: 320, cbrOnly: true, minBits: 16, rates: [44100, 48000], minDur: 120, maxDur: 900, strict: true, freeOnly: false, maxQueue: 50, owned: 'dim' },
  mp3: { name: 'MP3 320 CBR only', formats: ['MP3'], brOp: '=', brVal: 320, cbrOnly: true, minBits: 16, rates: [44100, 48000], minDur: 120, maxDur: 900, strict: true, freeOnly: false, maxQueue: 50, owned: 'dim' },
  lossless: { name: 'Lossless CD (16/44.1+)', formats: ['FLAC', 'WAV', 'AIFF', 'ALAC'], brOp: 'any', brVal: 320, cbrOnly: false, minBits: 16, rates: [44100, 48000], minDur: 60, maxDur: 1200, strict: true, freeOnly: false, maxQueue: 50, owned: 'dim' },
  hires: { name: 'Hi-res (24-bit)', formats: ['FLAC', 'WAV', 'AIFF', 'ALAC'], brOp: 'any', brVal: 320, cbrOnly: false, minBits: 24, rates: [48000, 88200, 96000, 176400, 192000], minDur: 60, maxDur: 1200, strict: true, freeOnly: false, maxQueue: 50, owned: 'show' },
};
const presetFilters = id => { const p = PRESETS[id]; return { formats: new Set(p.formats), brOp: p.brOp, brVal: p.brVal, cbrOnly: p.cbrOnly, minBits: p.minBits, rates: new Set(p.rates), minDur: p.minDur, maxDur: p.maxDur, strict: p.strict, freeOnly: p.freeOnly, maxQueue: p.maxQueue, owned: p.owned }; };

/* ---------- app state ---------- */
const S = {
  view: 'search', searchId: null, searches: [], nextSearch: 1, nextId: 1, rows: [], _v: null,
  filters: presetFilters('dj'), presetId: 'dj', presetDirty: false,
  group: 'folders', sort: 'best', sortDir: -1, collapsed: new Set(), moreFilters: false,
  sel: null, inspector: true, sidebar: true,
  budget: [], cache: new Map(), results: new Map(),
  netPaused: false, server: 'live', theme: store.get('theme', 'system'),
  autoPreview: false, cancelOnSkip: true,
  transfers: [], uploads: [], inbox: [], history: [], msgs: [], wishlist: [],
  peersWindow: '30d', transfersTab: 'down', msgSel: 0,
  filingTarget: 'mp3-320', inboxSel: new Set(), batch: null, conv: null,
};

/* ---------- search engine (simulated peers) ---------- */
const nn2 = i => String(i + 1).padStart(2, '0');
const fullTitle = (t, v) => (v ? `${t} (${v})` : t);
const sceneName = s => s.replace(/&/g, 'and').replace(/[^A-Za-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
const albumDir = rel => `${rel.artist} - ${rel.album} (${rel.year})`;
function shuffle(a, r) { for (let i = a.length - 1; i > 0; i--) { const j = Math.floor(r() * (i + 1)); [a[i], a[j]] = [a[j], a[i]]; } return a; }
const FOLDER_STYLES = new Set(['album', 'year', 'scene']);
function makePath(style, p, rel, i, v, grp) {
  const t = rel.tracks[i], title = fullTitle(t[0], t[1]), ext = EXT[v.fmt];
  switch (style) {
    case 'album': return `${p.root}\\Music\\${rel.artist}\\${rel.artist} - ${rel.album} (${rel.year}) [${v.tag}]\\${nn2(i)} - ${rel.artist} - ${title}.${ext}`;
    case 'year': return `${p.root}\\Electronic\\${rel.year}\\${rel.album}\\${nn2(i)}. ${title}.${ext}`;
    case 'scene': { const d = `${sceneName(rel.artist)}-${sceneName(rel.album)}-WEB-${rel.year}-${grp}`; return `${p.root}\\complete\\${d}\\${nn2(i)}-${sceneName(rel.artist).toLowerCase()}-${sceneName(title).toLowerCase()}.${ext}`; }
    case 'crate': return `${p.root}\\DJ\\Crates\\${rel.genre}\\${rel.artist} - ${title}.${ext}`;
    default: return `${p.root}\\Downloads\\${rel.artist} - ${title}.${ext}`;
  }
}
function pseudoReleases(query, r) {
  const title = query.trim().split(/\s+/).map(w => w.charAt(0).toUpperCase() + w.slice(1).toLowerCase()).join(' ');
  const artists = ['Mirela Stone', 'Odd Harbour', 'Tamsin Reyes', 'Kofi Mensah-Lund', 'Nadia Krell'];
  const versions = ['', 'Original Mix', 'Extended Mix', 'Dub', 'Radio Edit'];
  const out = [];
  for (let i = 0, n = 2 + Math.floor(r() * 3); i < n; i++) {
    out.push({ idx: [0], rel: { id: `x${i}`, pseudo: true, artist: artists[Math.floor(r() * artists.length)], album: title, year: 2017 + Math.floor(r() * 9), genre: 'House', bpm: 118 + Math.floor(r() * 10), single: true, tracks: [[title, versions[Math.floor(r() * versions.length)], 290 + Math.floor(r() * 170)]] } });
  }
  return out;
}
function generateResults(query, { pseudo = true, salt = 0 } = {}) {
  const qn = norm(query), toks = qn.split(' ').filter(Boolean);
  const r = mulberry32((hashStr(qn) ^ Math.imul(salt + 1, 2654435761)) >>> 0);
  const hits = new Map();
  for (const rel of CATALOG) {
    const relText = norm(`${rel.artist} ${rel.album} ${rel.year}`);
    rel.tracks.forEach((t, i) => {
      const text = `${relText} ${norm(t[0])} ${norm(t[1])}`;
      if (toks.length && toks.every(k => text.includes(k))) { if (!hits.has(rel)) hits.set(rel, []); hits.get(rel).push(i); }
    });
  }
  let rels = [...hits.entries()].map(([rel, idx]) => ({ rel, idx }));
  if (!rels.length && pseudo && toks.length) rels = pseudoReleases(query, r);
  const out = [];
  for (const { rel, idx } of rels) {
    const pool = shuffle(PEERS.slice(), r).slice(0, 7 + Math.floor(r() * (rel.pseudo ? 6 : 13)));
    for (const p of pool) {
      const v = pickVariant(r), sr = r();
      const style = rel.tracks.length > 1 ? (sr < 0.36 ? 'album' : sr < 0.5 ? 'year' : sr < 0.62 ? 'scene' : sr < 0.82 ? 'crate' : 'loose') : (sr < 0.55 ? 'loose' : 'crate');
      const grp = ['PTC', 'HYPE', 'WAX', 'DRT', 'MOS'][Math.floor(r() * 5)];
      const picks = FOLDER_STYLES.has(style) ? idx : [idx[Math.floor(r() * idx.length)]];
      const arrival = 120 + r() * 2800 * (1.4 - Math.min(1, p.speed / 8e6));
      for (const i of picks) {
        const t = rel.tracks[i];
        const dur = Math.max(60, t[2] + Math.round((r() - 0.5) * 3));
        let kbps;
        if (LOSSY.has(v.fmt)) kbps = v.vbr ? v.br * (0.93 + r() * 0.12) : v.br;
        else if (v.fmt === 'FLAC') kbps = v.bd === 24 ? (v.sr >= 88200 ? 2700 + r() * 600 : 1500 + r() * 320) : 780 + r() * 270;
        else if (v.fmt === 'ALAC') kbps = 820 + r() * 130;
        else kbps = (v.sr * v.bd * 2) / 1000;
        let size = (kbps * 1000 / 8) * dur + 60e3 + r() * 140e3;
        let suspicious = null;
        const roll = r();
        if (v.fmt === 'MP3' && v.br === 320 && roll < 0.08) { size = (128 * 1000 / 8) * dur + 90e3; suspicious = 'Size fits ~128 kbps, not 320'; }
        else if (roll > 0.975) { size *= 0.38; suspicious = `Size fits only ~${fmtDur(dur * 0.38)} of audio: probably truncated`; }
        const unknown = r() < 0.07;
        const path = makePath(style, p, rel, i, v, grp), cut = path.lastIndexOf('\\');
        const res = {
          id: `r${S.nextId++}`, peer: p, path, folder: path.slice(0, cut), file: path.slice(cut + 1), style,
          rel, ti: i, trackNo: FOLDER_STYLES.has(style) ? i + 1 : 0, artist: rel.artist, title: t[0], version: t[1], dur,
          fmt: v.fmt, br: unknown ? null : v.br ?? null, vbr: !!v.vbr, sr: unknown ? null : v.sr ?? null, bd: unknown ? null : v.bd ?? null,
          size, suspicious, arrival,
        };
        S.results.set(res.id, res);
        out.push(res);
      }
    }
  }
  out.sort((a, b) => a.arrival - b.arrival);
  return out;
}

/* ---------- quality, ownership, filters ---------- */
const estKbps = r => Math.round((r.size * 8) / r.dur / 1000);
function qualShort(r) {
  if (LOSSY.has(r.fmt)) {
    if (r.br == null) return { text: `~${estKbps(r)} kbps`, unknown: true };
    if (r.vbr) return { text: r.br >= 220 ? `V0 ~${r.br}` : `VBR ~${r.br}` };
    return { text: `${r.br} kbps` };
  }
  if (r.sr == null) return { text: `~${fmtInt(estKbps(r))} kbps`, unknown: true };
  return { text: `${r.bd} / ${khz(r.sr)}` };
}
const qualLong = x => (LOSSY.has(x.fmt) ? `${x.fmt} ${x.br != null ? (x.vbr ? `VBR ~${x.br}` : x.br) : '?'}` : `${x.fmt} ${x.bd ? `${x.bd}/${khz(x.sr)}` : ''}`).trim();
function qrank(x) { if (!x) return 0; if (!LOSSY.has(x.fmt)) return x.bd ? 1000 + (x.bd - 16) * 10 : 990; if (x.br == null) return 100; return x.fmt === 'MP3' ? x.br : x.br + 10; }
const ORIGINAL = new Set(['', 'original mix', 'original', 'album version']);
function versionRel(a, b) { const x = norm(a), y = norm(b); if (x === y || (ORIGINAL.has(x) && ORIGINAL.has(y))) return 'same'; if (ORIGINAL.has(x) || ORIGINAL.has(y)) return 'unsure'; return 'diff'; }
const OWN_ORDER = { owned: 4, upgrade: 3, likely: 2, version: 1 };
function ownership(r) {
  const a = norm(r.artist), t = norm(r.title);
  const tr = S.transfers.find(x => x.rid === r.id && x.kind !== 'preview' && !['done', 'failed', 'cancelled'].includes(x.state));
  if (tr) return { kind: 'queued', tr };
  for (const it of S.inbox) {
    if (norm(it.artist) !== a) continue;
    if (it.kind === 'single' && norm(it.title) === t && versionRel(it.version, r.version) === 'same') return { kind: 'inbox', item: it };
    if (it.kind === 'album' && it.rel && it.rel.tracks.some(x => norm(x[0]) === t && versionRel(x[1], r.version) === 'same')) return { kind: 'inbox', item: it };
  }
  let best = null;
  for (const c of COLLECTION.tracks) {
    if (norm(c.artist) !== a || norm(c.title) !== t) continue;
    const vr = versionRel(c.version, r.version);
    const k = vr === 'same' ? (qrank(c) < Math.min(320, qrank(r)) ? 'upgrade' : 'owned') : vr === 'unsure' ? 'likely' : 'version';
    if (!best || OWN_ORDER[k] > OWN_ORDER[best.kind]) best = { kind: k, track: c };
  }
  if (best) return best;
  const h = S.history.find(x => !x.album && norm(x.artist) === a && norm(x.title) === t && versionRel(x.version, r.version) === 'same');
  if (h) return { kind: h.session === SESSIONS[0].n ? 'session' : 'past', rec: h };
  return null;
}
const BADGES = {
  owned: { ic: 'check-c', cls: 'good', label: 'Owned', long: 'In your collection' },
  upgrade: { ic: 'up-c', cls: 'acc', label: 'Upgrade', long: 'You own a lower-quality copy' },
  likely: { ic: 'approx-c', cls: 'warn', label: 'Likely', long: 'Probably in your collection; the version is unclear' },
  version: { ic: 'approx-c', cls: 'mute', label: 'Other mix', long: 'You own a different mix of this track' },
  inbox: { ic: 'tray', cls: 'acc', label: 'Inbox', long: 'In your Inbox, not filed yet' },
  session: { ic: 'download', cls: 'acc', label: 'This session', long: 'Downloaded this session' },
  past: { ic: 'clock', cls: 'mute', label: 'Earlier', long: 'Downloaded in an earlier session' },
  queued: { ic: 'hourglass', cls: 'acc', label: 'Queued', long: 'In your transfers' },
};
const HAVE = new Set(['owned', 'inbox', 'session', 'queued']);
function ownDetail(o) {
  if (!o) return '';
  if (o.track) return `${COLLECTION.root}/${o.track.path} (${qualLong(o.track)})`;
  if (o.item) return `Inbox · ${o.item.kind === 'album' ? `${o.item.artist} - ${o.item.album}` : `${o.item.artist} - ${fullTitle(o.item.title, o.item.version)}`}`;
  if (o.rec) return `Session ${o.rec.session} · from ${o.rec.peer} · ${o.rec.where}`;
  if (o.tr) return o.tr.state === 'active' ? `Downloading, ${Math.floor(o.tr.progress * 100)}%` : 'Waiting in your transfers';
  return '';
}
function badgeHtml(o) {
  if (!o) return '';
  const b = BADGES[o.kind];
  let label = b.label;
  if (o.kind === 'queued' && o.tr.state === 'active') label = `${Math.floor(o.tr.progress * 100)}%`;
  if (o.kind === 'past') label = `Session ${o.rec.session}`;
  return `<span class="bdg ${b.cls}" title="${esc(`${b.long}: ${ownDetail(o)}`)}">${icon(b.ic)}${esc(label)}</span>`;
}
const REASONS = { format: 'Format', bitrate: 'Bitrate', lossless: 'Bit depth or rate', unknown: 'Quality not reported', suspicious: 'Size mismatch', length: 'Length', peer: 'Peer busy', owned: 'Already have' };
function hiddenReason(r, f, own) {
  if (!f.formats.has(r.fmt)) return 'format';
  if (LOSSY.has(r.fmt)) {
    if (r.br == null) { if (f.strict) return 'unknown'; }
    else if (f.brOp !== 'any') {
      if (f.cbrOnly && r.vbr) return 'bitrate';
      if (f.brOp === '=' && r.br !== f.brVal) return 'bitrate';
      if (f.brOp === '>=' && r.br < f.brVal) return 'bitrate';
      if (f.brOp === '<=' && r.br > f.brVal) return 'bitrate';
    }
  } else if (r.sr == null) { if (f.strict) return 'unknown'; }
  else if (r.bd < f.minBits || !f.rates.has(r.sr)) return 'lossless';
  if (r.dur < f.minDur || r.dur > f.maxDur) return 'length';
  if (f.strict && r.suspicious) return 'suspicious';
  if ((f.freeOnly && !r.peer.free) || r.peer.queue > f.maxQueue) return 'peer';
  if (f.owned === 'hide' && own && HAVE.has(own.kind)) return 'owned';
  return null;
}
function score(r) {
  let s = LOSSY.has(r.fmt) ? (r.br == null ? 0.2 : Math.min(1, r.br / 320) - (r.vbr ? 0.04 : 0)) : (r.sr == null ? 0.3 : 0.97 - (r.bd > 16 ? 0.05 : 0));
  s += r.peer.free ? 0.7 : Math.max(0, 0.45 - r.peer.queue * 0.02);
  s += Math.min(1, r.peer.speed / 6e6) * 0.35;
  s -= (r.peer.today / LIMITS.peerHard) * 0.5;
  if (r.suspicious) s -= 0.9;
  return s;
}
const SORTS = {
  best: { label: 'Best match', key: r => r._score, dir: -1 },
  quality: { label: 'Quality', key: r => qrank(r), dir: -1 },
  avail: { label: 'Availability', key: r => (r.peer.free ? 1e9 : 0) - r.peer.queue * 1e6 + r.peer.speed / 10, dir: -1 },
  fair: { label: 'Fewest from peer', key: r => r.peer.today, dir: 1 },
  title: { label: 'Title', key: r => `${r.artist} ${r.title} ${r.version}`, str: true, dir: 1 },
  format: { label: 'Format', key: r => r.fmt, str: true, dir: 1 },
  length: { label: 'Length', key: r => r.dur, dir: -1 },
  size: { label: 'Size', key: r => r.size, dir: -1 },
  peer: { label: 'Peer', key: r => r.peer.name, str: true, dir: 1 },
};
function summarizeQuality(items) {
  const set = [...new Set(items.map(qualLong))];
  return set.length <= 2 ? set.join(', ') : `${set.length} qualities`;
}
function computeView(s) {
  const f = S.filters, all = s.results.slice(0, s.revealed);
  const shown = [], why = {}, fmtCount = {};
  for (const r of all) {
    fmtCount[r.fmt] = (fmtCount[r.fmt] || 0) + 1;
    r._own = ownership(r); r._score = score(r);
    const reason = hiddenReason(r, f, r._own);
    if (reason) why[reason] = (why[reason] || 0) + 1; else shown.push(r);
  }
  const srt = SORTS[S.sort], dir = S.sortDir;
  const cmp = (a, b) => (srt.str ? srt.key(a).localeCompare(srt.key(b)) : srt.key(a) - srt.key(b)) * dir || b._score - a._score;
  shown.sort(cmp);
  let groups = null; const byId = new Map();
  if (S.group === 'folders') {
    const map = new Map();
    for (const r of shown) {
      const key = `${r.peer.name}|${r.folder}`;
      if (!map.has(key)) map.set(key, { key, id: 'g' + hashStr(key).toString(36), items: [], peer: r.peer, rel: r.rel, folder: r.folder, album: FOLDER_STYLES.has(r.style) && r.rel.tracks.length > 1 });
      map.get(key).items.push(r);
    }
    groups = [...map.values()];
    for (const g of groups) {
      g.items.sort((a, b) => a.trackNo - b.trackNo || cmp(a, b));
      g.size = g.items.reduce((a, r) => a + r.size, 0);
      g.total = g.album ? g.rel.tracks.length : g.items.length;
      byId.set(g.id, g);
    }
  }
  return { all, shown, why, fmtCount, groups, byId, peers: new Set(all.map(r => r.peer.name)).size };
}
const curSearch = () => S.searches.find(s => s.id === S.searchId) || null;
function ago(t) { const d = (Clock.t - t) / 1000; return d < 50 ? 'just now' : d < 3600 ? `${Math.round(d / 60)} min ago` : `${Math.round(d / 3600)} h ago`; }

/* ---------- search budget (network governor, simulated) ---------- */
function budgetUses() { const w = LIMITS.windowSec * 1000; S.budget = S.budget.filter(t => Clock.t - t < w); return S.budget; }
const budgetLeft = () => LIMITS.searches - budgetUses().length;
function budgetRefillIn() { const u = budgetUses(); return u.length ? Math.max(0, (u[0] + LIMITS.windowSec * 1000 - Clock.t) / 1000) : 0; }

function startSearch(query, { refresh = false } = {}) {
  query = query.trim().replace(/\s+/g, ' ');
  if (!query) return;
  const qn = norm(query);
  const existing = S.searches.find(s => norm(s.query) === qn);
  if (existing && !refresh) { nav('search:' + existing.id); return; }
  const cached = S.cache.get(qn);
  if (cached && !refresh) {
    const s = { id: 's' + S.nextSearch++, query, results: cached.results, revealed: cached.results.length, status: 'done', startedAt: cached.at, cachedAt: cached.at, sel: null };
    S.searches.unshift(s); nav('search:' + s.id);
    toast('Showing cached results', `“${esc(query)}” from ${ago(cached.at)}. No search was sent.`, { icon: 'clock' });
    return;
  }
  if (S.netPaused) { toast('Network activity is paused', 'Resume it from the toolbar to search.', { kind: 'warn' }); return; }
  if (budgetLeft() <= 0) { toast('Search budget used up', `The next search frees up in ${Math.ceil(budgetRefillIn())} s. Cached searches still open instantly.`, { kind: 'warn', icon: 'gauge' }); return; }
  S.budget.push(Clock.t);
  const prior = refresh && existing;
  const results = generateResults(query, { salt: prior ? (prior.salt || 0) + 1 : 0 });
  if (prior) {
    Object.assign(prior, { results, revealed: 0, status: 'running', startedAt: Clock.t, cachedAt: null, salt: (prior.salt || 0) + 1, sel: null });
    S.sel = null; nav('search:' + prior.id); return;
  }
  const s = { id: 's' + S.nextSearch++, query, results, revealed: 0, status: 'running', startedAt: Clock.t, cachedAt: null, sel: null };
  S.searches.unshift(s);
  nav('search:' + s.id);
}
function updateSearches() {
  let changed = false;
  for (const s of S.searches) {
    if (s.status !== 'running') continue;
    const el = Clock.t - s.startedAt;
    let n = s.revealed;
    while (n < s.results.length && s.results[n].arrival <= el) n++;
    if (n !== s.revealed) { s.revealed = n; changed = true; }
    if (el > 3300) { s.revealed = s.results.length; s.status = 'done'; S.cache.set(norm(s.query), { results: s.results, at: Clock.t }); changed = true; }
  }
  return changed;
}

/* ---------- toolbar, sidebar ---------- */
function ratioFor(w) { const x = RATIO[w]; return x.down.bytes ? x.up.bytes / x.down.bytes : 0; }
function budgetHtml() {
  const left = budgetLeft(), pct = left / LIMITS.searches;
  return `${icon('gauge')}<b>${left}</b><span class="mini-meter${pct < 0.3 ? ' warn' : ''}" aria-hidden="true"><i style="width:${pct * 100}%"></i></span>`;
}
function renderToolbar() {
  const [title, sub] = VIEWS[S.view].title();
  const r30 = ratioFor('30d');
  $('#toolbar').innerHTML = `
    <div class="tb-lights lights" aria-hidden="true"><i></i><i></i><i></i></div>
    <button class="tbtn" data-act="toggle-sidebar" aria-label="Show or hide the sidebar" title="Sidebar">${icon('sidebar')}</button>
    <div class="tb-title"><b id="tbTitle">${esc(title)}</b><span id="tbSub">${esc(sub)}</span></div>
    <div class="tb-spacer"></div>
    ${S.view === 'search' && curSearch() ? viewControlsHtml() : ''}
    <div class="tb-group tb-hide-sm">
      <button class="tbtn" id="tbBudget" data-act="budget-info" title="Searches left in the current ${LIMITS.windowSec}-second window (limit ${LIMITS.searches})">${budgetHtml()}</button>
      <button class="tbtn" data-act="nav" data-v="peers" title="Uploaded ÷ downloaded over the last 30 days">${icon('transfers')}<b>${r30.toFixed(2)}</b><span class="tb-hide-md">ratio</span></button>
    </div>
    <div class="tb-group">
      <button class="tbtn${S.netPaused ? ' on' : ''}" data-act="toggle-pause" aria-pressed="${S.netPaused}" title="${S.netPaused ? 'Resume network activity' : 'Pause network activity (no new searches or transfers)'}">${icon(S.netPaused ? 'play-c' : 'pause-c')}</button>
      <button class="tbtn tb-hide-sm" data-act="cycle-theme" title="Appearance: ${S.theme === 'system' ? 'follows the system' : S.theme} (click to change)">${icon('appearance')}</button>
      <button class="tbtn${S.inspector ? ' on' : ''}" data-act="toggle-inspector" aria-pressed="${S.inspector}" title="Inspector (⌘I)"${VIEWS[S.view].insp ? '' : ' disabled'}>${icon('inspector')}</button>
    </div>`;
}
function navModel() {
  const unread = S.msgs.reduce((a, m) => a + m.unread, 0);
  const active = S.transfers.filter(t => t.kind !== 'preview' && !['done', 'failed', 'cancelled'].includes(t.state)).length;
  return [
    { items: [{ id: 'batch', ic: 'batch', label: 'Batch search', cnt: S.batch && S.batch.running ? 'spin' : '' }, { id: 'wishlist', ic: 'star', label: 'Wishlist', cnt: S.wishlist.reduce((a, w) => a + w.found, 0) || '' }] },
    { title: 'Searches', items: S.searches.map(s => ({ id: 'search:' + s.id, ic: 'search', label: s.query, cnt: s.status === 'running' ? 'spin' : shownCount(s), close: s.id })) },
    { title: 'Library', items: [{ id: 'inbox', ic: 'inbox', label: 'Inbox', cnt: S.inbox.length || '' }, { id: 'collection', ic: 'music', label: 'Collection' }, { id: 'history', ic: 'history', label: 'History' }] },
    { title: 'Network', items: [{ id: 'transfers', ic: 'transfers', label: 'Transfers', cnt: active || '' }, { id: 'peers', ic: 'peers', label: 'Peers & ratio' }, { id: 'messages', ic: 'msg', label: 'Messages', cnt: unread ? { pill: unread } : '' }, { id: 'shares', ic: 'share', label: 'Shares' }] },
    { title: 'Tools', items: [{ id: 'converter', ic: 'convert', label: 'Converter' }, { id: 'settings', ic: 'settings', label: 'Settings' }] },
  ];
}
function shownCount(s) { let n = 0; for (let i = 0; i < s.revealed; i++) { const r = s.results[i]; if (!hiddenReason(r, S.filters, ownership(r))) n++; } return n; }
function renderNav() {
  const cur = S.view === 'search' ? 'search:' + S.searchId : S.view;
  const html = navModel().map(sec => `<div class="nav-sec">${sec.title ? `<div class="nav-title">${sec.title}</div>` : ''}${sec.items.length ? sec.items.map(it => {
    const on = it.id === cur;
    const cnt = it.cnt === 'spin' ? '<span class="spinner" role="img" aria-label="Running"></span>' : it.cnt && it.cnt.pill ? `<span class="cnt pill">${it.cnt.pill}</span>` : it.cnt !== '' && it.cnt != null ? `<span class="cnt">${fmtInt(it.cnt)}</span>` : '';
    return `<div class="nav-item${on ? ' on' : ''}" role="button" tabindex="0" data-act="nav" data-v="${esc(it.id)}"${on ? ' aria-current="page"' : ''}>${icon(it.ic)}<span class="lbl">${esc(it.label)}</span>${it.close ? `<span class="x" role="button" tabindex="-1" aria-label="Close this search" data-act="close-search" data-v="${it.close}">${icon('x')}</span>` : ''}${cnt}</div>`;
  }).join('') : '<div class="nav-title" style="font-weight:400">Searches you run appear here.</div>'}</div>`).join('');
  if (html !== renderNav.last) { $('#nav').innerHTML = html; renderNav.last = html; }
}
function renderFoot() {
  $('#sbfoot').innerHTML = `<div class="who"><span class="dot${S.netPaused ? ' warn' : ''}"></span>${S.netPaused ? 'Paused' : 'Online'} as <b>selecta_dig</b></div>
    <div>${S.server === 'live' ? 'Soulseek network' : '<span class="tag dev">DEV</span> Local test server'} · port 2234 open</div>`;
}

/* ---------- search view ---------- */
function filtersHtml() {
  const f = S.filters;
  const presetOpts = Object.entries(PRESETS).map(([id, p]) => `<option value="${id}"${id === S.presetId ? ' selected' : ''}>${esc(p.name)}${id === S.presetId && S.presetDirty ? ' (edited)' : ''}</option>`).join('');
  const chips = FORMATS.map(x => `<button class="chip${f.formats.has(x) ? ' on' : ''}" data-act="fmt" data-v="${x}" aria-pressed="${f.formats.has(x)}">${x}<span class="n" data-fmt-count="${x}">0</span></button>`).join('');
  const ops = [['any', 'Any'], ['=', '='], ['>=', '≥'], ['<=', '≤']].map(([v, l]) => `<option value="${v}"${v === f.brOp ? ' selected' : ''}>${l}</option>`).join('');
  const brs = [128, 160, 192, 224, 256, 320].map(b => `<option value="${b}"${b === f.brVal ? ' selected' : ''}>${b}</option>`).join('');
  const rates = RATES.map(sr => `<button class="chip${f.rates.has(sr) ? ' on' : ''}" data-act="rate" data-v="${sr}" aria-pressed="${f.rates.has(sr)}">${khz(sr)}</button>`).join('');
  return `
  <div class="vb-row">
    <label class="fl"><span class="fl-l">Preset</span><select id="fPreset" data-ch="preset">${presetOpts}</select></label>
    <div class="fl" role="group" aria-label="Formats"><span class="fl-l">Formats</span><span class="chips">${chips}</span></div>
    <button class="btn sm more-filters" data-act="more-filters">${S.moreFilters ? 'Fewer filters' : 'More filters'}</button>
  </div>
  <div class="vb-row secondary">
    <div class="fl"><span class="fl-l">Lossy</span><select id="fBrOp" data-ch="brOp" aria-label="Bitrate rule">${ops}</select><select id="fBrVal" data-ch="brVal" aria-label="Bitrate in kbps"${f.brOp === 'any' ? ' disabled' : ''}>${brs}</select><span class="fl-l">kbps</span><label class="cb"><input type="checkbox" id="fCbr" data-ch="cbrOnly"${f.cbrOnly ? ' checked' : ''}>CBR only</label></div>
    <div class="fl"><span class="fl-l">Lossless</span><select id="fBits" data-ch="minBits" aria-label="Minimum bit depth"><option value="16"${f.minBits === 16 ? ' selected' : ''}>≥ 16-bit</option><option value="24"${f.minBits === 24 ? ' selected' : ''}>≥ 24-bit</option></select><span class="chips" role="group" aria-label="Allowed sample rates">${rates}</span><span class="fl-l">kHz</span></div>
    <div class="fl"><span class="fl-l">Length</span><input class="field num" id="fMin" data-ch="minDur" value="${fmtDur(f.minDur)}" aria-label="Minimum length"><span class="fl-l">to</span><input class="field num" id="fMax" data-ch="maxDur" value="${fmtDur(f.maxDur)}" aria-label="Maximum length"></div>
    <div class="fl"><label class="cb"><input type="checkbox" id="fFree" data-ch="freeOnly"${f.freeOnly ? ' checked' : ''}>Free slot only</label><span class="fl-l">Queue ≤</span><select id="fQueue" data-ch="maxQueue" aria-label="Maximum peer queue">${[5, 10, 20, 50].map(q => `<option value="${q}"${q === f.maxQueue ? ' selected' : ''}>${q}</option>`).join('')}</select></div>
    <label class="cb" title="Hide results whose quality the peer didn't report, or whose size contradicts it"><input type="checkbox" id="fStrict" data-ch="strict"${f.strict ? ' checked' : ''}>Strict</label>
    <div class="fl"><span class="fl-l">Already have</span><span class="seg" role="group" aria-label="Results you already have">${['show', 'dim', 'hide'].map(o => `<button data-act="owned" data-v="${o}" class="${f.owned === o ? 'on' : ''}" aria-pressed="${f.owned === o}">${o[0].toUpperCase() + o.slice(1)}</button>`).join('')}</span></div>
  </div>`;
}
function theadHtml() {
  const col = (k, label, cls = '') => {
    const on = S.sort === k;
    return `<th class="sortable ${cls}" data-act="sort" data-v="${k}" aria-sort="${on ? (S.sortDir > 0 ? 'ascending' : 'descending') : 'none'}">${label}${on ? icon(S.sortDir > 0 ? 'chevu' : 'chevd', 'si') : ''}</th>`;
  };
  return `<tr><th>Status</th><th><span class="sr" hidden>Preview</span></th>${col('title', 'Title')}${col('quality', 'Quality')}${col('length', 'Length', 'r')}${col('size', 'Size', 'r')}${col('peer', 'Peer · today')}${col('avail', 'Availability')}<th><span hidden>Download</span></th></tr>`;
}
function qualHtml(r) {
  const q = qualShort(r);
  const warn = r.suspicious ? `<span title="${esc(r.suspicious)}" aria-label="${esc(r.suspicious)}">${icon('warn', 'warn-ic')}</span>` : '';
  return `<span class="q">${q.unknown ? `<span class="unk" title="The peer didn't report quality; estimated from size and length">${esc(q.text)}</span>` : esc(q.text)}${warn}</span>`;
}
function peerHtml(p) {
  const st = capState(p);
  const tip = `${p.today} downloads from ${p.name} today. Selecta warns at ${LIMITS.peerSoft} and stops at ${LIMITS.peerHard}.`;
  return `<span class="peer"><span class="pn">${esc(p.name)}</span>${p.today ? `<span class="cntb${st === 'ok' ? '' : ' ' + st}" title="${esc(tip)}">${p.today}</span>` : ''}${p.backoff ? `<span title="${esc(`Backing off: ${p.backoff.reason} (${p.backoff.ago})`)}">${icon('ban', 'faint')}</span>` : ''}</span>`;
}
const slotHtml = p => (p.free ? `<span class="slot"><span class="dot"></span>Free · ${fmtSpeed(p.speed)}</span>` : `<span class="slot">${icon('hourglass')}Queue ${p.queue} · ${fmtSpeed(p.speed)}</span>`);
function trackRow(r, alt, inGroup) {
  const own = r._own, sel = S.sel === r.id, playing = !!(P.track && P.track.rid === r.id);
  const dim = S.filters.owned === 'dim' && own && HAVE.has(own.kind);
  return `<tr class="r${alt ? ' alt' : ''}${sel ? ' sel' : ''}${dim ? ' dim' : ''}${playing ? ' playing' : ''}" data-act="sel" data-id="${r.id}" aria-selected="${sel}">
    <td>${badgeHtml(own)}</td>
    <td><button class="rb${playing ? ' on' : ''}" data-act="preview" data-id="${r.id}" aria-label="${playing && P.playing ? 'Pause preview' : 'Preview'}" title="Preview (Space)">${icon(playing && P.playing ? 'pause' : 'play')}</button></td>
    <td class="ti" title="${esc(r.file)}">${inGroup && r.trackNo ? `<span class="faint num">${String(r.trackNo).padStart(2, '0')}</span>&ensp;` : ''}${esc(r.artist)} — ${esc(r.title)}${r.version ? ` <span class="ver">(${esc(r.version)})</span>` : ''}</td>
    <td><span class="fmt">${r.fmt}</span> ${qualHtml(r)}</td>
    <td class="r num">${fmtDur(r.dur)}</td>
    <td class="r num">${fmtSize(r.size)}</td>
    <td>${peerHtml(r.peer)}</td>
    <td>${slotHtml(r.peer)}</td>
    <td><button class="rb" data-act="download" data-id="${r.id}" aria-label="Download" title="Download to the Inbox (Return)">${icon('download')}</button></td>
  </tr>`;
}
function albumLinks(rel) {
  const inInbox = [], inColl = [];
  for (const t of rel.tracks) {
    const a = norm(rel.artist), ti = norm(t[0]);
    const ib = S.inbox.find(it => it.kind === 'single' && norm(it.artist) === a && norm(it.title) === ti && versionRel(it.version, t[1]) === 'same');
    if (ib) inInbox.push({ title: t[0], item: ib });
    const c = COLLECTION.tracks.find(c => !c.album && norm(c.artist) === a && norm(c.title) === ti && versionRel(c.version, t[1]) === 'same');
    if (c) inColl.push({ title: t[0], track: c });
  }
  return { inInbox, inColl, albumInInbox: S.inbox.some(it => it.kind === 'album' && it.rel === rel), albumOwned: COLLECTION.tracks.some(c => c.album === rel.album && norm(c.artist) === norm(rel.artist)) };
}
function groupRow(g) {
  const sel = S.sel === g.id, col = S.collapsed.has(g.key), rel = g.rel;
  const name = g.album ? `${rel.artist} — ${rel.album} (${rel.year})` : g.folder.split('\\').slice(-2).join(' › ');
  let flag = '';
  if (g.album) {
    const L = albumLinks(rel);
    if (L.albumOwned) flag = `<span class="gflag acc">${icon('check-c')}In your collection</span>`;
    else if (L.albumInInbox) flag = `<span class="gflag acc">${icon('tray')}In your Inbox</span>`;
    else if (L.inInbox.length) flag = `<span class="gflag acc" title="Downloading this album moves that single to the Trash">${icon('tray')}“${esc(L.inInbox[0].title)}” single in Inbox</span>`;
    else if (L.inColl.length) flag = `<span class="gflag" title="Flag only: your collection file stays where it is">${icon('flag')}“${esc(L.inColl[0].title)}” in your Singles</span>`;
  }
  return `<tr class="g${sel ? ' sel' : ''}${col ? ' collapsed' : ''}" data-act="sel" data-id="${g.id}" aria-selected="${sel}">
    <td colspan="9"><div class="gwrap">
      <button class="disc" data-act="toggle-group" data-key="${esc(g.key)}" aria-label="${col ? 'Expand folder' : 'Collapse folder'}" aria-expanded="${!col}">${icon('chev')}</button>
      ${icon('folder', 'faint')}
      <span class="gname" title="${esc(g.folder)}">${esc(name)}</span>
      <span class="gmeta">${g.album && g.total > g.items.length ? `${g.items.length} of ${g.total} files` : plural(g.items.length, 'file')} · ${esc(summarizeQuality(g.items))} · ${fmtSize(g.size)}</span>
      ${flag}
      <span class="sp"></span>
      ${peerHtml(g.peer)}
      ${slotHtml(g.peer)}
      ${g.album ? `<button class="btn sm" data-act="dl-folder" data-key="${esc(g.key)}">Download ${rel.single ? 'folder' : 'album'}</button>` : ''}
    </div></td></tr>`;
}
function renderSearchView() {
  const s = curSearch();
  if (!s) {
    $('#content').innerHTML = `<div class="scroll"><div class="pane"><div class="sec-h"><h3>Search Soulseek</h3></div><p class="explain">Type in the search field at the top of the sidebar (⌘F). Results stream in from peers and pass through your filter preset. Searching again for something you searched in the last 24 hours opens the cached results without using your search budget.</p></div></div>`;
    return;
  }
  $('#content').innerHTML = `
    <div class="viewbar${S.moreFilters ? ' more' : ''}" id="filters">${filtersHtml()}</div>
    <div class="summary" id="summary"></div>
    <div class="scroll" id="resScroll"><table class="tbl" id="resTable" style="min-width:840px">
      <colgroup><col style="width:92px"><col style="width:30px"><col><col style="width:122px"><col style="width:60px"><col style="width:70px"><col style="width:142px"><col style="width:132px"><col style="width:34px"></colgroup>
      <thead>${theadHtml()}</thead><tbody id="resBody"></tbody></table></div>`;
  updateResults();
}
function updateResults() {
  const s = curSearch();
  if (!s || S.view !== 'search' || !$('#resBody')) return;
  const v = computeView(s); S._v = v;
  const hidden = v.all.length - v.shown.length;
  const why = Object.entries(v.why).sort((a, b) => b[1] - a[1]).map(([k, n]) => `<span>${esc(REASONS[k])} <span class="n">${fmtInt(n)}</span></span>`).join('');
  const status = s.status === 'running'
    ? `<span class="spinner" aria-hidden="true"></span>&nbsp;Receiving results · ${plural(v.peers, 'peer')} so far`
    : s.cachedAt != null ? `${icon('clock')} Cached ${ago(s.cachedAt)} · <button class="link" data-act="refresh-search">Search again</button> <span class="faint">(uses 1 search)</span>`
    : `${plural(v.peers, 'peer')} answered · <button class="link" data-act="refresh-search">Search again</button>`;
  $('#summary').innerHTML = `<span><b>${fmtInt(v.shown.length)}</b> of ${fmtInt(v.all.length)} match</span>${hidden ? `<span class="why"><span class="faint">Hidden by filters:</span>${why}</span>` : ''}<span class="grow-1"></span><span style="display:inline-flex;align-items:center;gap:4px">${status}</span>`;
  for (const el of $$('#filters [data-fmt-count]')) el.textContent = v.fmtCount[el.dataset.fmtCount] || 0;
  const rows = [], order = [];
  if (v.groups) {
    for (const g of v.groups) {
      rows.push(groupRow(g)); order.push(g.id);
      if (!S.collapsed.has(g.key)) g.items.forEach((r, i) => { rows.push(trackRow(r, i % 2 === 1, true)); order.push(r.id); });
    }
  } else v.shown.forEach((r, i) => { rows.push(trackRow(r, i % 2 === 1, false)); order.push(r.id); });
  S.rows = order;
  $('#resBody').innerHTML = rows.join('') || `<tr><td colspan="9" style="height:120px;text-align:center;color:var(--label-2);white-space:normal">${s.status === 'running' ? 'Waiting for peers to answer…' : `No results match these filters. ${fmtInt(hidden)} hidden: loosen a filter above or pick another preset.`}</td></tr>`;
  const sub = $('#tbSub'); if (sub) sub.textContent = VIEWS.search.title()[1];
  if (S.sel && !order.includes(S.sel) && !v.byId.has(S.sel) && !S.results.has(S.sel)) S.sel = null;
}

/* ---------- inspector (search) ---------- */
function peerSection(p) {
  const st = capState(p);
  return `<section class="insp-sec"><h4>Peer</h4><dl class="kv">
    <dt>User</dt><dd>${esc(p.name)}</dd>
    <dt>Upload slot</dt><dd>${p.free ? 'Free now' : `Busy · ${p.queue} waiting`}</dd>
    <dt>Speed</dt><dd class="num">${fmtSpeed(p.speed)}</dd>
    <dt>Shares</dt><dd class="num">${fmtInt(p.files)} files</dd>
    <dt>From them</dt><dd class="num">${p.today} today · ${p.session} this session · ${p.all} all time</dd>
    <dt>Daily limit</dt><dd><div style="display:flex;align-items:center;gap:8px"><div class="meter${st === 'cap' ? ' bad' : st === 'near' ? ' warn' : ''}" style="flex:1"><i style="width:${Math.min(100, p.today / LIMITS.peerHard * 100)}%"></i></div><span class="num muted">${p.today} / ${LIMITS.peerHard}</span></div>${st !== 'ok' ? `<div class="muted" style="margin-top:3px">${st === 'cap' ? 'Limit reached for today' : 'Getting close to the limit'}</div>` : ''}</dd>
  </dl>${p.backoff ? `<div class="note warn" style="margin-top:8px">${icon('ban')}<div>Backing off: they answered <b>“${esc(p.backoff.reason)}”</b> ${esc(p.backoff.ago)}. Selecta waits before asking again.</div></div>` : ''}</section>`;
}
function ownNote(r, o) {
  if (!o) return `<div class="note">${icon('info')}<div>Not in your collection, Inbox or download history.</div></div>`;
  const d = esc(ownDetail(o));
  switch (o.kind) {
    case 'owned': return `<div class="note good">${icon('check-c')}<div><b>In your collection</b> as <span class="mono">${d}</span>. Downloading again asks first.</div></div>`;
    case 'upgrade': return `<div class="note acc">${icon('up-c')}<div><b>Upgrade:</b> you own <span class="mono">${d}</span>. Filing adds the better copy; the old file stays where rekordbox expects it.</div></div>`;
    case 'likely': return `<div class="note warn">${icon('approx-c')}<div><b>Probably in your collection:</b> <span class="mono">${d}</span>. The version isn't clear from the file names.</div></div>`;
    case 'version': return `<div class="note">${icon('approx-c')}<div>You own another mix: <span class="mono">${d}</span>.</div></div>`;
    case 'inbox': return `<div class="note acc">${icon('tray')}<div><b>In your Inbox</b>, not filed yet (session ${o.item.session}).</div></div>`;
    case 'queued': return `<div class="note acc">${icon('hourglass')}<div><b>In your transfers:</b> ${d}.</div></div>`;
    default: return `<div class="note">${icon('clock')}<div>Downloaded in session ${o.rec.session} from ${esc(o.rec.peer)} · now ${esc(o.rec.where)}.</div></div>`;
  }
}
function plausibility(r) {
  if (r.suspicious) return `<span class="bdg warn" style="white-space:normal">${icon('warn')}${esc(r.suspicious)}</span>`;
  if ((LOSSY.has(r.fmt) && r.br == null) || (!LOSSY.has(r.fmt) && r.sr == null)) return `<span class="bdg mute" style="white-space:normal">${icon('info')}Peer didn't report quality</span>`;
  return `<span class="bdg good" style="white-space:normal">${icon('check-c')}Size matches the declared quality</span>`;
}
function inspectorTrack(r) {
  const o = r._own !== undefined ? r._own : ownership(r);
  return `<div class="insp">
    <div><div class="insp-title">${esc(r.title)}${r.version ? ` <span class="muted">(${esc(r.version)})</span>` : ''}</div><div class="insp-sub">${esc(r.artist)} · ${esc(r.rel.album)} (${r.rel.year})</div></div>
    <div class="insp-actions"><button class="btn" data-act="preview" data-id="${r.id}">${icon('play')} Preview</button><button class="btn primary" data-act="download" data-id="${r.id}">${icon('download')} Download</button></div>
    ${ownNote(r, o)}
    <section class="insp-sec"><h4>Quality</h4><dl class="kv">
      <dt>Format</dt><dd>${r.fmt}${LOSSY.has(r.fmt) && r.br != null ? (r.vbr ? ' · VBR' : ' · CBR') : ''}</dd>
      ${LOSSY.has(r.fmt) ? `<dt>Bitrate</dt><dd>${r.br != null ? `${r.br} kbps` : '<span class="faint">Not reported</span>'}</dd>` : `<dt>Resolution</dt><dd>${r.sr != null ? `${r.bd}-bit · ${khz(r.sr)} kHz` : '<span class="faint">Not reported</span>'}</dd>`}
      <dt>From size</dt><dd class="num">≈ ${fmtInt(estKbps(r))} kbps</dd>
      <dt>Length</dt><dd class="num">${fmtDur(r.dur)}</dd>
      <dt>Size</dt><dd class="num">${fmtSize(r.size)}</dd>
      <dt>Check</dt><dd>${plausibility(r)}</dd>
    </dl></section>
    <section class="insp-sec"><h4>File</h4><dl class="kv"><dt>Name</dt><dd class="mono">${esc(r.file)}</dd><dt>Folder</dt><dd class="mono">${esc(r.folder)}</dd><dt>Read as</dt><dd>${esc(r.artist)} · ${esc(r.title)}${r.version ? ` · ${esc(r.version)}` : ''}${r.trackNo ? ` · track ${r.trackNo}` : ''}</dd></dl></section>
    ${peerSection(r.peer)}
    <div class="insp-actions"><button class="btn" data-act="browse-peer" data-v="${esc(r.peer.name)}">${icon('folder')} Browse their files</button><button class="btn" data-act="add-wishlist" data-v="${esc(`${r.artist} ${r.title}`)}">${icon('star')} Add to wishlist</button></div>
  </div>`;
}
function inspectorGroup(g) {
  const rel = g.rel, p = g.peer, L = g.album ? albumLinks(rel) : null;
  const notes = [];
  if (L && L.inInbox.length) notes.push(`<div class="note acc">${icon('tray')}<div><b>“${esc(L.inInbox[0].title)}” is in your Inbox as a single.</b> When this album finishes, Selecta moves that single to the Trash. You can undo.</div></div>`);
  if (L && L.inColl.length) notes.push(`<div class="note warn">${icon('flag')}<div><b>“${esc(L.inColl[0].title)}” is already in your Singles.</b> Selecta flags the pair and never touches your collection file.</div></div>`);
  if (g.album) notes.push(`<div class="note">${icon('info')}<div>An album download goes to <span class="mono">Inbox/Albums/${esc(albumDir(rel))}/</span> with every file in the folder.</div></div>`);
  return `<div class="insp">
    <div style="display:flex;gap:12px;align-items:center"><span class="art" style="width:56px;height:56px;background:${artGradient(hashStr(rel.artist + rel.album))}"></span><div style="min-width:0"><div class="insp-title">${esc(g.album ? rel.album : 'Loose files')}</div><div class="insp-sub">${esc(rel.artist)}${g.album ? ` · ${rel.year} · ${esc(rel.genre)}` : ''}</div></div></div>
    <div class="insp-actions">${g.album ? `<button class="btn primary" data-act="dl-folder" data-key="${esc(g.key)}">${icon('download')} Download ${rel.single ? 'folder' : 'album'}</button>` : ''}<button class="btn" data-act="browse-peer" data-v="${esc(p.name)}">${icon('folder')} Browse folder</button></div>
    <div class="notes">${notes.join('')}</div>
    <section class="insp-sec"><h4>Folder</h4><dl class="kv"><dt>Path</dt><dd class="mono">${esc(g.folder)}</dd><dt>Files</dt><dd>${g.album ? `${g.total} audio files + cover.jpg${g.items.length < g.total ? ` (${g.items.length} match your search)` : ''}` : plural(g.items.length, 'file')}</dd><dt>Quality</dt><dd>${esc(summarizeQuality(g.items))}</dd><dt>Size</dt><dd class="num">${fmtSize(g.album ? (g.size / g.items.length) * g.total : g.size)}</dd></dl></section>
    ${peerSection(p)}
  </div>`;
}
function renderInspector() {
  const el = $('#inspector'), v = VIEWS[S.view];
  if (!v.insp) { el.innerHTML = ''; return; }
  if (S.view === 'batch') { el.innerHTML = inspectorBatch(); return; }
  if (!S.sel) { el.innerHTML = '<div class="insp-empty">Select a result to see its quality check, what you already have, and how much you took from this peer.</div>'; return; }
  const g = S._v && S._v.byId.get(S.sel);
  if (g) { el.innerHTML = inspectorGroup(g); return; }
  const r = S.results.get(S.sel);
  el.innerHTML = r ? inspectorTrack(r) : '';
}

/* ---------- downloads ---------- */
const BASE_MIN = 20 * 60 + 12;
const clockTime = () => { const m = BASE_MIN + Math.floor(Clock.t / 60000); return `${String(Math.floor(m / 60) % 24).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`; };
function requestDownload(r, o = {}) {
  if (S.netPaused) { toast('Network activity is paused', 'Resume it from the toolbar to download.', { kind: 'warn' }); return; }
  const own = ownership(r);
  if (!o.force && own && HAVE.has(own.kind)) {
    sheet({ title: `You already have “${r.title}”`, body: `<p>${esc(BADGES[own.kind].long)}: <span class="mono">${esc(ownDetail(own))}</span></p><p>This result is ${esc(qualLong(r))} from ${esc(r.peer.name)}.</p>`, actions: [{ label: 'Cancel' }, { label: 'Download anyway', primary: true, run: () => requestDownload(r, { ...o, force: true }) }] });
    return;
  }
  const p = r.peer;
  if (p.backoff && !o.forcePeer) { toast(`Holding off on ${p.name}`, `They answered “${esc(p.backoff.reason)}” ${esc(p.backoff.ago)}. Pick another peer for now.`, { kind: 'warn', icon: 'ban' }); return; }
  if (p.today >= LIMITS.peerHard && !o.forcePeer) { capSheet(r); return; }
  addDownload(r, o);
  if (p.today + 1 >= LIMITS.peerSoft && !o.quiet) toast(`${p.name}: ${p.today + 1} files today`, `Selecta warns at ${LIMITS.peerSoft} and stops at ${LIMITS.peerHard} per peer per day. Try to spread your downloads.`, { kind: 'warn', icon: 'peers' });
}
function alternativesFor(r) {
  const s = curSearch(); if (!s) return [];
  return s.results.slice(0, s.revealed).filter(x => x !== r && x.rel === r.rel && x.ti === r.ti && x.peer.today < LIMITS.peerSoft && !hiddenReason(x, S.filters, null)).sort((a, b) => score(b) - score(a)).slice(0, 3);
}
function capSheet(r) {
  const alts = alternativesFor(r);
  sheet({
    title: `Daily limit reached for ${r.peer.name}`,
    body: `<p>You took ${r.peer.today} files from ${esc(r.peer.name)} today. Selecta stops at ${LIMITS.peerHard} per peer per day so no single person carries your digging.</p>${alts.length ? `<p>The same track from other peers:</p><div class="group">${alts.map(a => `<div class="grow"><div class="gl">${esc(a.peer.name)}<div class="gd">${esc(qualLong(a))} · ${fmtSize(a.size)} · ${a.peer.free ? 'free slot' : `queue ${a.peer.queue}`}</div></div><button class="btn sm" data-act="dl-alt" data-id="${a.id}">Download</button></div>`).join('')}</div>` : '<p>No other peer in these results has it within your filters.</p>'}`,
    actions: [{ label: 'Close' }, { label: 'Download once anyway', run: () => requestDownload(r, { forcePeer: true, force: true }) }],
  });
}
function addDownload(r, o = {}) {
  const t = { id: 't' + S.nextId++, kind: o.preview ? 'preview' : o.album ? 'album' : 'single', rid: r.id, r, peer: r.peer, name: `${r.artist} — ${fullTitle(r.title, r.version)}`, qual: qualLong(r), size: r.size, progress: 0, speed: 0, state: 'waiting', queuePos: r.peer.free ? 0 : r.peer.queue + 1, since: Clock.t, album: o.album || null, dest: o.preview ? 'Preview cache' : o.album ? `Inbox/Albums/${albumDir(r.rel)}/` : 'Inbox/Singles/' };
  S.transfers.unshift(t);
  if (!o.album && !o.quiet) toast('Added to downloads', `${esc(t.name)} · ${esc(t.qual)} from ${esc(r.peer.name)} → ${esc(t.dest)}`, { icon: 'download' });
  refreshLive(true);
  return t;
}
function confirmAlbum(key) {
  const g = S._v && S._v.groups && S._v.groups.find(x => x.key === key);
  if (!g) return;
  const rel = g.rel, p = g.peer, L = albumLinks(rel), n = rel.tracks.length;
  const notes = [];
  if (L.inInbox.length) notes.push(`<div class="note acc">${icon('tray')}<div><b>“${esc(L.inInbox[0].title)}” is in your Inbox as a single.</b> When the album finishes, Selecta moves that single to the Trash. You can undo.</div></div>`);
  if (L.inColl.length) notes.push(`<div class="note warn">${icon('flag')}<div><b>“${esc(L.inColl[0].title)}” is already in your Singles.</b> Selecta flags the pair and leaves your collection file where rekordbox expects it.</div></div>`);
  if (p.today + n >= LIMITS.peerSoft) notes.push(`<div class="note ${p.today + n >= LIMITS.peerHard ? 'bad' : 'warn'}">${icon('peers')}<div>This takes you to <b>${p.today + n} files from ${esc(p.name)} today</b> (warning at ${LIMITS.peerSoft}, stop at ${LIMITS.peerHard}).</div></div>`);
  sheet({
    title: `Download “${rel.album}” as an album?`,
    body: `<p>${n} files · ${esc(summarizeQuality(g.items))} · about ${fmtSize((g.size / g.items.length) * n)} from ${esc(p.name)} (${p.free ? 'free slot' : `queue ${p.queue}`}).</p><p>Goes to <span class="mono">Inbox/Albums/${esc(albumDir(rel))}/</span> with the folder's cover image.</p><div class="notes">${notes.join('')}</div>`,
    actions: [{ label: 'Cancel' }, { label: `Download ${n} files`, primary: true, run: () => downloadAlbum(g) }],
  });
}
function downloadAlbum(g) {
  const rel = g.rel, base = g.items[0], albumId = 'al' + S.nextId++;
  const ts = rel.tracks.map((t, i) => {
    let r = g.items.find(x => x.ti === i);
    if (!r) { r = { ...base, id: `r${S.nextId++}`, ti: i, trackNo: i + 1, title: t[0], version: t[1], dur: t[2], size: (base.size / base.dur) * t[2], file: `${nn2(i)} - ${rel.artist} - ${fullTitle(t[0], t[1])}.${EXT[base.fmt]}` }; S.results.set(r.id, r); }
    return addDownload(r, { album: { id: albumId }, quiet: true });
  });
  S.albums.set(albumId, { id: albumId, rel, peer: g.peer, ts, done: 0, base });
  toast('Album added to downloads', `${esc(rel.artist)} — ${esc(rel.album)} · ${ts.length} files from ${esc(g.peer.name)} → Inbox/Albums/`, { icon: 'download' });
}
const SIM_SPEEDUP = 5; // the mock moves files faster than the displayed peer speed
function updateTransfers(dt) {
  const busy = new Map();
  for (const t of S.transfers) if (t.state === 'active' || t.state === 'connecting') busy.set(t.peer.name, (busy.get(t.peer.name) || 0) + 1);
  let changed = false;
  for (const t of S.transfers) {
    if (S.netPaused && t.state !== 'active') continue;
    if (t.state === 'waiting') {
      if (t.queuePos > 0) { t.state = 'queued'; t.since = Clock.t; changed = true; continue; }
      const n = busy.get(t.peer.name) || 0;
      if (n < LIMITS.peerConcurrent) { t.state = 'connecting'; t.since = Clock.t; busy.set(t.peer.name, n + 1); changed = true; }
    } else if (t.state === 'queued') {
      if (Clock.t - t.since > (t.qStep || 1300)) { t.queuePos--; t.since = Clock.t; changed = true; if (t.queuePos <= 0) { t.queuePos = 0; t.state = 'waiting'; } }
    } else if (t.state === 'connecting') {
      if (Clock.t - t.since > 650) { t.state = 'active'; t.since = Clock.t; changed = true; }
    } else if (t.state === 'active') {
      t.speed = t.peer.speed * (0.78 + Math.random() * 0.3);
      t.progress = Math.min(1, t.progress + (t.speed * (t.kind === 'preview' ? 1.6 : t.slow ? 0.15 : SIM_SPEEDUP) * dt / 1000) / t.size);
      changed = true;
      if (t.progress >= 1) finishTransfer(t);
    }
  }
  for (const u of S.uploads) {
    if (u.state !== 'active' || S.netPaused) continue;
    u.progress += (u.speed * dt / 1000) / u.size;
    if (u.progress >= 1) {
      for (const w of ['session', '30d', 'all']) { RATIO[w].up.files++; RATIO[w].up.bytes += u.size; }
      const next = UPLOAD_QUEUE.shift();
      if (next) Object.assign(u, next, { progress: 0 }); else u.state = 'done';
    }
    changed = true;
  }
  return changed;
}
function countDownload(t) {
  const p = t.peer; p.today++; p.session++; p.d30++; p.all++; p.last = clockTime();
  for (const w of ['session', '30d', 'all']) { RATIO[w].down.files++; RATIO[w].down.bytes += t.size; }
}
function finishTransfer(t) {
  t.state = 'done'; t.doneAt = Clock.t;
  countDownload(t);
  if (t.kind === 'preview') { if (t.keep) { inboxFromResult(t.r, t.peer); toast('Kept preview is in your Inbox', esc(t.name), { kind: 'good', icon: 'tray' }); } return; }
  if (t.album) { const al = S.albums.get(t.album.id); al.done++; if (al.done === al.ts.length) finishAlbum(al); return; }
  inboxFromResult(t.r, t.peer);
  toast('Downloaded to your Inbox', `${esc(t.name)} · ${esc(t.qual)}`, { kind: 'good', icon: 'tray' });
}
function inboxFromResult(r, p) {
  S.inbox.unshift({ id: 'ib' + S.nextId++, kind: 'single', artist: r.artist, title: r.title, version: r.version, fmt: r.fmt, br: r.br, vbr: r.vbr, sr: r.sr, bd: r.bd, dur: r.dur, size: r.size, peer: p.name, session: SESSIONS[0].n, rel: r.rel });
  S.history.unshift({ session: SESSIONS[0].n, artist: r.artist, title: r.title, version: r.version, q: qualLong(r), peer: p.name, where: 'Inbox', time: clockTime() });
}
function finishAlbum(al) {
  const rel = al.rel, base = al.base, L = albumLinks(rel);
  S.inbox.unshift({ id: 'ib' + S.nextId++, kind: 'album', artist: rel.artist, album: rel.album, year: rel.year, fmt: base.fmt, br: base.br, vbr: base.vbr, sr: base.sr, bd: base.bd, tracks: rel.tracks.length, size: al.ts.reduce((a, t) => a + t.size, 0), peer: al.peer.name, session: SESSIONS[0].n, rel });
  S.history.unshift({ session: SESSIONS[0].n, artist: rel.artist, title: rel.album, version: '', album: true, n: rel.tracks.length, q: qualLong(base), peer: al.peer.name, where: 'Inbox', time: clockTime() });
  const trashed = L.inInbox.map(x => x.item);
  const histOf = it => S.history.find(h => !h.album && norm(h.artist) === norm(it.artist) && norm(h.title) === norm(it.title));
  if (!trashed.length) { toast(`“${rel.album}” finished`, `${rel.tracks.length} files in <span class="mono">Inbox/Albums/${esc(albumDir(rel))}/</span>`, { kind: 'good', icon: 'tray' }); return; }
  S.inbox = S.inbox.filter(it => !trashed.includes(it));
  trashed.forEach(it => { const h = histOf(it); if (h) h.where = 'Trash (on album)'; });
  toast(`“${rel.album}” finished`, `${trashed.map(x => `“${esc(x.title)}”`).join(', ')} was in your Inbox as a single, so it went to the Trash.`, {
    kind: 'good', icon: 'trash', ms: 14000,
    actions: [{ label: 'Undo', run: () => { S.inbox.push(...trashed); trashed.forEach(it => { const h = histOf(it); if (h) h.where = 'Inbox'; }); toast('Back in your Inbox', 'The single was restored from the Trash.', { kind: 'good', icon: 'undo' }); refreshLive(true); } }],
  });
}

/* ---------- Inbox & filing ---------- */
const TARGETS = {
  'mp3-320': { label: 'MP3 320 CBR', fmt: 'MP3', br: 320 },
  'mp3-v0': { label: 'MP3 V0', fmt: 'MP3', br: 245, vbr: true },
  aiff: { label: 'AIFF 16/44.1', fmt: 'AIFF', sr: 44100, bd: 16 },
  wav: { label: 'WAV 16/44.1', fmt: 'WAV', sr: 44100, bd: 16 },
  flac: { label: 'FLAC', fmt: 'FLAC', lossless: true },
  asis: { label: 'Keep as downloaded', asis: true },
};
function filingPlan(it) {
  const tg = TARGETS[S.filingTarget];
  if (tg.asis) return { kind: 'move', text: 'Moves as downloaded' };
  if (!LOSSY.has(it.fmt)) {
    if (LOSSY.has(tg.fmt)) return { kind: 'convert', text: `Converts to ${tg.label}, tags and artwork carried over` };
    return it.fmt === tg.fmt ? { kind: 'move', text: 'Already in this format: moves as-is' } : { kind: 'convert', text: `Converts to ${tg.label} (lossless to lossless)` };
  }
  if (it.fmt === tg.fmt && (it.br || 0) >= (tg.br || 0)) return { kind: 'move', text: `Already ${qualLong(it)}: moves as-is, tags completed` };
  return { kind: 'below', text: `${qualLong(it)} is below ${tg.label}. Files as-is: re-encoding can't add quality` };
}
function inboxFlags(it) {
  if (it.kind === 'album') {
    const L = albumLinks(it.rel);
    return L.inColl.length ? ` · <span class="gflag" title="Flag only: your collection file stays where it is">${icon('flag')}“${esc(L.inColl[0].title)}” is also in your Singles</span>` : '';
  }
  const lower = COLLECTION.tracks.find(c => norm(c.artist) === norm(it.artist) && norm(c.title) === norm(it.title) && versionRel(c.version, it.version) === 'same' && qrank(c) < qrank(it));
  return lower ? ` · <span class="gflag acc" title="The old file stays where rekordbox expects it">${icon('up-c')}Upgrades your ${esc(qualLong(lower))} copy</span>` : '';
}
function inboxRow(it, alt) {
  const plan = filingPlan(it), f = S.filing.get(it.id);
  const title = it.kind === 'album' ? `${it.artist} — ${it.album} (${it.year})` : `${it.artist} — ${fullTitle(it.title, it.version)}`;
  const sub = it.kind === 'album' ? `${it.tracks} tracks · ${fmtSize(it.size)}` : `${fmtDur(it.dur)} · ${fmtSize(it.size)}`;
  const pk = plan.kind === 'below' ? ['warn', 'warn'] : plan.kind === 'convert' ? ['acc', 'convert'] : ['good', 'check-c'];
  return `<tr class="${alt ? 'alt' : ''}">
    <td><input type="checkbox" data-ch="inbox-sel" data-id="${it.id}"${S.inboxSel.has(it.id) ? ' checked' : ''} aria-label="Select ${esc(title)}"></td>
    <td style="height:44px"><div class="cell2"><span class="thumb" style="background:${artGradient(hashStr(it.artist + (it.album || it.title)))}"></span><div class="t"><div style="overflow:hidden;text-overflow:ellipsis">${esc(title)}</div><div class="faint" style="font-size:11px;overflow:hidden;text-overflow:ellipsis">${sub}${inboxFlags(it)}</div></div></div></td>
    <td>${esc(qualLong(it))}</td>
    <td style="white-space:normal">${f ? `<div class="prog"><div class="bar"><i style="width:${(f.p * 100).toFixed(1)}%"></i></div><span class="muted num" style="font-size:11px">${esc(f.step)}</span></div>` : `<span class="bdg ${pk[0]}" style="white-space:normal;align-items:flex-start">${icon(pk[1])}<span>${esc(plan.text)}</span></span>`}</td>
    <td><span class="muted">${esc(it.peer)}</span> <span class="faint">· session ${it.session}</span></td>
    <td class="r"><button class="btn sm" data-act="inbox-file" data-id="${it.id}"${f ? ' disabled' : ''}>File</button></td>
  </tr>`;
}
function renderInbox() {
  const albums = S.inbox.filter(i => i.kind === 'album'), singles = S.inbox.filter(i => i.kind === 'single');
  const opts = Object.entries(TARGETS).map(([k, t]) => `<option value="${k}"${k === S.filingTarget ? ' selected' : ''}>${t.label}${t.asis ? '' : ' · ID3v2.3 · artwork'}</option>`).join('');
  const section = (name, items) => items.length ? `<tr class="g"><td colspan="6"><div class="gwrap"><span class="gname">${name}</span><span class="gmeta">${items.length}</span></div></td></tr>${items.map((it, i) => inboxRow(it, i % 2 === 1)).join('')}` : '';
  $('#content').innerHTML = `
    <div class="viewbar"><div class="vb-row">
      <label class="fl"><span class="fl-l">File as</span><select id="fileAs" data-ch="filingTarget">${opts}</select></label>
      <span class="grow-1"></span>
      <button class="btn" data-act="inbox-trash"${S.inboxSel.size ? '' : ' disabled'}>${icon('trash')} Move to Trash</button>
      <button class="btn primary" data-act="inbox-file-sel"${S.inboxSel.size ? '' : ' disabled'}>File ${S.inboxSel.size ? S.inboxSel.size + ' ' : ''}selected</button>
    </div></div>
    <div class="scroll"><table class="tbl" style="min-width:860px"><colgroup><col style="width:36px"><col><col style="width:116px"><col style="width:280px"><col style="width:180px"><col style="width:64px"></colgroup>
      <thead><tr><th><input type="checkbox" data-ch="inbox-all" aria-label="Select everything"${S.inbox.length && S.inboxSel.size === S.inbox.length ? ' checked' : ''}></th><th>Item</th><th>Downloaded as</th><th>When you file it</th><th>From</th><th></th></tr></thead>
      <tbody>${section('Albums', albums)}${section('Singles', singles)}${S.inbox.length ? '' : '<tr><td colspan="6" style="height:120px;text-align:center;color:var(--label-2)">Your Inbox is empty. Downloads land here until you file them.</td></tr>'}</tbody></table></div>
    <div class="footbar">${icon('info')}<span>Singles are filed to <span class="mono">${COLLECTION.root}/${COLLECTION.singlesDir}/</span>, albums to <span class="mono">${COLLECTION.root}/${COLLECTION.albumsDir}/Artist - Album (Year)/</span>. Filing copies the file, checks the copy, then clears it from the Inbox; lossless originals go to the Trash after a checked conversion. Selecta never changes files already in your collection.</span></div>`;
}
function fileItems(ids) {
  for (const id of ids) {
    const it = S.inbox.find(x => x.id === id);
    if (!it || S.filing.has(id)) continue;
    const plan = filingPlan(it);
    S.filing.set(id, { p: 0, n: it.kind === 'album' ? it.tracks : 1, conv: plan.kind === 'convert', step: 'Starting', it, plan, target: S.filingTarget });
  }
  S.inboxSel.clear();
  renderContent();
}
function updateFiling(dt) {
  let changed = false;
  for (const [id, f] of S.filing) {
    f.p = Math.min(1, f.p + dt / ((f.conv ? 1100 : 450) * f.n));
    const k = Math.min(f.n, Math.floor(f.p * f.n) + 1);
    f.step = f.p < 0.94 ? `${f.conv ? 'Converting' : 'Copying'} ${k} of ${f.n}` : 'Checking the copy';
    changed = true;
    if (f.p >= 1) { S.filing.delete(id); completeFiling(f); }
  }
  return changed;
}
function completeFiling(f) {
  const it = f.it, tg = TARGETS[f.target];
  S.inbox = S.inbox.filter(x => x !== it);
  const out = f.plan.kind === 'convert' ? tg : it;
  const q = f.plan.kind === 'convert' ? `${qualLong(it)} → ${tg.label}` : qualLong(it);
  if (it.kind === 'album') {
    const dir = `${COLLECTION.albumsDir}/${albumDir(it.rel)}`;
    it.rel.tracks.forEach((t, i) => COLLECTION.tracks.push({ artist: it.artist, title: t[0], version: t[1], fmt: out.fmt, br: out.br, vbr: out.vbr, sr: out.sr, bd: out.bd, album: it.album, path: `${dir}/${nn2(i)} - ${t[0]}.${EXT[out.fmt]}` }));
    COLLECTION.stats.tracks += it.tracks; COLLECTION.stats.albums++;
    COLLECTION.recent.unshift({ name: `${it.artist} — ${it.album} (${it.tracks} tracks)`, path: `${dir}/`, q, when: 'Just now' });
    const h = S.history.find(x => x.album && x.title === it.album); if (h) h.where = `Filed · ${dir}/`;
    toast('Filed to your collection', `${esc(it.artist)} — ${esc(it.album)} → <span class="mono">${esc(dir)}/</span> · ${esc(q)}`, { kind: 'good', icon: 'check-c' });
  } else {
    const path = `${COLLECTION.singlesDir}/${it.artist} - ${fullTitle(it.title, it.version)}.${EXT[out.fmt]}`;
    COLLECTION.tracks.push({ artist: it.artist, title: it.title, version: it.version, fmt: out.fmt, br: out.br, vbr: out.vbr, sr: out.sr, bd: out.bd, path });
    COLLECTION.stats.tracks++; COLLECTION.stats.singles++;
    COLLECTION.recent.unshift({ name: `${it.artist} — ${fullTitle(it.title, it.version)}`, path, q, when: 'Just now' });
    const h = S.history.find(x => !x.album && norm(x.artist) === norm(it.artist) && norm(x.title) === norm(it.title)); if (h) h.where = `Filed · ${COLLECTION.singlesDir}/`;
    toast('Filed to your collection', `<span class="mono">${esc(path)}</span> · ${esc(q)}`, { kind: 'good', icon: 'check-c' });
  }
}

/* ---------- Batch search ---------- */
const SAMPLE_TRACKLIST = `Kassia Moreau - Low Orbit
Okuma Drift - Cold Harbour
Halden & Rue - Mirage Engine
Marlo Vance - Tidal Memory (Ines Varga Remix)
Ines Varga - Vesper
Teodor Lumen - Amber Static
Juno Ashgrove - Velvet Machine (Extended Mix)
Sefa Kintu - Lagos Night Bus
Pale Monarch - Updraft
Ravel Ortiz - Room 404 (Dub)
Kassia Moreau - Paper Lanterns
Okuma Drift - Kinetic (Club Edit)
Mirela Stone - Undersea Cathedral
Halden & Rue - Brine`;
function parseLine(raw) {
  const m = raw.match(/^(.*?)\s+[-–—]\s+(.*)$/);
  let artist = m ? m[1] : '', title = m ? m[2] : raw, version = '';
  const v = title.match(/^(.*?)\s*\(([^)]+)\)\s*$/);
  if (v) { title = v[1]; version = v[2]; }
  return { artist: artist.trim(), title: title.trim(), version: version.trim() };
}
function batchParse() {
  const B = S.batch, seen = new Set();
  B.lines = B.text.split('\n').map(x => x.trim()).filter(Boolean).filter(x => { const k = norm(x); if (seen.has(k)) return false; seen.add(k); return true; }).map((raw, i) => {
    const p = parseLine(raw);
    const fake = { artist: p.artist, title: p.title, version: p.version, fmt: 'MP3', br: 320 };
    const own = ownership(fake);
    return { id: 'b' + i, n: i + 1, raw, ...p, status: B.skipOwned && own && HAVE.has(own.kind) ? 'owned' : 'pending', own, best: null, alts: [], scoreV: 0, approved: false };
  });
  B.sel = null; B.done = false;
}
function similarity(a, b) {
  const A = new Set(norm(a).split(' ').filter(Boolean)), B2 = new Set(norm(b).split(' ').filter(Boolean));
  if (!A.size && !B2.size) return 1;
  if (!A.size || !B2.size) return 0;
  let n = 0; A.forEach(x => { if (B2.has(x)) n++; });
  return (2 * n) / (A.size + B2.size);
}
function batchResolve(line) {
  const f = presetFilters(S.batch.preset);
  const res = generateResults(line.raw.replace(/[()]/g, ' '), { pseudo: false }).filter(r => !hiddenReason(r, f, null));
  const scored = res.map(r => ({ r, m: 0.45 * similarity(line.artist, r.artist) + 0.45 * similarity(`${line.title} ${line.version}`, `${r.title} ${r.version}`) + 0.1 * Math.min(1, score(r) / 1.6) })).sort((a, b) => b.m - a.m);
  const seenPeers = new Set(), picks = [];
  for (const x of scored) { if (seenPeers.has(x.r.peer.name)) continue; seenPeers.add(x.r.peer.name); picks.push(x); if (picks.length >= 4) break; }
  if (!picks.length) { line.status = 'notfound'; return; }
  line.best = picks[0].r; line.alts = picks.slice(1).map(x => x.r); line.scoreV = picks[0].m; line.candidates = res.length;
  line.status = picks[0].m >= 0.85 ? 'match' : 'review';
}
function updateBatch() {
  const B = S.batch;
  if (!B || !B.running) return false;
  const searching = B.lines.find(l => l.status === 'searching');
  if (searching) {
    if (Clock.t - searching.started > 1800) { batchResolve(searching); return true; }
    return false;
  }
  const next = B.lines.find(l => l.status === 'pending');
  if (!next) { B.running = false; B.done = true; Clock.speed = 1; toast('Batch search finished', `${B.lines.filter(l => l.status === 'match').length} confident matches, ${B.lines.filter(l => l.status === 'review').length} to review, ${B.lines.filter(l => l.status === 'notfound').length} not found.`, { kind: 'good', icon: 'batch' }); return true; }
  const cached = S.cache.get(norm(next.raw));
  if (cached) { next.status = 'searching'; next.started = Clock.t - 1700; next.cached = true; return true; }
  if (S.netPaused) return false;
  if (Clock.t < B.nextAt || budgetLeft() <= LIMITS.batchKeepFree) { next.waiting = true; return false; }
  S.budget.push(Clock.t);
  next.status = 'searching'; next.started = Clock.t; next.waiting = false;
  B.nextAt = Clock.t + (LIMITS.windowSec * 1000) / (LIMITS.searches - LIMITS.batchKeepFree);
  return true;
}
const BSTAT = {
  pending: ['clock', 'mute', 'Waiting'], searching: ['search', 'acc', 'Searching'], match: ['check-c', 'good', 'Match'], review: ['approx-c', 'warn', 'Review'],
  notfound: ['x-c', 'bad', 'Not found'], owned: ['check-c', 'mute', 'You have it'],
};
function batchRows() {
  const B = S.batch;
  return B.lines.map((l, i) => {
    const st = BSTAT[l.status]; const sel = B.sel === l.id;
    const label = l.status === 'pending' ? (S.batch.running ? (l.waiting ? 'Paced' : 'Queued') : 'Ready') : st[2];
    const best = l.best ? `<div class="cell2"><div class="t">${esc(l.best.artist)} — ${esc(fullTitle(l.best.title, l.best.version))}<div class="faint" style="font-size:11px">${esc(qualLong(l.best))} · ${esc(l.best.peer.name)} · ${l.best.peer.free ? 'free slot' : `queue ${l.best.peer.queue}`}${l.alts.length ? ` · ${plural(l.alts.length, 'alternative')}` : ''}</div></div></div>` : l.status === 'owned' ? `<span class="faint" title="${esc(ownDetail(l.own))}">${esc(BADGES[l.own.kind].long)}</span>` : l.status === 'notfound' ? '<span class="faint">Nothing within your preset. Kept for the wishlist.</span>' : '';
    const playing = l.best && P.track && P.track.rid === l.best.id;
    return `<tr class="r${i % 2 ? ' alt' : ''}${sel ? ' sel' : ''}" data-act="batch-sel" data-id="${l.id}" style="height:42px">
      <td class="num faint">${l.n}</td>
      <td title="${esc(l.raw)}">${esc(l.raw)}</td>
      <td><span class="bdg ${st[1]}">${l.status === 'searching' ? '<span class="spinner"></span>' : icon(st[0])}${esc(label)}${l.cached ? ' · cached' : ''}</span></td>
      <td style="white-space:normal">${best}</td>
      <td class="r num">${l.best ? `${Math.round(l.scoreV * 100)}%` : ''}</td>
      <td class="r">${l.best ? `<button class="rb${playing ? ' on' : ''}" data-act="preview" data-id="${l.best.id}" aria-label="Preview the best match">${icon(playing && P.playing ? 'pause' : 'play')}</button> <label class="cb" title="Approve this match"><input type="checkbox" data-ch="batch-approve" data-id="${l.id}"${l.approved ? ' checked' : ''}${l.dl ? ' disabled' : ''}> ${l.dl ? 'Queued' : 'OK'}</label>` : ''}</td>
    </tr>`;
  }).join('');
}
function renderBatch() {
  const B = S.batch;
  const counts = s => B.lines.filter(l => l.status === s).length;
  const left = budgetLeft(), done = B.lines.filter(l => !['pending', 'searching'].includes(l.status)).length;
  const toSearch = B.lines.filter(l => l.status === 'pending' && !S.cache.has(norm(l.raw))).length;
  const pace = (LIMITS.windowSec / (LIMITS.searches - LIMITS.batchKeepFree));
  const approved = B.lines.filter(l => l.approved && !l.dl).length, confident = B.lines.filter(l => l.status === 'match' && !l.approved && !l.dl).length;
  $('#content').innerHTML = `<div class="batch">
    <div class="batch-left">
      <div class="sec-h" style="margin:0"><h3>Tracklist</h3><span>${plural(B.lines.length, 'line')}, one track per line</span></div>
      <textarea class="tracklist" id="tracklist" data-ch="batch-text" spellcheck="false" aria-label="Tracklist, one track per line"${B.running ? ' readonly' : ''}>${esc(B.text)}</textarea>
      <label class="fl"><span class="fl-l">Preset for every line</span><select id="bPreset" data-ch="batch-preset"${B.running ? ' disabled' : ''}>${Object.entries(PRESETS).map(([k, p]) => `<option value="${k}"${k === B.preset ? ' selected' : ''}>${esc(p.name)}</option>`).join('')}</select></label>
      <label class="cb"><input type="checkbox" id="bSkip" data-ch="batch-skip"${B.skipOwned ? ' checked' : ''}${B.running ? ' disabled' : ''}>Skip tracks I already have</label>
      <label class="cb"><input type="checkbox" id="bSpread" data-ch="batch-spread"${B.spread ? ' checked' : ''}>Spread downloads across peers</label>
      <div class="card">
        <div class="row"><b>Pacing</b><span class="muted num">${left} of ${LIMITS.searches} searches left</span></div>
        <div class="pace" aria-hidden="true">${B.lines.map(l => `<i class="${l.status === 'searching' ? 'now' : l.status === 'owned' ? 'skip' : l.status === 'pending' ? '' : 'done'}"></i>`).join('')}</div>
        <div class="muted">One search every ${pace.toFixed(1)} s, keeping ${LIMITS.batchKeepFree} searches free for you. ${toSearch ? `About ${fmtDur(toSearch * pace)} for the ${plural(toSearch, 'line')} left to search.` : 'Lines you searched recently open from the cache and cost nothing.'}</div>
        <label class="cb"><input type="checkbox" id="bDemo" data-ch="batch-demo"${B.demo ? ' checked' : ''}>Demo clock ×10 <span class="faint">(mock only)</span></label>
        <div class="row" style="justify-content:flex-start;gap:8px">${B.running ? `<button class="btn" data-act="batch-pause">${icon('pause')} Pause</button>` : `<button class="btn primary" data-act="batch-start"${B.lines.some(l => l.status === 'pending') ? '' : ' disabled'}>${icon('search')} ${B.started ? 'Resume' : 'Start searching'}</button>`}<button class="btn" data-act="batch-reset"${B.running ? ' disabled' : ''}>Re-read list</button></div>
      </div>
    </div>
    <div class="batch-right">
      <div class="summary"><span><b>${counts('match')}</b> confident</span><span><b>${counts('review')}</b> to review</span><span><b>${counts('notfound')}</b> not found</span><span><b>${counts('owned')}</b> you have</span><span class="grow-1"></span><span>Nothing downloads until you approve it.</span></div>
      <div class="scroll"><table class="tbl" style="min-width:600px"><colgroup><col style="width:36px"><col style="width:30%"><col style="width:106px"><col><col style="width:50px"><col style="width:92px"></colgroup>
        <thead><tr><th>#</th><th>Line</th><th>Status</th><th>Best match</th><th class="r">Match</th><th class="r">Review</th></tr></thead>
        <tbody id="batchBody">${batchRows()}</tbody></table></div>
      <div class="footbar"><button class="btn" data-act="batch-approve-all"${confident ? '' : ' disabled'}>Approve ${confident || ''} confident</button><button class="btn primary" data-act="batch-download"${approved ? '' : ' disabled'}>${icon('download')} Download ${approved || ''} approved</button><span>Approved downloads respect your per-peer limits${B.spread ? ' and spread across peers' : ''}.</span></div>
    </div></div>`;
}
function inspectorBatch() {
  const B = S.batch, l = B && B.lines.find(x => x.id === B.sel);
  if (!l) return '<div class="insp-empty">Select a line to compare its best match with the alternatives.</div>';
  const cand = l.best ? [l.best, ...l.alts] : [];
  return `<div class="insp"><div><div class="insp-title">${esc(l.title)}${l.version ? ` <span class="muted">(${esc(l.version)})</span>` : ''}</div><div class="insp-sub">${esc(l.artist || 'Unknown artist')} · line ${l.n}</div></div>
    ${l.status === 'review' ? `<div class="note warn">${icon('approx-c')}<div><b>Check this one.</b> The best match is ${Math.round(l.scoreV * 100)}% similar to your line. Preview it before approving.</div></div>` : ''}
    ${l.status === 'notfound' ? `<div class="note">${icon('info')}<div>Nothing matched within “${esc(PRESETS[B.preset].name)}”. <button class="link" data-act="add-wishlist" data-v="${esc(l.raw)}">Add it to the wishlist</button></div></div>` : ''}
    ${cand.length ? `<section class="insp-sec"><h4>Candidates</h4><div class="group">${cand.map((r, i) => `<div class="grow" style="gap:8px"><button class="rb" style="opacity:1" data-act="preview" data-id="${r.id}" aria-label="Preview">${icon('play')}</button><div class="gl" style="min-width:0">${esc(fullTitle(r.title, r.version))}<div class="gd">${esc(qualLong(r))} · ${esc(r.peer.name)} · ${r.peer.free ? 'free' : `queue ${r.peer.queue}`}</div></div>${i === 0 ? '<span class="bdg good">' + icon('check') + 'Best</span>' : `<button class="btn sm" data-act="batch-pick" data-id="${r.id}">Use</button>`}</div>`).join('')}</div></section>` : ''}
  </div>`;
}

/* ---------- other views ---------- */
const RATIO = {
  session: { up: { files: 7, bytes: 0.24e9 }, down: { files: 4, bytes: 0.11e9 } },
  '30d': { up: { files: 41, bytes: 1.9e9 }, down: { files: 66, bytes: 3.1e9 } },
  all: { up: { files: 1205, bytes: 38.2e9 }, down: { files: 1870, bytes: 61.4e9 } },
};
const UPLOAD_QUEUE = [
  { user: 'oddfellow', file: 'Halden & Rue - Brine.mp3', size: 12e6, speed: 0.9e6, state: 'active' },
  { user: 'marsh_tones', file: 'Teodor Lumen - Fieldwork.mp3', size: 13.3e6, speed: 1.6e6, state: 'active' },
  { user: 'bassbin', file: 'Kassia Moreau - Paper Lanterns.mp3', size: 15e6, speed: 2.1e6, state: 'active' },
];
const tile = (l, v, s = '', cls = '') => `<div class="tile"><span class="l">${esc(l)}</span><span class="v">${v}</span>${s ? `<span class="s ${cls}">${s}</span>` : ''}</div>`;
function renderCollection() {
  const st = COLLECTION.stats, total = COLLECTION.formats.reduce((a, f) => a + f[1], 0);
  const colors = ['var(--c1)', 'var(--c2)', 'var(--c3)', 'var(--c4)'];
  const pairs = [];
  for (const it of S.inbox.filter(i => i.kind === 'album')) for (const x of albumLinks(it.rel).inColl) pairs.push({ single: x.track.path, album: `${it.artist} — ${it.album} (${it.year})`, where: 'in your Inbox' });
  pairs.push({ single: 'Singles/Pale Monarch - Kestrel.mp3', album: 'Pale Monarch — Thermals (2019)', where: 'in Albums' });
  $('#content').innerHTML = `<div class="scroll"><div class="pane">
    <div class="tiles">${tile('Tracks', fmtInt(st.tracks), esc(COLLECTION.root))}${tile('Size on disk', fmtSize(st.bytes), 'Scanned 2 min ago, watching for changes')}${tile('Singles', fmtInt(st.singles), `${COLLECTION.singlesDir}/, one flat folder`)}${tile('Albums', fmtInt(st.albums), `${COLLECTION.albumsDir}/, one folder each`)}</div>
    <section><div class="sec-h"><h3>Formats</h3><span>${fmtInt(total)} audio files</span></div>
      <div class="group" style="padding:12px 14px">
        <div class="stack" role="img" aria-label="${COLLECTION.formats.map(f => `${f[0]} ${f[1]}`).join(', ')}">${COLLECTION.formats.map((f, i) => `<i style="width:${(f[1] / total) * 100}%;background:${colors[i]}" title="${f[0]}: ${fmtInt(f[1])} files (${Math.round((f[1] / total) * 100)}%)"></i>`).join('')}</div>
        <div class="legend">${COLLECTION.formats.map((f, i) => `<span><span class="sw" style="background:${colors[i]}"></span>${f[0]} <span class="n">${fmtInt(f[1])} · ${Math.round((f[1] / total) * 100)}%</span></span>`).join('')}</div>
      </div></section>
    <section><div class="sec-h"><h3>Folders</h3><span>Read-only. Selecta never moves, renames, re-tags or deletes files here.</span></div>
      <div class="group">
        <div class="grow">${icon('folder')}<div class="gl"><span class="mono">${esc(COLLECTION.root)}</span><div class="gd">Collection root · ${fmtInt(st.tracks)} tracks</div></div><span class="bdg good">${icon('check-c')}Indexed</span></div>
        <div class="grow">${icon('folder')}<div class="gl"><span class="mono">${esc(COLLECTION.root)}/${COLLECTION.singlesDir}/</span><div class="gd">Singles · ${fmtInt(st.singles)} files</div></div></div>
        <div class="grow">${icon('folder')}<div class="gl"><span class="mono">${esc(COLLECTION.root)}/${COLLECTION.albumsDir}/</span><div class="gd">Albums · ${fmtInt(st.albums)} folders</div></div></div>
        <div class="grow">${icon('inbox')}<div class="gl"><span class="mono">~/Music/Selecta/Inbox/</span><div class="gd">Downloads wait here until you file them · ${plural(S.inbox.length, 'item')}</div></div><button class="btn sm" data-act="nav" data-v="inbox">Open Inbox</button></div>
      </div></section>
    <section><div class="sec-h"><h3>Singles that are also on an album</h3><span>Flagged only. Remove the single in rekordbox and Finder yourself if you want to.</span></div>
      <div class="group">${pairs.map(p => `<div class="grow">${icon('flag')}<div class="gl"><span class="mono">${esc(p.single)}</span><div class="gd">Also on ${esc(p.album)}, ${esc(p.where)}</div></div></div>`).join('')}</div></section>
    <section><div class="sec-h"><h3>Recently filed</h3></div>
      <div class="group">${COLLECTION.recent.slice(0, 6).map(x => `<div class="grow">${icon('music')}<div class="gl">${esc(x.name)}<div class="gd mono">${esc(x.path)}</div></div><span class="muted">${esc(x.q)}</span><span class="faint">${esc(x.when)}</span></div>`).join('')}</div></section>
  </div></div>`;
}
function renderHistory() {
  $('#content').innerHTML = `<div class="scroll"><table class="tbl" style="min-width:780px"><colgroup><col style="width:70px"><col><col style="width:150px"><col style="width:150px"><col style="width:260px"></colgroup>
    <thead><tr><th>Time</th><th>Download</th><th>Quality</th><th>From</th><th>Now</th></tr></thead><tbody>
    ${SESSIONS.map(se => {
      const items = S.history.filter(h => h.session === se.n);
      return `<tr class="g"><td colspan="5"><div class="gwrap"><span class="gname">Session ${se.n}</span><span class="gmeta">${esc(se.when)} · ${esc(se.range)} · ${plural(items.length, 'download')}</span>${se.current ? '<span class="tag">CURRENT</span>' : ''}</div></td></tr>${items.map((h, i) => `<tr class="${i % 2 ? 'alt' : ''}"><td class="num faint">${esc(h.time)}</td><td>${esc(h.artist)} — ${esc(h.album ? `${h.title} (album, ${h.n} tracks)` : fullTitle(h.title, h.version))}</td><td class="muted">${esc(h.q)}</td><td class="muted">${esc(h.peer)}</td><td>${h.where.startsWith('Filed') ? `<span class="bdg good">${icon('check-c')}${esc(h.where)}</span>` : h.where.startsWith('Trash') ? `<span class="bdg mute">${icon('trash')}${esc(h.where)}</span>` : `<span class="bdg acc">${icon('tray')}${esc(h.where)}</span>`}</td></tr>`).join('')}`;
    }).join('')}</tbody></table></div>`;
}
const TSTATE = t => {
  switch (t.state) {
    case 'waiting': return `Waiting · ${LIMITS.peerConcurrent} at a time per peer`;
    case 'queued': return `Queued at peer · position ${t.queuePos}`;
    case 'connecting': return 'Connecting…';
    case 'active': return `${fmtSpeed(t.speed)} · ${fmtDur((t.size * (1 - t.progress)) / Math.max(1, t.speed * (t.kind === 'preview' ? 1.6 : t.slow ? 0.15 : SIM_SPEEDUP)))} left`;
    case 'done': return 'Done';
    case 'cancelled': return 'Cancelled: preview skipped';
    default: return t.reason || 'Failed';
  }
};
function renderTransfers() {
  const downs = S.transfers, ups = S.uploads;
  const body = S.transfersTab === 'down'
    ? `<table class="tbl" style="min-width:860px"><colgroup><col><col style="width:150px"><col style="width:250px"><col style="width:170px"><col style="width:200px"></colgroup><thead><tr><th>Item</th><th>Peer</th><th>Status</th><th>Progress</th><th>To</th></tr></thead><tbody>${downs.map((t, i) => `<tr class="${i % 2 ? 'alt' : ''}"><td>${t.kind === 'preview' ? '<span class="tag">PREVIEW</span> ' : ''}${esc(t.name)} <span class="faint">· ${esc(t.qual)}</span></td><td class="muted">${esc(t.peer.name)}</td><td class="${t.state === 'failed' ? '' : 'muted'}">${t.state === 'failed' ? `<span class="bdg bad">${icon('ban')}${esc(TSTATE(t))}</span>` : esc(TSTATE(t))}</td><td><div class="prog"><div class="bar"><i style="width:${(t.progress * 100).toFixed(1)}%"></i></div><span class="num muted" style="font-size:11px;min-width:32px;text-align:right">${Math.floor(t.progress * 100)}%</span></div></td><td class="mono faint">${esc(t.kind === 'preview' ? 'Preview cache' : t.dest)}</td></tr>`).join('') || '<tr><td colspan="5" style="height:100px;text-align:center;color:var(--label-2)">No downloads yet this session.</td></tr>'}</tbody></table>`
    : `<table class="tbl" style="min-width:700px"><colgroup><col style="width:170px"><col><col style="width:200px"><col style="width:110px"></colgroup><thead><tr><th>To user</th><th>File</th><th>Progress</th><th>Speed</th></tr></thead><tbody>${ups.map((u, i) => `<tr class="${i % 2 ? 'alt' : ''}"><td>${esc(u.user)}</td><td class="muted">${esc(u.file)}</td><td><div class="prog"><div class="bar"><i style="width:${Math.min(100, u.progress * 100).toFixed(1)}%"></i></div><span class="num muted" style="font-size:11px">${u.state === 'done' ? 'Done' : Math.floor(u.progress * 100) + '%'}</span></div></td><td class="num muted">${u.state === 'done' ? '' : fmtSpeed(u.speed)}</td></tr>`).join('')}</tbody></table>`;
  $('#content').innerHTML = `<div class="viewbar"><div class="vb-row"><span class="seg" role="group" aria-label="Direction"><button data-act="tr-tab" data-v="down" class="${S.transfersTab === 'down' ? 'on' : ''}">Downloads</button><button data-act="tr-tab" data-v="up" class="${S.transfersTab === 'up' ? 'on' : ''}">Uploads</button></span><span class="grow-1"></span><span class="fl-l">Downloads: ${LIMITS.peerConcurrent} at a time per peer, ${LIMITS.peerQueued} queued per peer · Uploads: 3 slots, 4 MB/s cap</span></div></div><div class="scroll" id="trScroll">${body}</div>`;
}
function renderPeers() {
  const R = RATIO[S.peersWindow], ratio = R.down.bytes ? R.up.bytes / R.down.bytes : 0;
  const st = ratio >= 1 ? ['good', 'check-c', 'Healthy: you give back more than you take'] : ratio >= 0.5 ? ['', 'info', 'Fair: keep sharing and spread your downloads'] : ['warn', 'warn', 'Low: you take much more than you give'];
  const list = PEERS.filter(p => p.today || p.d30 || p.session).sort((a, b) => b.today - a.today || b.d30 - a.d30);
  $('#content').innerHTML = `<div class="viewbar"><div class="vb-row"><span class="seg" role="group" aria-label="Period">${[['session', 'This session'], ['30d', '30 days'], ['all', 'All time']].map(([k, l]) => `<button data-act="peers-window" data-v="${k}" class="${S.peersWindow === k ? 'on' : ''}">${l}</button>`).join('')}</span><span class="grow-1"></span><span class="fl-l">Per peer: ${LIMITS.peerConcurrent} at a time · ${LIMITS.peerQueued} queued · warning at ${LIMITS.peerSoft} a day · stop at ${LIMITS.peerHard}</span><button class="btn sm" data-act="nav" data-v="settings">Change limits</button></div></div>
  <div class="scroll"><div class="pane">
    <div class="tiles">${tile('Ratio (uploaded ÷ downloaded)', ratio.toFixed(2), `${icon(st[1])}${esc(st[2])}`, st[0])}${tile('Uploaded', fmtSize(R.up.bytes), `${plural(R.up.files, 'file')} to other users`)}${tile('Downloaded', fmtSize(R.down.bytes), `${plural(R.down.files, 'file')}, previews included`)}${tile('Sharing', '8,412', 'files from 1 folder')}</div>
    <section><div class="sec-h"><h3>Peers you downloaded from</h3><span>Counts include previews: they are real transfers for the peer.</span></div>
    <div style="overflow-x:auto"><table class="tbl" style="min-width:760px"><colgroup><col><col style="width:64px"><col style="width:84px"><col style="width:70px"><col style="width:70px"><col style="width:76px"><col style="width:190px"></colgroup>
      <thead><tr><th>Peer</th><th class="r">Today</th><th class="r">Session</th><th class="r">30 days</th><th class="r">All time</th><th>Last</th><th>Today vs limit</th></tr></thead>
      <tbody>${list.map((p, i) => { const c = capState(p); return `<tr class="${i % 2 ? 'alt' : ''}"><td>${esc(p.name)}${p.backoff ? ` <span class="bdg warn">${icon('ban')}Backing off: ${esc(p.backoff.reason)}</span>` : ''}</td><td class="r num">${p.today}</td><td class="r num">${p.session}</td><td class="r num">${p.d30}</td><td class="r num">${p.all}</td><td class="muted">${esc(p.last || '')}</td><td><div class="prog"><div class="meter${c === 'cap' ? ' bad' : c === 'near' ? ' warn' : ''}" style="flex:1"><i style="width:${Math.min(100, (p.today / LIMITS.peerHard) * 100)}%"></i></div><span class="muted" style="font-size:11px">${c === 'cap' ? 'Limit reached' : c === 'near' ? 'Near limit' : 'OK'}</span></div></td></tr>`; }).join('')}</tbody></table></div></section>
  </div></div>`;
}
function renderMessages() {
  const m = S.msgs[S.msgSel] || S.msgs[0];
  if (m) m.unread = 0;
  $('#content').innerHTML = `<div class="msgs"><div class="msg-list">${S.msgs.map((x, i) => `<div class="msg-item${i === S.msgSel ? ' on' : ''}" role="button" tabindex="0" data-act="msg-sel" data-v="${i}"><span class="top">${icon('user')}${esc(x.peer)}${x.unread ? '<span class="dot" style="background:var(--accent)"></span>' : ''}</span><span class="pv">${esc(x.items[x.items.length - 1].text)}</span></div>`).join('')}</div>
    <div class="thread"><div class="bubbles">${m ? m.items.map(b => `<div class="bubble${b.me ? ' me' : ''}">${esc(b.text)}<small>${esc(b.t)}</small></div>`).join('') : ''}</div>
    <form class="composer" id="composer"><input id="msgInput" placeholder="Message ${esc(m ? m.peer : '')}" aria-label="Message"><button class="btn primary" type="submit">Send</button></form></div></div>`;
  renderNav();
}
function renderShares() {
  $('#content').innerHTML = `<div class="scroll"><div class="pane">
    <section><div class="sec-h"><h3>Shared folders</h3><span>Other users can browse and search these</span></div><div class="group">
      <div class="grow">${icon('folder')}<div class="gl"><span class="mono">${esc(COLLECTION.root)}</span><div class="gd">${fmtInt(COLLECTION.stats.tracks)} files · ${fmtSize(COLLECTION.stats.bytes)} · shown to peers as <span class="mono">Collection\\…</span>, never your full path</div></div><select aria-label="Who can see this folder"><option>Everyone</option><option>Buddies only</option></select></div>
      <div class="grow" style="justify-content:flex-end"><button class="btn sm">${icon('plus')} Add folder…</button></div></div></section>
    <section><div class="sec-h"><h3>Never shared</h3></div><div class="group"><div class="grow">${icon('lock')}<div class="gl">Inbox and preview cache<div class="gd">Unfiled downloads and previews stay private until you file them.</div></div></div></div></section>
    <section><div class="sec-h"><h3>Uploads</h3></div><div class="group">
      <div class="grow"><div class="gl">Upload slots<div class="gd">How many people download from you at once</div></div><select aria-label="Upload slots"><option>2</option><option selected>3</option><option>4</option></select></div>
      <div class="grow"><div class="gl">Upload speed limit</div><select aria-label="Upload speed limit"><option>No limit</option><option selected>4 MB/s</option><option>2 MB/s</option></select></div>
      <div class="grow"><div class="gl">Queue limit per user<div class="gd">Further requests get “Too many files”</div></div><span class="muted">50 files · 2 GB</span></div></div></section>
    <section><div class="sec-h"><h3>Today</h3></div><div class="tiles">${tile('Searches answered', '1,204', 'from the distributed network')}${tile('Users browsing you', '37')}${tile('Files uploaded', fmtInt(RATIO.session.up.files), 'this session')}${tile('Excluded phrases', '212', 'from the server; matching searches get no answer')}</div></section>
  </div></div>`;
}
function renderWishlist() {
  $('#content').innerHTML = `<div class="viewbar"><div class="vb-row"><span class="fl-l">The server sets how often wishlist searches run: every 12 minutes right now.</span><span class="grow-1"></span><button class="btn sm" data-act="add-wishlist" data-v="">${icon('plus')} Add search</button></div></div>
  <div class="scroll"><table class="tbl" style="min-width:640px"><colgroup><col><col style="width:220px"><col style="width:120px"><col style="width:140px"></colgroup><thead><tr><th>Search</th><th>Preset</th><th>Last run</th><th>New results</th></tr></thead><tbody>
  ${S.wishlist.map((w, i) => `<tr class="${i % 2 ? 'alt' : ''}"><td>${esc(w.q)}</td><td class="muted">${esc(PRESETS[w.preset].name)}</td><td class="muted">${esc(w.last)}</td><td>${w.found ? `<button class="btn sm" data-act="wish-open" data-v="${esc(w.q)}">Show ${w.found} new</button>` : '<span class="faint">None yet</span>'}</td></tr>`).join('')}</tbody></table></div>`;
}

/* ---------- converter ---------- */
const CONV_FILES = [
  { id: 'c1', name: 'Halden & Rue — Salt Flats', n: 8, src: { fmt: 'FLAC', sr: 44100, bd: 16 }, where: 'Inbox/Albums/Halden & Rue - Salt Flats (2022)/', tags: { TITLE: 'Salt Flats', ARTIST: 'Halden & Rue', ALBUM: 'Salt Flats', ALBUMARTIST: 'Halden & Rue', TRACK: '1/8', DISC: '1/1', DATE: '2022', GENRE: 'Deep House', BPM: '118', KEY: '8A', LABEL: 'Saltwater Audio', ISRC: 'GB-SWA-22-00101' }, art: { from: 'folder', file: 'cover.jpg', w: 1400 } },
  { id: 'c2', name: 'Okuma Drift — Sodium', n: 1, src: { fmt: 'FLAC', sr: 44100, bd: 16 }, where: 'Inbox/Singles/', tags: { TITLE: 'Sodium', ARTIST: 'Okuma Drift', ALBUM: '', ALBUMARTIST: '', TRACK: '', DISC: '', DATE: '2024', GENRE: 'Progressive House', BPM: '122', KEY: '5A', LABEL: '', ISRC: '' }, art: { from: 'embedded', w: 600 }, supplement: { ALBUM: 'Sodium', ALBUMARTIST: 'Okuma Drift', TRACK: '1/1' } },
  { id: 'c3', name: 'Marlo Vance — Tidal Memory (Original Mix)', n: 1, src: { fmt: 'MP3', br: 192 }, where: 'Collection/Singles/', tags: { TITLE: 'Tidal Memory (Original Mix)', ARTIST: 'Marlo Vance', ALBUM: 'Tidal Memory', ALBUMARTIST: 'Marlo Vance', TRACK: '1/2', DISC: '', DATE: '2021', GENRE: 'Techno', BPM: '126', KEY: '', LABEL: 'Undertow Records', ISRC: '' }, art: null },
  { id: 'c4', name: 'Ines Varga — Vesper', n: 1, src: { fmt: 'FLAC', sr: 96000, bd: 24 }, where: 'Inbox/Singles/', tags: { TITLE: 'Vesper', ARTIST: 'Ines Varga', ALBUM: 'Glass Choir', ALBUMARTIST: 'Ines Varga', TRACK: '2/3', DISC: '1/1', DATE: '2025', GENRE: 'Melodic Techno', BPM: '125', KEY: '11A', LABEL: 'Vitrail', ISRC: 'DE-VTR-25-00002' }, art: { from: 'folder', file: 'folder.jpg', w: 3000 } },
  { id: 'c5', name: 'Pale Monarch — Kestrel', n: 1, src: { fmt: 'MP3', br: 256 }, where: 'Collection/Singles/', tags: { TITLE: 'Kestrel', ARTIST: 'Pale Monarch', ALBUM: 'Thermals', ALBUMARTIST: 'Pale Monarch', TRACK: '3/5', DISC: '', DATE: '2019', GENRE: 'Minimal', BPM: '123', KEY: '', LABEL: '', ISRC: '' }, art: { from: 'embedded', w: 500 } },
];
function convVerdict(src, tg) {
  const sl = LOSSY.has(src.fmt);
  if (tg.asis) return { lvl: 'ok', t: 'No conversion', d: 'Files stay as they are.' };
  const tl = LOSSY.has(tg.fmt);
  if (sl && !tl) return { lvl: 'warn', t: 'No quality gain', d: `The ${src.br} kbps source stays ${src.br} kbps quality, in a file about ${Math.round(1411 / src.br)}× larger.`, confirm: true };
  if (sl && tl) {
    if (src.fmt === tg.fmt && src.br === tg.br) return { lvl: 'info', t: 'Same format', d: 'Audio copied as-is; tags and artwork completed.' };
    if ((tg.br || 0) > src.br) return { lvl: 'warn', t: 'No quality gain', d: `A ${src.br} kbps file can't get better, and re-encoding adds loss.`, confirm: true };
    return { lvl: 'warn', t: 'Generation loss', d: `The copy sounds slightly worse than the ${src.br} kbps source.`, confirm: true };
  }
  if (!sl && tl) return { lvl: 'ok', t: 'Lossless → lossy', d: 'Expected for a DJ library. The original stays untouched.' };
  if (tg.lossless || (src.sr === tg.sr && src.bd === tg.bd)) return { lvl: 'ok', t: 'Lossless → lossless', d: 'Identical audio in a new container.' };
  if (src.sr > tg.sr || src.bd > tg.bd) return { lvl: 'info', t: 'Downsampled', d: `${src.bd}/${khz(src.sr)} → ${tg.bd}/${khz(tg.sr)}, resampled and dithered.` };
  return { lvl: 'warn', t: 'No quality gain', d: 'Upsampling adds size, not detail.', confirm: true };
}
const LVL = { ok: ['good', 'check-c'], info: ['acc', 'info'], warn: ['warn', 'warn'] };
function renderConverter() {
  const C = S.conv, tg = TARGETS[C.target], cur = CONV_FILES[C.sel];
  const targetSeg = Object.entries(TARGETS).filter(([k]) => k !== 'asis').map(([k, t]) => `<button data-act="conv-target" data-v="${k}" class="${C.target === k ? 'on' : ''}">${t.label}</button>`).join('');
  const rows = CONV_FILES.map((f, i) => {
    const v = convVerdict(f.src, tg), st = C.state[f.id], lv = LVL[v.lvl];
    return `<tr class="r${i % 2 ? ' alt' : ''}${i === C.sel ? ' sel' : ''}" data-act="conv-sel" data-v="${i}" style="height:48px">
      <td><div class="t">${esc(f.name)}${f.n > 1 ? ` <span class="faint">· ${f.n} files</span>` : ''}<div class="faint" style="font-size:11px">${esc(qualLong(f.src))} · <span class="mono" style="font-size:10.5px">${esc(f.where)}</span></div></div></td>
      <td style="white-space:normal">${st ? `<div class="prog"><div class="bar"><i style="width:${(st.p * 100).toFixed(1)}%"></i></div><span class="muted" style="font-size:11px">${st.p >= 1 ? 'Done' : 'Converting'}</span></div>` : `<span class="bdg ${lv[0]}" style="white-space:normal;align-items:flex-start">${icon(lv[1])}<span><b>${esc(v.t)}.</b> ${esc(v.d)}</span></span>`}</td>
      <td class="r">${v.confirm && !st ? `<label class="cb"><input type="checkbox" data-ch="conv-confirm" data-id="${f.id}"${C.confirmed.has(f.id) ? ' checked' : ''}>Convert anyway</label>` : ''}</td></tr>`;
  }).join('');
  const v23 = C.id3 === '2.3', toId3 = !['FLAC'].includes(tg.fmt);
  const fields = [['TITLE', 'TIT2', 'Title'], ['ARTIST', 'TPE1', 'Artist'], ['ALBUM', 'TALB', 'Album'], ['ALBUMARTIST', 'TPE2', 'Album artist'], ['TRACK', 'TRCK', 'Track'], ['DISC', 'TPOS', 'Disc'], ['DATE', v23 ? 'TYER' : 'TDRC', 'Year'], ['GENRE', 'TCON', 'Genre'], ['BPM', 'TBPM', 'BPM'], ['KEY', 'TKEY', 'Key'], ['LABEL', 'TPUB', 'Label'], ['ISRC', 'TSRC', 'ISRC']];
  const srcTag = LOSSY.has(cur.src.fmt) ? 'ID3v2' : 'Vorbis comment';
  const art = cur.art;
  const artLine = art ? `${art.from === 'folder' ? `<span class="mono">${esc(art.file)}</span> from the folder` : 'Embedded picture'} · ${art.w}×${art.w}${C.artMax && art.w > C.artMax ? ` → resized to ${C.artMax}×${C.artMax}` : ''}` : 'No artwork found in the file or its folder';
  $('#content').innerHTML = `<div class="viewbar">
      <div class="vb-row"><span class="fl-l">Convert to</span><span class="seg" role="group" aria-label="Target format">${targetSeg}</span></div>
      <div class="vb-row secondary" style="display:flex"><label class="fl"><span class="fl-l">Tags</span><select data-ch="conv-id3" id="cId3"${toId3 ? '' : ' disabled'}><option value="2.3"${v23 ? ' selected' : ''}>ID3v2.3 (best for DJ gear)</option><option value="2.4"${v23 ? '' : ' selected'}>ID3v2.4</option></select></label>
      <label class="fl"><span class="fl-l">Artwork</span><select data-ch="conv-art" id="cArt"><option value="1000"${C.artMax === 1000 ? ' selected' : ''}>Embed, max 1000 px</option><option value="600"${C.artMax === 600 ? ' selected' : ''}>Embed, max 600 px</option><option value="0"${C.artMax === 0 ? ' selected' : ''}>Embed at original size</option></select></label>
      <span class="fl-l">New files go next to the originals. Originals are never changed.</span><span class="grow-1"></span>
      <button class="btn primary" data-act="conv-run"${C.running ? ' disabled' : ''}>${icon('convert')} Convert</button></div>
    </div>
    <div class="conv">
      <div class="conv-left"><table class="tbl" style="min-width:560px"><colgroup><col style="width:42%"><col><col style="width:128px"></colgroup><thead><tr><th>Source</th><th>Result as ${esc(tg.label)}</th><th></th></tr></thead><tbody>${rows}</tbody></table></div>
      <div class="conv-right"><div class="pane" style="gap:14px">
        <div style="display:flex;gap:12px;align-items:center"><span class="art" style="background:${art ? artGradient(hashStr(cur.name)) : 'var(--fill-2)'}"></span><div style="min-width:0"><div class="insp-title">${esc(cur.name)}</div><div class="insp-sub">${esc(srcTag)} → ${toId3 ? `ID3v${C.id3}` : 'Vorbis comment'}</div><div class="insp-sub">${artLine}${art ? ' → APIC front cover' : ''}</div></div></div>
        <table class="tbl mapt"><colgroup><col style="width:34%"><col style="width:20%"><col></colgroup><thead><tr><th>${esc(srcTag)}</th><th>${toId3 ? `ID3v${C.id3}` : 'Vorbis'}</th><th>Value</th></tr></thead><tbody>
          ${fields.map(([vk, fr, label], i) => { const val = cur.tags[vk] || (cur.supplement && cur.supplement[vk]) || ''; const sup = !cur.tags[vk] && cur.supplement && cur.supplement[vk]; return `<tr class="${i % 2 ? 'alt' : ''}"><td class="src mono">${LOSSY.has(cur.src.fmt) ? fr : vk}</td><td class="mono">${toId3 ? fr : vk}</td><td>${val ? esc(val) : '<span class="faint">Empty</span>'}${sup ? '<span class="sup" title="Filled from the folder and file names">ADDED</span>' : ''}</td></tr>`; }).join('')}
          <tr><td class="src mono">${art ? (art.from === 'folder' ? esc(art.file) : 'PICTURE') : '—'}</td><td class="mono">${toId3 ? 'APIC' : 'PICTURE'}</td><td>${art ? `Front cover${art.from === 'folder' ? '<span class="sup">ADDED</span>' : ''}` : '<span class="faint">None</span>'}</td></tr>
        </tbody></table>
        <p class="explain">${v23 ? 'ID3v2.3 writes the year as TYER and uses UTF-16 text, which older CDJs and rekordbox read reliably.' : 'ID3v2.4 writes the year as TDRC and allows UTF-8; some older DJ hardware ignores v2.4 tags.'}</p>
      </div></div>
    </div>`;
}
function updateConvert(dt) {
  const C = S.conv; if (!C || !C.running) return false;
  const ids = Object.keys(C.state).filter(id => C.state[id].p < 1);
  if (!ids.length) { C.running = false; toast('Conversion finished', `${plural(Object.keys(C.state).length, 'item')} converted to ${esc(TARGETS[C.target].label)}. New files sit next to the originals.`, { kind: 'good', icon: 'convert' }); return true; }
  const id = ids[0], f = CONV_FILES.find(x => x.id === id);
  C.state[id].p = Math.min(1, C.state[id].p + dt / (1200 * f.n));
  return true;
}

/* ---------- settings ---------- */
function renderSettings() {
  const row = (label, desc, ctl) => `<div class="grow"><div class="gl">${label}${desc ? `<div class="gd">${desc}</div>` : ''}</div>${ctl}</div>`;
  const seg = (act, cur, opts) => `<span class="seg" role="group">${opts.map(([v, l]) => `<button data-act="${act}" data-v="${v}" class="${cur === v ? 'on' : ''}" aria-pressed="${cur === v}">${l}</button>`).join('')}</span>`;
  $('#content').innerHTML = `<div class="scroll"><div class="pane" style="max-width:820px">
    <section><div class="sec-h"><h3>Appearance</h3></div><div class="group">${row('Theme', 'Follows macOS unless you pick one', seg('set-theme', S.theme, [['system', 'System'], ['light', 'Light'], ['dark', 'Dark']]))}${row('Waveform style', 'The preview waveform stays dark in both themes', seg('wf-mode', P.mode, [['pioneer', 'Pioneer'], ['layered', 'Layered'], ['stacked', 'Stacked'], ['lanes', 'Lanes']]))}</div></section>
    <section><div class="sec-h"><h3>Network</h3><span>Debug builds only connect to a local test server</span></div><div class="group">
      ${row('Server', S.server === 'live' ? 'server.slsknet.org · the real Soulseek network' : 'localhost:2242 · Soulfind test server with test peers', seg('set-server', S.server, [['live', 'Soulseek network'], ['local', 'Local test server']]))}
      ${row('Listen port', 'Peers connect to you on this port', '<span class="mono">2234</span> <span class="bdg good">' + icon('check-c') + 'Open</span>')}
      ${row('Network activity', 'Pausing stops new searches and transfers', `<button class="btn" data-act="toggle-pause">${S.netPaused ? 'Resume' : 'Pause'}</button>`)}</div></section>
    <section><div class="sec-h"><h3>Limits</h3><span>You can make these stricter, never looser than the ceiling</span></div><div class="group">
      ${row('Searches', `At most ${LIMITS.searches} per ${LIMITS.windowSec} s · ceiling ${LIMITS.searches}`, '<select><option selected>34 per 220 s</option><option>20 per 220 s</option><option>10 per 220 s</option></select>')}
      ${row('Batch search', `Keeps ${LIMITS.batchKeepFree} searches free for you`, '<select><option selected>Keep 8 free</option><option>Keep 16 free</option></select>')}
      ${row('Downloads per peer', 'At the same time · queued', `<span class="muted">${LIMITS.peerConcurrent} at a time · ${LIMITS.peerQueued} queued</span>`)}
      ${row('Daily limit per peer', 'Warn, then stop until tomorrow', `<span class="muted">Warn at ${LIMITS.peerSoft} · stop at ${LIMITS.peerHard}</span>`)}
      ${row('Reconnect', 'Waits longer after each failed login', '<span class="muted">30 s, doubling up to 10 min</span>')}</div></section>
    <section><div class="sec-h"><h3>Library</h3></div><div class="group">
      ${row('Collection folder', 'Read-only. Add more folders if your music lives in several places', `<span class="mono">${esc(COLLECTION.root)}</span> <button class="btn sm">Change…</button>`)}
      ${row('Singles folder', 'Inside the collection', `<input class="field" style="width:120px" value="${COLLECTION.singlesDir}" aria-label="Singles folder name">`)}
      ${row('Albums folder', 'One folder per album: Artist - Album (Year)', `<input class="field" style="width:120px" value="${COLLECTION.albumsDir}" aria-label="Albums folder name">`)}
      ${row('Inbox', 'Where downloads wait until you file them', '<span class="mono">~/Music/Selecta/Inbox</span> <button class="btn sm">Change…</button>')}</div></section>
    <section><div class="sec-h"><h3>Filing</h3></div><div class="group">
      ${row('File as', 'Applied when you file from the Inbox', `<select data-ch="filingTarget" aria-label="Filing format">${Object.entries(TARGETS).map(([k, t]) => `<option value="${k}"${k === S.filingTarget ? ' selected' : ''}>${t.label}</option>`).join('')}</select>`)}
      ${row('Tags', 'ID3v2.3 reads best on CDJs and in rekordbox', '<select><option selected>ID3v2.3</option><option>ID3v2.4</option></select>')}
      ${row('Single file name', '', '<span class="mono">Artist - Title (Version).mp3</span>')}
      ${row('Lossless originals', 'After converting to MP3 at filing time', '<select aria-label="Lossless originals"><option selected>Move to the Trash</option><option>Keep in ~/Music/Selecta/Originals</option></select>')}</div></section>
    <section><div class="sec-h"><h3>Preview</h3></div><div class="group">
      ${row('Auto-preview', 'Start a preview when the selection rests on a track', `<input type="checkbox" data-ch="autoPreview"${S.autoPreview ? ' checked' : ''} aria-label="Auto-preview">`)}
      ${row('When you skip a preview', 'Cancelling saves the peer’s upload bandwidth', `<select data-ch="cancelOnSkip" aria-label="When you skip a preview"><option value="1"${S.cancelOnSkip ? ' selected' : ''}>Cancel the transfer</option><option value="0"${S.cancelOnSkip ? '' : ' selected'}>Let it finish</option></select>`)}
      ${row('Preview cache', 'Oldest previews are removed first', '<span class="muted">412 MB of 2 GB</span> <button class="btn sm">Clear</button>')}</div></section>
    <section><div class="sec-h"><h3>Caches</h3></div><div class="group">
      ${row('Search results', 'Re-running a search within 24 h opens the cache and costs no search', `<span class="muted">${S.cache.size} searches</span> <button class="btn sm">Clear</button>`)}
      ${row('Browsed shares', 'Kept 24 h so you don’t ask the same peer twice', '<span class="muted">3 users</span> <button class="btn sm">Clear</button>')}
      ${row('Waveforms', 'Small; re-opening a preview is instant', '<span class="muted">1.4 MB</span> <button class="btn sm">Clear</button>')}</div></section>
    <section><div class="sec-h"><h3>Keyboard</h3></div><div class="group" style="padding:12px 14px"><div class="shortcuts">
      <span class="kbd">↑ ↓</span><span>Previous / next result</span><span class="kbd">Space</span><span>Preview the selection, or play / pause</span><span class="kbd">← →</span><span>Seek 5 s (with ⇧: 30 s). On a folder: collapse / expand</span>
      <span class="kbd">Return</span><span>Download the selection (folder: the whole album)</span><span class="kbd">K</span><span>Keep the preview: move it to the Inbox</span><span class="kbd">⌫</span><span>Discard the preview</span>
      <span class="kbd">⌘F</span><span>Search</span><span class="kbd">⌘I</span><span>Inspector</span><span class="kbd">⌘1 … ⌘9</span><span>Sidebar sections</span></div></div></section>
  </div></div>`;
}

/* =====================================================================
   Player, synthetic preview audio, 3-band waveform, wiring, startup
   ===================================================================== */

/* ---------- player state ---------- */
const P = { track: null, playing: false, pos: 0, volume: 0.8, mode: store.get('wfMode', 'pioneer'), zoomSec: 8, waiting: false, autoplay: false, dirty: true, hoverX: null, drag: null, scrub: null };
if (!['pioneer', 'layered', 'stacked', 'lanes'].includes(P.mode)) P.mode = 'pioneer';
S.albums = new Map();
S.filing = new Map();
const PREVIEW_CACHE = new Map(); // result id -> track; insertion order = least recently used first
const MAX_AUDIO_TRACKS = 4;

/* ---------- sample data ---------- */
function fakeResult(relIdx, ti, peerName, vIdx) {
  const rel = CATALOG[relIdx], t = rel.tracks[ti], v = VARIANTS[vIdx], p = PEER[peerName];
  const kbps = LOSSY.has(v.fmt) ? v.br : v.fmt === 'FLAC' ? 900 : (v.sr * v.bd * 2) / 1000;
  const file = `${rel.artist} - ${fullTitle(t[0], t[1])}.${EXT[v.fmt]}`;
  const r = { id: `r${S.nextId++}`, peer: p, path: `${p.root}\\Downloads\\${file}`, folder: `${p.root}\\Downloads`, file, style: 'loose', rel, ti, trackNo: 0, artist: rel.artist, title: t[0], version: t[1], dur: t[2], fmt: v.fmt, br: v.br ?? null, vbr: !!v.vbr, sr: v.sr ?? null, bd: v.bd ?? null, size: (kbps * 1000 / 8) * t[2] + 1e5, suspicious: null, arrival: 0 };
  S.results.set(r.id, r);
  return r;
}
function seedData() {
  const rel = album => CATALOG.find(r => r.album === album);
  COLLECTION.tracks.push({ artist: 'Pale Monarch', title: 'Cloud Street', version: '', fmt: 'MP3', br: 320, path: 'Singles/Pale Monarch - Cloud Street.mp3' });
  COLLECTION.recent = [
    { name: 'Pale Monarch — Cloud Street', path: 'Singles/Pale Monarch - Cloud Street.mp3', q: 'MP3 320', when: 'Fri 17 Oct' },
    { name: 'Ines Varga — Hollow Bells (Edit)', path: 'Singles/Ines Varga - Hollow Bells (Edit).mp3', q: 'MP3 320', when: 'Sun 12 Oct' },
    { name: 'Teodor Lumen — Copper Season (6 tracks)', path: 'Albums/Teodor Lumen - Copper Season (2020)/', q: 'FLAC → MP3 320', when: 'Sun 5 Oct' },
    { name: 'Kassia Moreau — Paper Lanterns', path: 'Singles/Kassia Moreau - Paper Lanterns.mp3', q: 'MP3 320', when: 'Tue 2 Sep' },
  ];
  const single = (artist, title, version, q, more) => ({ id: 'ib' + S.nextId++, kind: 'single', artist, title, version, br: null, vbr: false, sr: null, bd: null, ...q, ...more });
  S.inbox = [
    { id: 'ib' + S.nextId++, kind: 'album', artist: 'Halden & Rue', album: 'Salt Flats', year: 2022, fmt: 'FLAC', br: null, vbr: false, sr: 44100, bd: 16, tracks: 8, size: 412e6, peer: 'basement.tapes', session: 14, rel: rel('Salt Flats') },
    single('Marlo Vance', 'Tidal Memory', 'Original Mix', { fmt: 'MP3', br: 320 }, { dur: 396, size: 15.9e6, peer: 'selector_k', session: 15, rel: rel('Tidal Memory') }),
    single('Ravel Ortiz', 'Room 404', '', { fmt: 'AIFF', sr: 44100, bd: 16 }, { dur: 389, size: 68.6e6, peer: 'vinyl_ghost', session: 15, rel: rel('Room 404') }),
    single('Sefa Kintu', 'Okada Ride', '', { fmt: 'MP3', br: 256 }, { dur: 352, size: 11.4e6, peer: 'tapehiss', session: 15, rel: rel('Lagos Night Bus') }),
    single('Juno Ashgrove', 'Velvet Machine', 'Radio Edit', { fmt: 'MP3', br: 320 }, { dur: 201, size: 8.2e6, peer: 'nightbus_records', session: 15, rel: rel('Velvet Machine') }),
    single('Okuma Drift', 'Sodium', '', { fmt: 'FLAC', sr: 44100, bd: 16 }, { dur: 381, size: 44e6, peer: 'flac_archivist', session: 14, rel: rel('Neon Rain') }),
  ];
  S.history = [
    { session: 15, artist: 'Juno Ashgrove', title: 'Velvet Machine', version: 'Radio Edit', q: 'MP3 320', peer: 'nightbus_records', where: 'Inbox', time: '20:11' },
    { session: 15, artist: 'Sefa Kintu', title: 'Okada Ride', version: '', q: 'MP3 256', peer: 'tapehiss', where: 'Inbox', time: '20:06' },
    { session: 15, artist: 'Ravel Ortiz', title: 'Room 404', version: '', q: 'AIFF 16/44.1', peer: 'vinyl_ghost', where: 'Inbox', time: '20:04' },
    { session: 15, artist: 'Marlo Vance', title: 'Tidal Memory', version: 'Original Mix', q: 'MP3 320', peer: 'selector_k', where: 'Inbox', time: '20:01' },
    { session: 14, artist: 'Halden & Rue', title: 'Salt Flats', version: '', album: true, n: 8, q: 'FLAC 16/44.1', peer: 'basement.tapes', where: 'Inbox', time: '23:20' },
    { session: 14, artist: 'Okuma Drift', title: 'Sodium', version: '', q: 'FLAC 16/44.1', peer: 'flac_archivist', where: 'Inbox', time: '22:58' },
    { session: 14, artist: 'Pale Monarch', title: 'Cloud Street', version: '', q: 'MP3 320', peer: 'cratedigger_77', where: 'Filed · Singles/', time: '22:31' },
    { session: 13, artist: 'Ines Varga', title: 'Hollow Bells', version: 'Edit', q: 'MP3 320', peer: 'cratedigger_77', where: 'Filed · Singles/', time: '15:40' },
    { session: 13, artist: 'Ravel Ortiz', title: 'Room 404', version: 'Dub', q: 'MP3 320', peer: 'lowend_theory', where: 'Trash', time: '15:12' },
    { session: 12, artist: 'Teodor Lumen', title: 'Copper Season', version: '', album: true, n: 6, q: 'FLAC → MP3 320', peer: 'subharmonic', where: 'Filed · Albums/Teodor Lumen - Copper Season (2020)/', time: '21:02' },
  ];
  const t1 = addDownload(fakeResult(8, 1, 'cratedigger_77', 0), { quiet: true });
  Object.assign(t1, { state: 'active', progress: 0.62, speed: 3.9e6, slow: true });
  const t2 = addDownload(fakeResult(0, 1, 'flac_archivist', 5), { quiet: true });
  Object.assign(t2, { state: 'queued', queuePos: 3, since: 0, qStep: 15000, slow: true });
  const t3 = addDownload(fakeResult(4, 0, 'tapehiss', 0), { quiet: true });
  Object.assign(t3, { state: 'failed', reason: 'Denied: “Too many files”' });
  S.uploads = [UPLOAD_QUEUE.shift(), UPLOAD_QUEUE.shift()].map((u, i) => ({ ...u, progress: i ? 0.18 : 0.64 }));
  S.msgs = [
    { peer: 'flac_archivist', unread: 1, items: [{ t: '20:09', text: 'hey, saw you queue a bunch from me today. all good, just keep it under 10 at a time please' }] },
    { peer: 'warehouse_w', unread: 0, items: [{ t: '19:12', text: 'thanks for sharing the Copper Season files, sounds great' }, { me: true, t: '19:14', text: 'enjoy! the whole album is in my shares' }] },
  ];
  S.wishlist = [
    { q: 'mirela stone undersea cathedral', preset: 'dj', last: '8 min ago', found: 0 },
    { q: 'halden rue salt flats', preset: 'lossless', last: '8 min ago', found: 3 },
    { q: 'teodor lumen fieldwork extended', preset: 'dj', last: '8 min ago', found: 0 },
  ];
  S.batch = { text: SAMPLE_TRACKLIST, parsedText: SAMPLE_TRACKLIST, preset: 'mp3', skipOwned: true, spread: true, demo: true, lines: [], running: false, nextAt: 0, sel: null, done: false };
  batchParse();
  S.conv = { target: 'mp3-320', id3: '2.3', artMax: 1000, sel: 0, state: {}, confirmed: new Set(), running: false };
}

/* ---------- views ---------- */
const VIEWS = {
  search: {
    insp: true, render: renderSearchView,
    title: () => {
      const s = curSearch();
      if (!s) return ['Search', 'Use the search field at the top of the sidebar'];
      const v = S._v;
      return [s.query, s.status === 'running' ? 'Searching…' : v ? `${fmtInt(v.shown.length)} of ${fmtInt(v.all.length)} results shown` : ''];
    },
  },
  batch: { insp: true, render: renderBatch, title: () => ['Batch search', `${plural(S.batch.lines.length, 'line')} · ${PRESETS[S.batch.preset].name}`] },
  wishlist: { render: renderWishlist, title: () => ['Wishlist', plural(S.wishlist.length, 'saved search', 'saved searches')] },
  inbox: {
    render: renderInbox,
    title: () => { const a = S.inbox.filter(i => i.kind === 'album').length, n = S.inbox.length - a; return ['Inbox', S.inbox.length ? `${plural(a, 'album')} · ${plural(n, 'single')} waiting to be filed` : 'Nothing waiting']; },
  },
  collection: { render: renderCollection, title: () => ['Collection', `${COLLECTION.root} · ${fmtInt(COLLECTION.stats.tracks)} tracks`] },
  history: { render: renderHistory, title: () => ['History', `Session ${SESSIONS[0].n} started at 19:58`] },
  transfers: {
    render: renderTransfers,
    title: () => {
      const d = S.transfers.filter(t => t.state === 'active').length, w = S.transfers.filter(t => ['waiting', 'queued', 'connecting'].includes(t.state)).length, u = S.uploads.filter(x => x.state === 'active').length;
      return ['Transfers', `${d} downloading · ${w} waiting · ${u} uploading`];
    },
  },
  peers: { render: renderPeers, title: () => ['Peers & ratio', `Ratio ${ratioFor('30d').toFixed(2)} over 30 days`] },
  messages: { render: renderMessages, title: () => { const n = S.msgs.reduce((a, m) => a + m.unread, 0); return ['Messages', n ? `${n} unread` : 'No unread messages']; } },
  shares: { render: renderShares, title: () => ['Shares', `${fmtInt(COLLECTION.stats.tracks)} files shared`] },
  converter: { render: renderConverter, title: () => ['Converter', `${CONV_FILES.length} items → ${TARGETS[S.conv.target].label}`] },
  settings: { render: renderSettings, title: () => ['Settings', ''] },
};

/* ---------- rendering & navigation ---------- */
function applyChrome() {
  $('#win').classList.toggle('sb-off', !S.sidebar);
  $('#middle').classList.toggle('insp-off', !S.inspector || !VIEWS[S.view].insp);
}
function render() { applyChrome(); renderNav(); renderFoot(); renderToolbar(); renderContent(); renderInspector(); renderPlayer(); }
function renderContent() { VIEWS[S.view].render(); }
function nav(id) {
  const cur = curSearch();
  if (S.view === 'search' && cur) cur.sel = S.sel;
  if (id.startsWith('search:')) { S.view = 'search'; S.searchId = id.slice(7); const s = curSearch(); S.sel = s ? s.sel : null; }
  else if (VIEWS[id]) S.view = id;
  if (window.innerWidth <= 760) S.sidebar = false;
  render();
  $('#content').focus({ preventScroll: true });
}
function closeSearch(id) {
  const i = S.searches.findIndex(s => s.id === id);
  if (i < 0) return;
  const wasCur = S.view === 'search' && S.searchId === id;
  S.searches.splice(i, 1);
  if (!wasCur) { renderNav(); return; }
  const next = S.searches[i] || S.searches[i - 1];
  if (next) nav('search:' + next.id); else { S.searchId = null; S.sel = null; nav('search'); }
}
function toggleInspector() { if (!VIEWS[S.view].insp) return; S.inspector = !S.inspector; applyChrome(); renderToolbar(); P.dirty = true; }
function applyTheme() {
  if (S.theme === 'system') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.setAttribute('data-theme', S.theme);
  store.set('theme', S.theme);
  WF.colors = null; WF.overKey = ''; P.dirty = true;
}
function keepScroll(fn) {
  const sel = '#content .scroll, #content .conv-left, #content .conv-right';
  const pos = $$(sel).map(e => e.scrollTop);
  fn();
  $$(sel).forEach((e, i) => { if (pos[i] != null) e.scrollTop = pos[i]; });
}
function viewSig() {
  const play = P.track ? P.track.rid + P.playing : '';
  switch (S.view) {
    case 'search': { const s = curSearch(); return s ? `${s.id}|${s.revealed}|${s.status}|${S.transfers.map(t => t.state + (t.state === 'active' ? Math.floor(t.progress * 20) : '')).join('')}|${S.inbox.length}|${play}` : ''; }
    case 'transfers': return S.transfers.map(t => t.state + Math.floor(t.progress * 200) + (t.queuePos || '')).join('') + S.uploads.map(u => Math.floor(u.progress * 200)).join('');
    case 'batch': { const B = S.batch; return `${B.running}|${budgetLeft()}|${B.lines.map(l => l.status + (l.approved ? 1 : 0) + (l.dl ? 1 : 0) + (l.waiting ? 1 : 0)).join('')}|${play}`; }
    case 'inbox': return S.inbox.map(i => i.id).join() + [...S.filing.values()].map(f => Math.floor(f.p * 100)).join();
    case 'converter': return Object.values(S.conv.state).map(s => Math.floor(s.p * 100)).join() + S.conv.running;
    case 'peers': return `${PEERS.reduce((a, p) => a + p.today, 0)}|${RATIO.session.down.files}|${RATIO.session.up.files}|${S.peersWindow}`;
    case 'collection': case 'history': return `${S.inbox.length}|${S.history.length}|${COLLECTION.tracks.length}|${S.history.map(h => h.where).join()}`;
    default: return '';
  }
}
let lastSig = '';
function refreshLive(force) {
  if (!S.ready) return;
  const b = $('#tbBudget'), bh = budgetHtml();
  if (b && b.innerHTML !== bh) b.innerHTML = bh;
  renderNav();
  const sub = $('#tbSub'); if (sub) sub.textContent = VIEWS[S.view].title()[1];
  const sig = `${S.view}#${viewSig()}`;
  if (!force && sig === lastSig) return;
  lastSig = sig;
  const ae = document.activeElement, typing = !!ae && /^(INPUT|TEXTAREA|SELECT)$/.test(ae.tagName) && $('#content').contains(ae);
  if (S.view === 'search') { updateResults(); return; }
  if (S.view === 'batch') {
    if (typing) { const tb = $('#batchBody'); if (tb) tb.innerHTML = batchRows(); } else keepScroll(renderBatch);
    renderInspector();
    return;
  }
  if (!typing && ['transfers', 'inbox', 'converter', 'peers', 'collection', 'history'].includes(S.view)) keepScroll(renderContent);
}

function viewControlsHtml() {
  return `<div class="tb-group tb-hide-sm" role="group" aria-label="Layout"><button class="tbtn${S.group === 'folders' ? ' on' : ''}" data-act="group" data-v="folders" aria-pressed="${S.group === 'folders'}" title="Group results by folder">${icon('folder')}</button><button class="tbtn${S.group === 'tracks' ? ' on' : ''}" data-act="group" data-v="tracks" aria-pressed="${S.group === 'tracks'}" title="Flat list of tracks">${icon('batch')}</button></div>
    <label class="tb-sort tb-hide-md"><span class="fl-l">Sort</span><select id="tbSort" data-ch="sort" aria-label="Sort results">${Object.entries(SORTS).map(([k, s]) => `<option value="${k}"${k === S.sort ? ' selected' : ''}>${s.label}</option>`).join('')}</select></label>`;
}

/* ---------- toasts & sheets ---------- */
const TOAST_ACTS = new Map();
function toast(title, body = '', o = {}) {
  const id = 'toast' + S.nextId++;
  const el = document.createElement('div');
  el.className = `toast${o.kind ? ' ' + o.kind : ''}`;
  el.id = id;
  el.setAttribute('role', 'status');
  const acts = o.actions && o.actions.length ? `<div class="acts">${o.actions.map((a, i) => `<button class="btn sm" data-act="toast-act" data-t="${id}" data-i="${i}">${esc(a.label)}</button>`).join('')}</div>` : '';
  el.innerHTML = `${icon(o.icon || (o.kind === 'warn' ? 'warn' : o.kind === 'good' ? 'check-c' : 'info'))}<div class="tx"><b>${esc(title)}</b>${body ? `<div>${body}</div>` : ''}${acts}</div><button class="rb" style="opacity:1" data-act="toast-close" data-t="${id}" aria-label="Dismiss">${icon('x')}</button>`;
  TOAST_ACTS.set(id, o.actions || []);
  const box = $('#toasts');
  box.prepend(el);
  while (box.children.length > 4) { const last = box.lastElementChild; TOAST_ACTS.delete(last.id); last.remove(); }
  setTimeout(() => dismissToast(id), o.ms || 6000);
}
function dismissToast(id) { const el = document.getElementById(id); if (el) el.remove(); TOAST_ACTS.delete(id); }
let SHEET_ACTS = [];
function sheet({ title, body, actions }) {
  SHEET_ACTS = actions && actions.length ? actions : [{ label: 'OK', primary: true }];
  const bg = $('#sheet');
  bg.innerHTML = `<div class="sheet" role="dialog" aria-modal="true" aria-labelledby="sheetTitle"><h3 id="sheetTitle">${esc(title)}</h3>${body || ''}<div class="acts">${SHEET_ACTS.map((a, i) => `<button class="btn${a.primary ? ' primary' : ''}" data-act="sheet-act" data-i="${i}">${esc(a.label)}</button>`).join('')}</div></div>`;
  bg.hidden = false;
  (bg.querySelector('.btn.primary') || bg.querySelector('.btn')).focus();
}
function closeSheet() { const bg = $('#sheet'); bg.hidden = true; bg.innerHTML = ''; }

/* ---------- selection & filters ---------- */
function select(id, { scroll = false } = {}) {
  S.sel = id;
  for (const tr of $$('#resBody tr.sel')) { tr.classList.remove('sel'); tr.setAttribute('aria-selected', 'false'); }
  const row = id ? document.querySelector(`#resBody tr[data-id="${CSS.escape(id)}"]`) : null;
  if (row) { row.classList.add('sel'); row.setAttribute('aria-selected', 'true'); if (scroll) row.scrollIntoView({ block: 'nearest' }); }
  renderInspector();
}
let autoTimer = 0;
function moveSel(d) {
  if (S.view === 'batch') {
    const L = S.batch.lines;
    if (!L.length) return;
    let i = L.findIndex(l => l.id === S.batch.sel);
    i = i < 0 ? (d > 0 ? 0 : L.length - 1) : clamp(i + d, 0, L.length - 1);
    S.batch.sel = L[i].id;
    const tb = $('#batchBody'); if (tb) tb.innerHTML = batchRows();
    const row = document.querySelector(`#batchBody tr[data-id="${L[i].id}"]`); if (row) row.scrollIntoView({ block: 'nearest' });
    renderInspector();
    if (S.autoPreview && L[i].best) { clearTimeout(autoTimer); const best = L[i].best; autoTimer = setTimeout(() => previewResult(best), 450); }
    return;
  }
  if (S.view !== 'search' || !S.rows.length) return;
  let i = S.rows.indexOf(S.sel);
  i = i < 0 ? (d > 0 ? 0 : S.rows.length - 1) : clamp(i + d, 0, S.rows.length - 1);
  select(S.rows[i], { scroll: true });
  const r = S.results.get(S.rows[i]);
  if (S.autoPreview && r) { clearTimeout(autoTimer); autoTimer = setTimeout(() => { if (S.sel === r.id) previewResult(r); }, 450); }
}
function previewSelection() {
  if (S.view === 'search') { const r = S.results.get(S.sel); if (r) previewResult(r); }
  else if (S.view === 'batch') { const l = S.batch.lines.find(x => x.id === S.batch.sel); if (l && l.best) previewResult(l.best); }
}
function toggleGroup(key) { S.collapsed.has(key) ? S.collapsed.delete(key) : S.collapsed.add(key); updateResults(); }
function setSort(k) {
  if (!SORTS[k]) return;
  if (S.sort === k) S.sortDir = -S.sortDir; else { S.sort = k; S.sortDir = SORTS[k].dir; }
  if (S.view !== 'search') return;
  const th = $('#resTable thead'); if (th) th.innerHTML = theadHtml();
  const fs = $('#tbSort'); if (fs) fs.value = S.sort;
  updateResults();
}
function filtersChanged(markDirty = true) {
  if (markDirty) S.presetDirty = true;
  if (S.view !== 'search') return;
  const fb = $('#filters');
  if (fb) { fb.innerHTML = filtersHtml(); fb.classList.toggle('more', S.moreFilters); }
  updateResults();
  renderNav();
}
const parseDur = v => { const m = String(v).trim().match(/^(\d+)(?::(\d{1,2}))?$/); return m ? +m[1] * 60 + (m[2] ? +m[2] : 0) : null; };

/* ---------- synthetic preview audio (deterministic per result) ---------- */
const PROG = [[0, 3, 7], [-4, 0, 3], [-2, 2, 5], [-5, -2, 2]]; // i – VI – VII – v
function makeSynth(seed, bpm, n) {
  const r = mulberry32(seed);
  const beatS = 60 / bpm, nBars = Math.max(16, Math.floor(n / SR / (beatS * 4)));
  const b4 = x => Math.round((nBars * x) / 4) * 4;
  return {
    sixteenth: (beatS * SR) / 4, nBars, n, noise: seed >>> 0 || 1, last16: -1,
    sec: { groove: b4(0.1), drop1: b4(0.22), brk: b4(0.47), drop2: b4(0.58), outro: b4(0.86) },
    root: 55 * Math.pow(2, Math.floor(r() * 12) / 12),
    k: { ph: 0, env: 0, f: 0 }, hat: { env: 0, open: 0, prev: 0 }, clap: { env: 0, lp: 0, lp2: 0 },
    bass: { ph: 0, env: 0, lp: 0, f: 55 }, stab: { ph: [0, 0, 0], env: 0, lp: 0, f: [0, 0, 0] },
    pad: { ph: [0, 0, 0, 0, 0, 0], lp: 0, lvl: 0, target: 0, f: [0, 0, 0, 0, 0, 0] },
  };
}
const DEC = (sec) => Math.exp(-1 / (SR * sec));
const K_DEC = DEC(0.13), K_SWEEP = DEC(0.035), H_DEC = DEC(0.018), O_DEC = DEC(0.09), C_DEC = DEC(0.11), B_DEC = DEC(0.085), S_DEC = DEC(0.14);
function synthRender(sy, out, from, to) {
  const sec = sy.sec, fadeOut = sy.n - SR * 2, k = sy.k, h = sy.hat, c = sy.clap, b = sy.bass, st = sy.stab, pd = sy.pad;
  let nz = sy.noise;
  for (let i = from; i < to; i++) {
    const p16 = Math.floor(i / sy.sixteenth), bar = Math.floor(p16 / 16), in16 = p16 % 16;
    const inBrk = bar >= sec.brk && bar < sec.drop2;
    if (p16 !== sy.last16) {
      sy.last16 = p16;
      const groove = bar >= sec.groove && !inBrk && bar < sec.outro;
      const stabOn = (bar >= sec.drop1 && bar < sec.brk) || (bar >= sec.drop2 && bar < sec.outro);
      const chord = PROG[Math.floor(bar / 2) % 4];
      if (in16 % 4 === 0 && !inBrk) { k.env = 1; k.f = 160; k.ph = 0; }
      h.env = (in16 % 2 ? 0.5 : 0.28) * (inBrk ? 0.35 : 1);
      if (in16 % 4 === 2) h.open = inBrk ? 0.2 : 0.75;
      if (groove && (in16 === 4 || in16 === 12)) c.env = 1;
      if (groove && in16 % 4 !== 0) { b.env = 1; b.f = sy.root * Math.pow(2, chord[0] / 12); }
      if (stabOn && (in16 === 3 || in16 === 6 || in16 === 10 || in16 === 14)) { st.env = 1; for (let j = 0; j < 3; j++) st.f[j] = sy.root * 4 * Math.pow(2, chord[j] / 12); }
      pd.target = inBrk ? 0.2 : stabOn ? 0.05 : 0;
      for (let j = 0; j < 6; j++) pd.f[j] = sy.root * 2 * Math.pow(2, chord[j % 3] / 12) * (j < 3 ? 1.003 : 0.997);
    }
    nz = (Math.imul(nz, 1664525) + 1013904223) >>> 0;
    const n0 = nz / 2147483648 - 1, hp = n0 - h.prev;
    h.prev = n0;
    let s = 0;
    if (k.env > 1e-4) { k.f = 48 + (k.f - 48) * K_SWEEP; k.ph += k.f / SR; if (k.ph >= 1) k.ph -= 1; s += Math.sin(k.ph * 6.283185307) * k.env * 0.95; k.env *= K_DEC; }
    if (h.env > 1e-4 || h.open > 1e-4) { s += hp * (h.env * 0.16 + h.open * 0.1); h.env *= H_DEC; h.open *= O_DEC; }
    if (c.env > 1e-4) { c.lp += 0.28 * (n0 - c.lp); c.lp2 += 0.05 * (c.lp - c.lp2); s += (c.lp - c.lp2) * c.env * 0.9; c.env *= C_DEC; }
    if (b.env > 1e-4) { b.ph += b.f / SR; if (b.ph >= 1) b.ph -= 1; b.lp += 0.095 * (2 * b.ph - 1 - b.lp); s += b.lp * b.env * 0.5; b.env *= B_DEC; }
    if (st.env > 1e-4) {
      let v = 0;
      for (let j = 0; j < 3; j++) { st.ph[j] += st.f[j] / SR; if (st.ph[j] >= 1) st.ph[j] -= 1; v += 2 * st.ph[j] - 1; }
      st.lp += 0.25 * (v / 3 - st.lp); s += st.lp * st.env * 0.42; st.env *= S_DEC;
    }
    pd.lvl += (pd.target - pd.lvl) * 0.00004;
    if (pd.lvl > 1e-3) {
      let v = 0;
      for (let j = 0; j < 6; j++) { pd.ph[j] += pd.f[j] / SR; if (pd.ph[j] >= 1) pd.ph[j] -= 1; v += 2 * pd.ph[j] - 1; }
      pd.lp += 0.07 * (v / 6 - pd.lp); s += pd.lp * pd.lvl * 2.2;
    }
    if (bar >= sec.drop2 - 4 && bar < sec.drop2) { const x = (i / sy.sixteenth / 16 - (sec.drop2 - 4)) / 4; s += hp * x * x * 0.35; }
    let g = 0.9;
    if (i < SR * 0.5) g *= i / (SR * 0.5);
    if (i > fadeOut) g *= Math.max(0, (sy.n - i) / (SR * 2));
    out[i] = Math.tanh(s * g);
  }
  sy.noise = nz;
}

/* ---------- 3-band analysis: Linkwitz-Riley-style crossovers at 200 Hz and 2.5 kHz ---------- */
function biquad(type, fc) {
  const w = (2 * Math.PI * fc) / SR, cs = Math.cos(w), al = Math.sin(w) / (2 * Math.SQRT1_2), a0 = 1 + al;
  const lp = type === 'lp';
  return { b0: (lp ? (1 - cs) / 2 : (1 + cs) / 2) / a0, b1: (lp ? 1 - cs : -(1 + cs)) / a0, b2: (lp ? (1 - cs) / 2 : (1 + cs) / 2) / a0, a1: (-2 * cs) / a0, a2: (1 - al) / a0, x1: 0, x2: 0, y1: 0, y2: 0 };
}
function bq(f, x) { const y = f.b0 * x + f.b1 * f.x1 + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2; f.x2 = f.x1; f.x1 = x; f.y2 = f.y1; f.y1 = y; return y; }
const makeAnalyzer = () => ({ l: [biquad('lp', 200), biquad('lp', 200)], m: [biquad('hp', 200), biquad('hp', 200), biquad('lp', 2500), biquad('lp', 2500)], h: [biquad('hp', 2500), biquad('hp', 2500)], pl: 0, pm: 0, sh: 0, cnt: 0, bi: 0 });
function analyze(tk, from, to) {
  const an = tk.an, a = tk.audio, hop = tk.hop;
  for (let i = from; i < to; i++) {
    const x = a[i];
    let l = bq(an.l[1], bq(an.l[0], x)), m = bq(an.m[3], bq(an.m[2], bq(an.m[1], bq(an.m[0], x)))), h = bq(an.h[1], bq(an.h[0], x));
    if (l < 0) l = -l; if (m < 0) m = -m; if (h < 0) h = -h;
    if (l > an.pl) an.pl = l; if (m > an.pm) an.pm = m; an.sh += h * h;
    if (++an.cnt === hop) {
      const bi = an.bi++;
      if (bi < tk.nb) {
        const hr = Math.sqrt(an.sh / hop); // highs as RMS: steadier than noise peaks
        tk.low[bi] = an.pl; tk.mid[bi] = an.pm; tk.high[bi] = hr;
        if (an.pl > tk.maxL) tk.maxL = an.pl; if (an.pm > tk.maxM) tk.maxM = an.pm; if (hr > tk.maxH) tk.maxH = hr;
        if (an.bi > tk.analyzed) tk.analyzed = an.bi;
      }
      an.pl = an.pm = an.sh = 0; an.cnt = 0;
    }
  }
}
function makeTrack(r) {
  const n = Math.floor(r.dur * SR), hop = Math.round(SR / 150), nb = Math.ceil(n / hop);
  return { rid: r.id, r, n, dur: r.dur, bpm: r.rel.bpm || 124, hop, nb, audio: null, synth: null, an: null, gen: 0, low: new Float32Array(nb), mid: new Float32Array(nb), high: new Float32Array(nb), analyzed: 0, maxL: 1e-6, maxM: 1e-6, maxH: 1e-6, tr: null, complete: false, kept: false };
}
const trackFraction = tk => (tk.complete ? 1 : tk.tr ? tk.tr.progress : 0);
function pumpAudio(budgetMs) {
  const tk = P.track;
  if (!tk) return;
  const target = Math.floor(trackFraction(tk) * tk.n);
  if (tk.gen >= target) return;
  if (!tk.audio) { tk.audio = new Float32Array(tk.n); tk.synth = makeSynth(hashStr(tk.rid + tk.r.title), tk.bpm, tk.n); tk.an = makeAnalyzer(); tk.gen = 0; }
  const t0 = performance.now();
  while (tk.gen < target && performance.now() - t0 < budgetMs) {
    const end = Math.min(target, tk.gen + 8192);
    synthRender(tk.synth, tk.audio, tk.gen, end);
    analyze(tk, tk.gen, end);
    tk.gen = end;
  }
  P.dirty = true;
}
function evictAudio() {
  let withAudio = [...PREVIEW_CACHE.values()].filter(t => t.audio);
  while (withAudio.length > MAX_AUDIO_TRACKS) {
    const old = withAudio.shift();
    if (old === P.track) continue;
    old.audio = null; old.synth = null; old.an = null; old.gen = 0; // keep the waveform bins; audio regenerates on demand
  }
}

/* ---------- Web Audio playback with seamless chaining ---------- */
const AE = {
  ctx: null, gain: null, srcs: [], t0: 0, p0: 0, end: 0,
  ensure() {
    if (!this.ctx) {
      const C = window.AudioContext || window.webkitAudioContext;
      if (!C) return false;
      this.ctx = new C();
      this.gain = this.ctx.createGain();
      this.gain.gain.value = P.volume;
      this.gain.connect(this.ctx.destination);
    }
    if (this.ctx.state === 'suspended') this.ctx.resume();
    return true;
  },
  start(tk, pos) {
    this.stop();
    if (!this.ensure() || !tk.audio) return false;
    const s0 = Math.floor(pos * SR), s1 = tk.gen;
    if (s1 - s0 < SR * 0.3) return false;
    this.t0 = this.ctx.currentTime + 0.03; this.p0 = pos; this.end = pos;
    this.append(tk, s0, s1, this.t0);
    return true;
  },
  append(tk, s0, s1, when) {
    const buf = this.ctx.createBuffer(1, s1 - s0, SR);
    buf.copyToChannel(tk.audio.subarray(s0, s1), 0);
    const src = this.ctx.createBufferSource();
    src.buffer = buf; src.connect(this.gain); src.start(when);
    src.onended = () => { this.srcs = this.srcs.filter(x => x !== src); };
    this.srcs.push(src);
    this.end = s1 / SR;
  },
  extend(tk) {
    if (!this.srcs.length || !tk.audio) return;
    const have = Math.round(this.end * SR);
    if (tk.gen <= have || (tk.gen - have < SR * 2 && tk.gen < tk.n)) return;
    const when = this.t0 + (this.end - this.p0);
    if (when < this.ctx.currentTime + 0.05) return;
    this.append(tk, have, tk.gen, when);
  },
  pos() { return this.ctx ? Math.min(this.end, this.p0 + Math.max(0, this.ctx.currentTime - this.t0)) : P.pos; },
  stop() { for (const s of this.srcs) { s.onended = null; try { s.stop(); } catch (e) { /* already stopped */ } s.disconnect(); } this.srcs = []; },
};
const curPos = () => (P.playing && AE.srcs.length ? AE.pos() : P.pos);
function play() {
  const tk = P.track;
  if (!tk) return;
  if (!AE.ensure()) { toast('Audio playback is not available in this browser', '', { kind: 'warn' }); return; }
  P.playing = true;
  if (P.pos >= tk.dur - 0.1) P.pos = 0;
  P.waiting = !AE.start(tk, P.pos);
  renderPlayer(); markPlayingRow();
}
function pause() {
  if (P.playing && AE.srcs.length) P.pos = AE.pos();
  AE.stop(); P.playing = false; P.waiting = false;
  renderPlayer(); markPlayingRow();
}
function togglePlay() { if (!P.track) { previewSelection(); return; } if (P.playing) pause(); else play(); }
function seekTo(t) {
  const tk = P.track;
  if (!tk) return;
  P.pos = clamp(t, 0, tk.dur - 0.05);
  if (P.playing) { AE.stop(); P.waiting = !AE.start(tk, P.pos); }
  P.dirty = true;
  renderPlayerLite();
}
const seekRel = d => { if (P.track) seekTo(curPos() + d); };
function updatePlayback() {
  const tk = P.track;
  if (!tk || !P.playing) return;
  if (P.waiting) { if (tk.gen >= Math.min(tk.n, Math.floor(P.pos * SR) + SR)) P.waiting = !AE.start(tk, P.pos); return; }
  const pos = AE.pos();
  P.pos = pos;
  if (pos >= tk.dur - 0.06) { AE.stop(); P.playing = false; P.pos = 0; renderPlayer(); markPlayingRow(); return; }
  if (AE.end - pos < 2) AE.extend(tk);
  if (pos >= AE.end - 0.02 && tk.gen < tk.n) { AE.stop(); P.pos = AE.end; P.waiting = true; }
}

/* ---------- preview lifecycle ---------- */
function previewResult(r, { autoplay = true } = {}) {
  if (!r) return;
  if (P.track && P.track.rid === r.id) { togglePlay(); return; }
  const prev = P.track;
  if (prev && prev.tr && prev.tr.kind === 'preview' && !['done', 'failed', 'cancelled'].includes(prev.tr.state) && !prev.tr.keep && S.cancelOnSkip) {
    prev.tr.state = 'cancelled';
    PREVIEW_CACHE.delete(prev.rid);
    if (!S.warnedCancel) { S.warnedCancel = true; toast('Stopped the previous preview', 'Skipping a preview cancels its transfer to save the peer’s bandwidth. You can change this in Settings → Preview.', { icon: 'info', ms: 8000 }); }
  }
  AE.stop(); P.playing = false; P.waiting = false; P.pos = 0;
  let tk = PREVIEW_CACHE.get(r.id);
  if (!tk) {
    if (S.netPaused) { toast('Network activity is paused', 'Resume it from the toolbar to preview new results.', { kind: 'warn' }); return; }
    tk = makeTrack(r);
    tk.tr = addDownload(r, { quiet: true, preview: true });
  }
  if (autoplay) AE.ensure();
  PREVIEW_CACHE.delete(r.id); PREVIEW_CACHE.set(r.id, tk);
  evictAudio();
  P.track = tk; P.autoplay = autoplay; P.dirty = true; WF.overKey = '';
  renderPlayer(); markPlayingRow();
}
function markPlayingRow() {
  if (S.view === 'search') updateResults();
  else if (S.view === 'batch') { const tb = $('#batchBody'); if (tb) tb.innerHTML = batchRows(); }
}
const trackName = tk => `${tk.r.artist} — ${fullTitle(tk.r.title, tk.r.version)}`;
function keepPreview() {
  const tk = P.track;
  if (!tk || tk.kept) return;
  const t = tk.tr;
  if (t && !['done', 'cancelled', 'failed'].includes(t.state)) { t.keep = true; tk.kept = true; toast('Kept', `${esc(trackName(tk))} goes to your Inbox when its download finishes.`, { kind: 'good', icon: 'tray' }); }
  else if (tk.complete) { inboxFromResult(tk.r, tk.r.peer); tk.kept = true; toast('Kept: moved to your Inbox', `${esc(trackName(tk))} → <span class="mono">Inbox/Singles/</span>. No second download needed.`, { kind: 'good', icon: 'tray' }); }
  else { toast('This preview was cancelled', 'Preview it again, or download it.', { kind: 'warn' }); return; }
  renderPlayer(); refreshLive(true);
}
function discardPreview() {
  const tk = P.track;
  if (!tk) return;
  if (tk.tr && !['done', 'failed', 'cancelled'].includes(tk.tr.state) && !tk.tr.keep) tk.tr.state = 'cancelled';
  PREVIEW_CACHE.delete(tk.rid);
  AE.stop();
  P.track = null; P.playing = false; P.waiting = false; P.pos = 0; P.dirty = true; WF.overKey = '';
  toast('Preview discarded', 'Removed from the preview cache.', { icon: 'trash', ms: 3500 });
  renderPlayer(); markPlayingRow(); refreshLive(true);
}

/* ---------- player bar ---------- */
function playerStatus(tk) {
  const t = tk.tr;
  if (P.playing && P.waiting) return 'Buffering…';
  if (tk.kept && (tk.complete || !t || t.state === 'done')) return 'Kept · in your Inbox';
  if (tk.complete || !t) return 'In preview cache';
  switch (t.state) {
    case 'waiting': return 'Waiting for a download slot';
    case 'queued': return `Queued at peer · #${t.queuePos}`;
    case 'connecting': return 'Connecting…';
    case 'active': return `Downloading ${Math.floor(t.progress * 100)} % · ${fmtSpeed(t.speed)}`;
    case 'cancelled': return 'Transfer cancelled';
    case 'failed': return t.reason || 'Failed';
    default: return 'In preview cache';
  }
}
function renderPlayer() {
  const tk = P.track, btn = $('#plPlay');
  for (const b of $$('#wfModes button')) b.classList.toggle('on', b.dataset.v === P.mode);
  const ab = $('#autoBtn');
  if (ab) { ab.classList.toggle('on', S.autoPreview); ab.setAttribute('aria-pressed', String(S.autoPreview)); }
  if (!tk) {
    $('#plArt').style.background = 'var(--fill-2)';
    $('#plTitle').textContent = 'Nothing playing';
    $('#plSub').textContent = 'Select a result and press Space to preview it';
    $('#plTime').innerHTML = '<b>0:00</b> / 0:00';
    $('#plActions').innerHTML = '';
    $('#scrStatus').textContent = 'No preview loaded';
    btn.innerHTML = icon('play'); btn.setAttribute('aria-label', 'Play');
    return;
  }
  const r = tk.r;
  $('#plArt').style.background = artGradient(hashStr(r.artist + r.rel.album));
  $('#plTitle').textContent = trackName(tk);
  $('#plActions').innerHTML = `<button class="btn${tk.kept ? '' : ' primary'}" data-act="keep" title="Move this preview to your Inbox (K)"${tk.kept ? ' disabled' : ''}>${icon('tray')} ${tk.kept ? 'Kept' : 'Keep'}</button><button class="btn" data-act="discard" title="Discard this preview (⌫)" aria-label="Discard preview">${icon('trash')}</button>`;
  btn.innerHTML = icon(P.playing ? 'pause' : 'play');
  btn.setAttribute('aria-label', P.playing ? 'Pause' : 'Play');
  renderPlayerLite();
}
function renderPlayerLite() {
  const tk = P.track;
  if (!tk) return;
  const st = playerStatus(tk);
  $('#plTime').innerHTML = `<b>${fmtDur(curPos())}</b> / ${fmtDur(tk.dur)}`;
  $('#plSub').textContent = `${qualLong(tk.r)} · from ${tk.r.peer.name} · ${st}`;
  $('#scrStatus').innerHTML = `Preview · ${esc(qualLong(tk.r))} · ${esc(tk.r.peer.name)} · <b>${esc(st)}</b> · ${tk.bpm} BPM`;
}

/* ---------- waveform rendering ---------- */
const WF = { colors: null, off: null, overKey: '' };
function wfColors() {
  if (!WF.colors) {
    const cs = getComputedStyle(document.documentElement), g = (n, d) => cs.getPropertyValue(n).trim() || d;
    WF.colors = { low: g('--wf-low', '#0055e1'), mid: g('--wf-mid', '#ffa600'), overlap: g('--wf-overlap', '#b36607'), high: g('--wf-high', '#ffffff'), bg: g('--screen', '#0b0c0f') };
  }
  return WF.colors;
}
function sizeCanvas(cv) {
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  const w = Math.max(1, Math.round(cv.clientWidth * dpr)), h = Math.max(1, Math.round(cv.clientHeight * dpr));
  if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
  return w / Math.max(1, cv.clientWidth);
}
function columns(tk, t0, t1, W) {
  const bps = SR / tk.hop, dt = (t1 - t0) / W, nA = tk.analyzed;
  const L = new Float32Array(W), M = new Float32Array(W), H = new Float32Array(W);
  const kL = 1 / tk.maxL, kM = 1 / tk.maxM, kH = 1 / tk.maxH;
  for (let x = 0; x < W; x++) {
    const ta = t0 + x * dt;
    const b0 = Math.floor(ta * bps);
    let b1 = Math.floor((ta + dt) * bps);
    if (b1 <= b0) b1 = b0 + 1;
    if (b0 < 0 || b0 >= nA) { L[x] = -1; continue; }
    if (b1 > nA) b1 = nA;
    let l = 0, m = 0, h = 0;
    for (let b = b0; b < b1; b++) { if (tk.low[b] > l) l = tk.low[b]; if (tk.mid[b] > m) m = tk.mid[b]; if (tk.high[b] > h) h = tk.high[b]; }
    L[x] = Math.min(1, l * kL); M[x] = Math.pow(Math.min(1, m * kM), 0.85); H[x] = Math.pow(Math.min(1, h * kH), 0.8);
  }
  return { L, M, H };
}
function drawBands(ctx, cols, W, H, view) {
  const c = wfColors(), L = cols.L, M = cols.M, Hi = cols.H, mid = H / 2, half = H / 2 - 1;
  if (P.mode === 'pioneer') {
    if (view === 'over') {
      // Overview: one-sided cumulative stack from the bottom: blue, amber on top, white crest.
      const s = (H - 2) / 1.06;
      for (const [color, wl, wm, wh] of [[c.high, 0.5, 0.32, 0.24], [c.mid, 0.5, 0.32, 0], [c.low, 0.5, 0, 0]]) {
        ctx.fillStyle = color;
        for (let x = 0; x < W; x++) { if (L[x] < 0) continue; const v = (L[x] * wl + M[x] * wm + Hi[x] * wh) * s; if (v > 0.25) ctx.fillRect(x, H - v, 1, v); }
      }
      return;
    }
    // Zoomed: mirrored; brown where lows and mids overlap, the larger band beyond it in its own colour, white highs on top.
    const lo = x => L[x] * 0.96 * half, mi = x => M[x] * 0.85 * half;
    ctx.fillStyle = c.overlap;
    for (let x = 0; x < W; x++) { if (L[x] < 0) continue; const o = Math.min(lo(x), mi(x)); if (o > 0.25) ctx.fillRect(x, mid - o, 1, o * 2); }
    for (const [color, mine, other] of [[c.low, lo, mi], [c.mid, mi, lo]]) {
      ctx.fillStyle = color;
      for (let x = 0; x < W; x++) {
        if (L[x] < 0) continue;
        const a = mine(x), o = Math.min(a, other(x));
        if (a - o > 0.25) { ctx.fillRect(x, mid - a, 1, a - o); ctx.fillRect(x, mid + o, 1, a - o); }
      }
    }
    ctx.globalAlpha = 0.85; ctx.fillStyle = c.high;
    for (let x = 0; x < W; x++) { if (L[x] < 0) continue; const v = Hi[x] * 0.5 * half; if (v > 0.25) ctx.fillRect(x, mid - v, 1, v * 2); }
    ctx.globalAlpha = 1;
    return;
  }
  if (P.mode === 'lanes') {
    const lh = H / 3, hh = lh / 2 - 1;
    [[c.high, Hi], [c.mid, M], [c.low, L]].forEach(([color, arr], i) => {
      const cy = lh * i + lh / 2;
      ctx.fillStyle = color;
      for (let x = 0; x < W; x++) { if (L[x] < 0) continue; const v = arr[x] * hh; if (v > 0.25) ctx.fillRect(x, cy - v, 1, v * 2); }
    });
    return;
  }
  const stacked = P.mode === 'stacked';
  // Layered: lows behind, highs in front, each from the centre. Stacked: lows at the centre, highs outermost.
  const layers = stacked ? [[c.high, 0.5, 0.32, 0.24], [c.mid, 0.5, 0.32, 0], [c.low, 0.5, 0, 0]] : [[c.low, 0.96, 0, 0], [c.mid, 0, 0.72, 0], [c.high, 0, 0, 0.48]];
  const sc = stacked ? half / 1.06 : half;
  for (const [color, wl, wm, wh] of layers) {
    ctx.fillStyle = color;
    for (let x = 0; x < W; x++) {
      if (L[x] < 0) continue;
      const v = (L[x] * wl + M[x] * wm + Hi[x] * wh) * sc;
      if (v > 0.25) ctx.fillRect(x, mid - v, 1, v * 2);
    }
  }
}
function hatch(ctx, x0, x1, H, dpr) {
  if (x1 <= x0) return;
  ctx.save();
  ctx.beginPath(); ctx.rect(x0, 0, x1 - x0, H); ctx.clip();
  ctx.strokeStyle = 'rgba(255,255,255,0.10)'; ctx.lineWidth = dpr;
  ctx.beginPath();
  for (let x = x0 - H; x < x1; x += 7 * dpr) { ctx.moveTo(x, H); ctx.lineTo(x + H, 0); }
  ctx.stroke();
  ctx.fillStyle = 'rgba(255,255,255,0.2)'; ctx.fillRect(x0, H / 2 - dpr / 2, x1 - x0, dpr);
  ctx.restore();
}
function drawZoom() {
  const cv = $('#cvZoom'), dpr = sizeCanvas(cv), ctx = cv.getContext('2d'), W = cv.width, H = cv.height, c = wfColors(), tk = P.track;
  ctx.fillStyle = c.bg; ctx.fillRect(0, 0, W, H);
  if (!tk) {
    ctx.fillStyle = 'rgba(255,255,255,.4)'; ctx.font = `${11 * dpr}px -apple-system, BlinkMacSystemFont, sans-serif`; ctx.textAlign = 'center';
    ctx.fillText('Select a result and press Space to preview it', W / 2, H / 2 + 4 * dpr);
    return;
  }
  const pos = curPos(), z = P.zoomSec, t0 = pos - z / 2, t1 = pos + z / 2, beat = 60 / tk.bpm;
  for (let bt = Math.ceil(Math.max(0, t0) / beat); bt * beat < Math.min(t1, tk.dur); bt++) {
    const bar = bt % 4 === 0;
    ctx.fillStyle = bar ? 'rgba(255,255,255,0.16)' : 'rgba(255,255,255,0.06)';
    ctx.fillRect(Math.round(((bt * beat - t0) / z) * W), 0, Math.max(1, Math.round(dpr * (bar ? 1 : 0.6))), H);
  }
  drawBands(ctx, columns(tk, t0, t1, W), W, H, 'zoom');
  const dl = trackFraction(tk) * tk.dur;
  if (dl < Math.min(t1, tk.dur)) hatch(ctx, Math.max(0, ((dl - t0) / z) * W), Math.min(W, ((tk.dur - t0) / z) * W), H, dpr);
  const cx = Math.round(W / 2);
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(cx - dpr, 0, 2 * dpr, H);
  ctx.beginPath(); ctx.moveTo(cx - 5 * dpr, 0); ctx.lineTo(cx + 5 * dpr, 0); ctx.lineTo(cx, 6 * dpr); ctx.closePath(); ctx.fill();
  ctx.beginPath(); ctx.moveTo(cx - 5 * dpr, H); ctx.lineTo(cx + 5 * dpr, H); ctx.lineTo(cx, H - 6 * dpr); ctx.closePath(); ctx.fill();
  ctx.font = `600 ${10 * dpr}px ui-monospace, SFMono-Regular, Menlo, monospace`;
  ctx.textAlign = 'left';
  const label = (text, x, right) => {
    const w = ctx.measureText(text).width + 8 * dpr, x0 = right ? x - w : x;
    ctx.fillStyle = 'rgba(0,0,0,0.62)'; ctx.fillRect(x0, 3 * dpr, w, 14 * dpr);
    ctx.fillStyle = 'rgba(255,255,255,0.92)'; ctx.fillText(text, x0 + 4 * dpr, 13.5 * dpr);
  };
  label(fmtClock(pos), 4 * dpr, false);
  label(`-${fmtClock(Math.max(0, tk.dur - pos))}`, W - 4 * dpr, true);
}
function drawOverview() {
  const cv = $('#cvOver'), dpr = sizeCanvas(cv), ctx = cv.getContext('2d'), W = cv.width, H = cv.height, c = wfColors(), tk = P.track;
  if (!WF.off) WF.off = document.createElement('canvas');
  const key = tk ? `${tk.rid}|${W}|${H}|${P.mode}|${tk.analyzed}|${c.bg}` : `none|${W}|${H}|${c.bg}`;
  if (WF.overKey !== key) {
    WF.overKey = key; WF.off.width = W; WF.off.height = H;
    const o = WF.off.getContext('2d');
    o.fillStyle = c.bg; o.fillRect(0, 0, W, H);
    if (tk) drawBands(o, columns(tk, 0, tk.dur, W), W, H, 'over');
  }
  ctx.drawImage(WF.off, 0, 0);
  if (!tk) return;
  const px = (curPos() / tk.dur) * W;
  ctx.fillStyle = 'rgba(0,0,0,0.42)'; ctx.fillRect(0, 0, px, H);
  const f = trackFraction(tk);
  if (f < 1) {
    hatch(ctx, f * W, W, H, dpr);
    ctx.fillStyle = 'rgba(255,255,255,.75)'; ctx.font = `600 ${10 * dpr}px -apple-system, BlinkMacSystemFont, sans-serif`; ctx.textAlign = 'right';
    ctx.fillText(`${Math.floor(f * 100)} %`, W - 5 * dpr, H / 2 + 3.5 * dpr);
  }
  ctx.fillStyle = 'rgba(255,255,255,.28)';
  for (let m = 60; m < tk.dur; m += 60) ctx.fillRect(Math.round((m / tk.dur) * W), H - 4 * dpr, dpr, 4 * dpr);
  ctx.fillStyle = '#ffffff'; ctx.fillRect(Math.round(px) - dpr, 0, 2 * dpr, H);
  if (P.hoverX != null) { ctx.fillStyle = 'rgba(255,255,255,.5)'; ctx.fillRect(Math.round(P.hoverX * dpr), 0, dpr, H); }
}
function drawScreen() {
  const tk = P.track;
  const busy = tk && (P.playing || tk.gen < Math.floor(trackFraction(tk) * tk.n) || (tk.tr && tk.tr.state === 'active'));
  if (!P.dirty && !busy && !P.drag) return;
  drawZoom(); drawOverview();
  P.dirty = false;
}
function waveformEvents() {
  const ov = $('#cvOver'), zv = $('#cvZoom'), tip = $('#scrTip'), screen = $('#screen');
  const timeAt = e => { const r = ov.getBoundingClientRect(); return { t: clamp((e.clientX - r.left) / r.width, 0, 1) * (P.track ? P.track.dur : 0), x: e.clientX - r.left }; };
  ov.addEventListener('pointerdown', e => { if (!P.track) return; ov.setPointerCapture(e.pointerId); P.scrub = { was: P.playing }; if (P.playing) pause(); P.pos = timeAt(e).t; P.dirty = true; renderPlayerLite(); });
  ov.addEventListener('pointermove', e => {
    if (!P.track) return;
    const a = timeAt(e), sr = screen.getBoundingClientRect(), or = ov.getBoundingClientRect();
    P.hoverX = a.x; tip.hidden = false; tip.textContent = fmtDur(a.t);
    tip.style.left = `${e.clientX - sr.left}px`; tip.style.top = `${or.top - sr.top - 4}px`;
    if (P.scrub) { P.pos = a.t; renderPlayerLite(); }
    P.dirty = true;
  });
  const endScrub = () => { if (!P.scrub) return; const was = P.scrub.was; P.scrub = null; if (was) play(); };
  ov.addEventListener('pointerup', endScrub); ov.addEventListener('pointercancel', endScrub);
  ov.addEventListener('pointerleave', () => { P.hoverX = null; tip.hidden = true; P.dirty = true; });
  zv.addEventListener('pointerdown', e => { if (!P.track) return; zv.setPointerCapture(e.pointerId); P.drag = { x: e.clientX, pos: curPos(), was: P.playing }; if (P.playing) pause(); });
  zv.addEventListener('pointermove', e => {
    if (!P.drag) return;
    const w = zv.getBoundingClientRect().width;
    P.pos = clamp(P.drag.pos - ((e.clientX - P.drag.x) * P.zoomSec) / w, 0, P.track.dur - 0.05);
    P.dirty = true; renderPlayerLite();
  });
  const endDrag = () => { if (!P.drag) return; const was = P.drag.was; P.drag = null; if (was) play(); };
  zv.addEventListener('pointerup', endDrag); zv.addEventListener('pointercancel', endDrag);
  new ResizeObserver(() => { P.dirty = true; }).observe(screen);
  $('#vol').addEventListener('input', e => { P.volume = +e.target.value; if (AE.gain) AE.gain.gain.value = P.volume; });
}

/* ---------- batch & converter actions ---------- */
function batchDownload() {
  const B = S.batch, used = new Set();
  let n = 0;
  for (const l of B.lines) {
    if (!l.approved || l.dl || !l.best) continue;
    let pick = l.best;
    if (B.spread && used.has(pick.peer.name)) pick = [l.best, ...l.alts].find(r => !used.has(r.peer.name)) || l.best;
    used.add(pick.peer.name);
    requestDownload(pick, { force: true, quiet: true });
    l.dl = true; n++;
  }
  if (n) toast(`Downloading ${plural(n, 'track')}`, `${B.spread ? `Spread across ${plural(used.size, 'peer')}. ` : ''}Each goes to <span class="mono">Inbox/Singles/</span> when it finishes.`, { kind: 'good', icon: 'download' });
  renderBatch(); renderNav();
}
function convRun() {
  const C = S.conv, tg = TARGETS[C.target], skipped = [];
  C.state = {};
  for (const f of CONV_FILES) {
    const v = convVerdict(f.src, tg);
    if (v.confirm && !C.confirmed.has(f.id)) { skipped.push(f.name); continue; }
    C.state[f.id] = { p: 0 };
  }
  if (!Object.keys(C.state).length) { toast('Nothing to convert', 'Tick “Convert anyway” on the files with a warning, or pick another format.', { kind: 'warn' }); renderConverter(); return; }
  C.running = true;
  if (skipped.length) toast(`Skipping ${plural(skipped.length, 'file')}`, `${skipped.map(esc).join(', ')}: the conversion can't improve quality. Tick “Convert anyway” to include them.`, { kind: 'warn', ms: 9000 });
  renderConverter();
}

/* ---------- event wiring ---------- */
const ACT = {
  nav: el => nav(el.dataset.v),
  'close-search': (el, e) => { e.stopPropagation(); closeSearch(el.dataset.v); },
  sel: el => select(el.dataset.id),
  preview: (el, e) => { e.stopPropagation(); previewResult(S.results.get(el.dataset.id)); },
  download: (el, e) => { e.stopPropagation(); const r = S.results.get(el.dataset.id); if (r) requestDownload(r); },
  'dl-folder': (el, e) => { e.stopPropagation(); confirmAlbum(el.dataset.key); },
  'dl-alt': el => { closeSheet(); const r = S.results.get(el.dataset.id); if (r) requestDownload(r, { force: true }); },
  'toggle-group': (el, e) => { e.stopPropagation(); toggleGroup(el.dataset.key); },
  sort: el => setSort(el.dataset.v),
  fmt: el => { const f = S.filters.formats; if (f.has(el.dataset.v)) f.delete(el.dataset.v); else f.add(el.dataset.v); filtersChanged(); },
  rate: el => { const f = S.filters.rates, v = +el.dataset.v; if (f.has(v)) f.delete(v); else f.add(v); filtersChanged(); },
  owned: el => { S.filters.owned = el.dataset.v; filtersChanged(); },
  group: el => { S.group = el.dataset.v; filtersChanged(false); renderToolbar(); },
  'more-filters': () => { S.moreFilters = !S.moreFilters; filtersChanged(false); },
  'refresh-search': () => { const s = curSearch(); if (s) startSearch(s.query, { refresh: true }); },
  'toggle-sidebar': () => { S.sidebar = !S.sidebar; applyChrome(); P.dirty = true; },
  'toggle-inspector': () => toggleInspector(),
  'toggle-pause': () => {
    S.netPaused = !S.netPaused;
    toast(S.netPaused ? 'Network activity paused' : 'Network activity resumed', S.netPaused ? 'No new searches or transfers start until you resume.' : 'Searches and transfers continue.', { icon: S.netPaused ? 'pause-c' : 'play-c', ms: 3500 });
    renderToolbar(); renderFoot(); if (S.view === 'settings') renderSettings();
  },
  'cycle-theme': () => {
    S.theme = { system: 'light', light: 'dark', dark: 'system' }[S.theme] || 'system';
    applyTheme(); renderToolbar();
    toast('Appearance', S.theme === 'system' ? 'Follows the macOS setting.' : `Always ${S.theme}.`, { icon: 'appearance', ms: 2500 });
  },
  'set-theme': el => { S.theme = el.dataset.v; applyTheme(); renderSettings(); renderToolbar(); },
  'set-server': el => {
    S.server = el.dataset.v; renderFoot(); renderSettings();
    toast(S.server === 'local' ? 'Using the local test server' : 'Using the Soulseek network', S.server === 'local' ? 'Debug builds use this by default. Nothing leaves your machine.' : 'Release builds connect here.', { icon: 'shield' });
  },
  'budget-info': () => toast(`${budgetLeft()} of ${LIMITS.searches} searches left`, `${budgetLeft() < LIMITS.searches ? `The next one frees up in ${Math.ceil(budgetRefillIn())} s. ` : ''}Opening a search you ran in the last 24 hours uses the cache and costs nothing.`, { icon: 'gauge' }),
  'browse-peer': el => toast(`Browse ${el.dataset.v}`, 'Browsing a user’s shares is part of v1. The mock doesn’t simulate it yet.', { icon: 'folder' }),
  'add-wishlist': el => {
    const q = (el.dataset.v || '').trim();
    if (!q) { toast('Wishlist', 'Run a search, then use “Add to wishlist” in the inspector.', { icon: 'star' }); return; }
    if (!S.wishlist.some(w => norm(w.q) === norm(q))) S.wishlist.unshift({ q: q.toLowerCase(), preset: S.presetId, last: 'not yet', found: 0 });
    toast('Added to your wishlist', `“${esc(q)}” runs in the background at the interval the server sets.`, { kind: 'good', icon: 'star' });
    renderNav(); if (S.view === 'wishlist') renderWishlist();
  },
  'wish-open': el => startSearch(el.dataset.v),
  'inbox-file': el => fileItems([el.dataset.id]),
  'inbox-file-sel': () => fileItems([...S.inboxSel]),
  'inbox-trash': () => {
    const items = S.inbox.filter(i => S.inboxSel.has(i.id));
    if (!items.length) return;
    S.inbox = S.inbox.filter(i => !S.inboxSel.has(i.id)); S.inboxSel.clear();
    renderInbox(); renderNav();
    toast(`Moved ${plural(items.length, 'item')} to the Trash`, '', { icon: 'trash', ms: 9000, actions: [{ label: 'Undo', run: () => { S.inbox.push(...items); renderContent(); renderNav(); } }] });
  },
  'batch-sel': el => { S.batch.sel = el.dataset.id; const tb = $('#batchBody'); if (tb) tb.innerHTML = batchRows(); renderInspector(); },
  'batch-start': () => {
    const B = S.batch;
    if (!B.lines.length || B.parsedText !== B.text) { batchParse(); B.parsedText = B.text; }
    B.running = true; B.started = true; B.nextAt = Clock.t; Clock.speed = B.demo ? 10 : 1;
    renderBatch(); renderNav();
  },
  'batch-pause': () => { S.batch.running = false; Clock.speed = 1; renderBatch(); renderNav(); },
  'batch-reset': () => { batchParse(); S.batch.started = false; S.batch.parsedText = S.batch.text; renderBatch(); renderInspector(); renderToolbar(); },
  'batch-approve-all': () => { for (const l of S.batch.lines) if (l.status === 'match' && !l.dl) l.approved = true; renderBatch(); },
  'batch-download': () => batchDownload(),
  'batch-pick': el => {
    const B = S.batch, l = B.lines.find(x => x.id === B.sel), r = S.results.get(el.dataset.id);
    if (!l || !r) return;
    l.alts = [l.best, ...l.alts.filter(x => x !== r)]; l.best = r; l.status = 'match'; l.approved = true;
    renderBatch(); renderInspector();
  },
  'tr-tab': el => { S.transfersTab = el.dataset.v; renderTransfers(); },
  'peers-window': el => { S.peersWindow = el.dataset.v; renderPeers(); },
  'msg-sel': el => { S.msgSel = +el.dataset.v; renderMessages(); renderToolbar(); },
  'conv-target': el => { S.conv.target = el.dataset.v; S.conv.state = {}; S.conv.running = false; renderConverter(); renderToolbar(); },
  'conv-sel': el => { S.conv.sel = +el.dataset.v; renderConverter(); },
  'conv-run': () => convRun(),
  'toggle-play': () => togglePlay(),
  'seek-rel': el => seekRel(+el.dataset.v),
  'prev-result': () => { moveSel(-1); previewSelection(); },
  'next-result': () => { moveSel(1); previewSelection(); },
  'auto-preview': () => { S.autoPreview = !S.autoPreview; renderPlayer(); if (S.view === 'settings') renderSettings(); },
  keep: () => keepPreview(),
  discard: () => discardPreview(),
  'wf-mode': el => { P.mode = el.dataset.v; store.set('wfMode', P.mode); WF.overKey = ''; P.dirty = true; renderPlayer(); if (S.view === 'settings') renderSettings(); },
  zoom: el => { const Z = [4, 8, 12, 24]; P.zoomSec = Z[clamp(Z.indexOf(P.zoomSec) + (el.dataset.v === 'in' ? -1 : 1), 0, Z.length - 1)]; P.dirty = true; },
  'toast-act': el => { const a = (TOAST_ACTS.get(el.dataset.t) || [])[+el.dataset.i]; dismissToast(el.dataset.t); if (a && a.run) a.run(); },
  'toast-close': el => dismissToast(el.dataset.t),
  'sheet-act': el => { const a = SHEET_ACTS[+el.dataset.i]; closeSheet(); if (a && a.run) a.run(); },
};
const CH = {
  preset: el => { S.presetId = el.value; S.filters = presetFilters(el.value); S.presetDirty = false; filtersChanged(false); },
  brOp: el => { S.filters.brOp = el.value; filtersChanged(); },
  brVal: el => { S.filters.brVal = +el.value; filtersChanged(); },
  cbrOnly: el => { S.filters.cbrOnly = el.checked; filtersChanged(); },
  minBits: el => { S.filters.minBits = +el.value; filtersChanged(); },
  minDur: el => { const v = parseDur(el.value); if (v == null) { el.value = fmtDur(S.filters.minDur); return; } S.filters.minDur = v; filtersChanged(); },
  maxDur: el => { const v = parseDur(el.value); if (v == null) { el.value = fmtDur(S.filters.maxDur); return; } S.filters.maxDur = v; filtersChanged(); },
  freeOnly: el => { S.filters.freeOnly = el.checked; filtersChanged(); },
  maxQueue: el => { S.filters.maxQueue = +el.value; filtersChanged(); },
  strict: el => { S.filters.strict = el.checked; filtersChanged(); },
  sort: el => { S.sort = el.value; S.sortDir = SORTS[el.value].dir; const th = $('#resTable thead'); if (th) th.innerHTML = theadHtml(); updateResults(); },
  autoPreview: el => { S.autoPreview = el.checked; renderPlayer(); },
  cancelOnSkip: el => { S.cancelOnSkip = el.value === '1'; },
  filingTarget: el => { S.filingTarget = el.value; renderContent(); },
  'inbox-sel': el => { if (el.checked) S.inboxSel.add(el.dataset.id); else S.inboxSel.delete(el.dataset.id); renderInbox(); },
  'inbox-all': el => { S.inboxSel = new Set(el.checked ? S.inbox.map(i => i.id) : []); renderInbox(); },
  'batch-preset': el => { S.batch.preset = el.value; batchParse(); S.batch.parsedText = S.batch.text; renderBatch(); renderToolbar(); },
  'batch-skip': el => { S.batch.skipOwned = el.checked; batchParse(); S.batch.parsedText = S.batch.text; renderBatch(); },
  'batch-spread': el => { S.batch.spread = el.checked; renderBatch(); },
  'batch-demo': el => { S.batch.demo = el.checked; if (S.batch.running) Clock.speed = el.checked ? 10 : 1; },
  'batch-approve': el => { const l = S.batch.lines.find(x => x.id === el.dataset.id); if (l) l.approved = el.checked; renderBatch(); },
  'conv-confirm': el => { if (el.checked) S.conv.confirmed.add(el.dataset.id); else S.conv.confirmed.delete(el.dataset.id); },
  'conv-id3': el => { S.conv.id3 = el.value; renderConverter(); },
  'conv-art': el => { S.conv.artMax = +el.value; renderConverter(); },
};
const ROW_ACTS = new Set(['sel', 'batch-sel', 'conv-sel']);
document.addEventListener('click', e => {
  const bg = $('#sheet');
  if (e.target === bg) { closeSheet(); return; }
  const el = e.target.closest('[data-act]');
  if (!el || el.disabled) return;
  if (ROW_ACTS.has(el.dataset.act) && e.target.closest('input, select, textarea, label')) return; // let form controls inside rows work
  const fn = ACT[el.dataset.act];
  if (fn) fn(el, e);
});
document.addEventListener('dblclick', e => { const row = e.target.closest('#resBody tr.r'); if (row) previewResult(S.results.get(row.dataset.id)); });
document.addEventListener('change', e => { const el = e.target.closest('[data-ch]'); if (el && el.dataset.ch !== 'batch-text' && CH[el.dataset.ch]) CH[el.dataset.ch](el); });
document.addEventListener('input', e => { const el = e.target; if (el.dataset && el.dataset.ch === 'batch-text') S.batch.text = el.value; });
document.addEventListener('submit', e => {
  e.preventDefault();
  if (e.target.id === 'searchForm') { const q = $('#q').value; if (q.trim()) { $('#q').blur(); startSearch(q); } return; }
  if (e.target.id === 'composer') {
    const inp = $('#msgInput'), text = inp.value.trim(), m = S.msgs[S.msgSel];
    if (!text || !m) return;
    m.items.push({ me: true, t: clockTime(), text });
    renderMessages(); $('#msgInput').focus();
  }
});
function spaceKey() {
  if (S.view === 'search') { const r = S.results.get(S.sel); if (r && (!P.track || P.track.rid !== r.id)) { previewResult(r); return; } }
  if (S.view === 'batch') { const l = S.batch.lines.find(x => x.id === S.batch.sel); if (l && l.best && (!P.track || P.track.rid !== l.best.id)) { previewResult(l.best); return; } }
  togglePlay();
}
document.addEventListener('keydown', e => {
  const t = e.target, typing = /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || t.isContentEditable, mod = e.metaKey || e.ctrlKey, key = e.key;
  if (mod && key.toLowerCase() === 'f') { e.preventDefault(); if (!S.sidebar) { S.sidebar = true; applyChrome(); } $('#q').focus(); $('#q').select(); return; }
  if (mod && key.toLowerCase() === 'i') { e.preventDefault(); toggleInspector(); return; }
  if (mod && key === ',') { e.preventDefault(); nav('settings'); return; }
  if (mod && /^[1-9]$/.test(key)) { e.preventDefault(); const it = $$('#nav .nav-item')[+key - 1]; if (it) nav(it.dataset.v); return; }
  if (key === 'Escape') { if (!$('#sheet').hidden) closeSheet(); else if (typing) t.blur(); return; }
  if (!$('#sheet').hidden || typing || mod || e.altKey) return;
  if ((key === 'Enter' || key === ' ') && t.getAttribute && t.getAttribute('role') === 'button') { e.preventDefault(); t.click(); return; }
  if (t.tagName === 'BUTTON' && (key === ' ' || key === 'Enter')) return;
  const g = S.view === 'search' && S._v ? S._v.byId.get(S.sel) : null;
  switch (key) {
    case 'ArrowDown': e.preventDefault(); moveSel(1); break;
    case 'ArrowUp': e.preventDefault(); moveSel(-1); break;
    case ' ': e.preventDefault(); spaceKey(); break;
    case 'ArrowLeft': case 'ArrowRight':
      e.preventDefault();
      if (g) { if ((key === 'ArrowRight') === S.collapsed.has(g.key)) toggleGroup(g.key); }
      else seekRel((key === 'ArrowLeft' ? -1 : 1) * (e.shiftKey ? 30 : 5));
      break;
    case 'Enter': {
      e.preventDefault();
      if (g) { if (g.album) confirmAlbum(g.key); break; }
      const r = S.view === 'search' ? S.results.get(S.sel) : null;
      if (r) requestDownload(r);
      break;
    }
    case 'k': case 'K': keepPreview(); break;
    case 'Backspace': case 'Delete': e.preventDefault(); discardPreview(); break;
    default: break;
  }
});

/* ---------- main loop & startup ---------- */
let lastLive = 0;
function frame(now) {
  const dt = Math.min(100, now - Clock.last) * Clock.speed;
  Clock.tick(now);
  updateSearches(); updateTransfers(dt); updateBatch(); updateFiling(dt); updateConvert(dt);
  const tk = P.track;
  if (tk && tk.tr && tk.tr.state === 'done' && !tk.complete) tk.complete = true;
  pumpAudio(6);
  if (tk && P.autoplay && !P.playing && (tk.gen >= tk.n * 0.08 || tk.gen >= SR * 20)) { P.autoplay = false; play(); }
  updatePlayback();
  drawScreen();
  if (now - lastLive > 250) { lastLive = now; refreshLive(false); if (P.playing || (tk && tk.tr && tk.tr.state !== 'done')) renderPlayerLite(); }
  requestAnimationFrame(frame);
}
function init() {
  seedData();
  const mk = (q, secondsAgo) => {
    const results = generateResults(q);
    const s = { id: 's' + S.nextSearch++, query: q, results, revealed: results.length, status: 'done', startedAt: -secondsAgo * 1000, cachedAt: null, sel: null };
    S.cache.set(norm(q), { results, at: s.startedAt });
    S.budget.push(s.startedAt);
    return s;
  };
  S.searches = [mk('neon rain', 40), mk('tidal memory', 95), mk('kassia moreau', 150)];
  S.budget.sort((a, b) => a - b);
  S.searchId = S.searches[0].id;
  S.view = 'search';
  const h = new URLSearchParams(location.hash.slice(1));
  if (['light', 'dark', 'system'].includes(h.get('theme'))) S.theme = h.get('theme');
  if (S.theme !== 'system') applyTheme();
  if (window.innerWidth <= 980) S.inspector = false;
  if (window.innerWidth <= 760) S.sidebar = false;
  const v = computeView(S.searches[0]);
  S._v = v;
  const first = v.groups && v.groups.length ? v.groups[0].items[0] : v.shown[0];
  if (first) { const tk = makeTrack(first); tk.complete = true; PREVIEW_CACHE.set(first.id, tk); P.track = tk; P.pos = 84; S.sel = first.id; }
  waveformEvents();
  if (h.get('search')) startSearch(h.get('search'));
  if (h.get('view') && VIEWS[h.get('view')]) S.view = h.get('view');
  S.ready = true;
  render();
  Clock.last = performance.now();
  requestAnimationFrame(frame);
}
init();
