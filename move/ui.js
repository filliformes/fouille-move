/*
 * Fouille — Overtake UI (QuickJS). Drives the engine via string params.
 *
 * Pads de gauche (16) = les voix. Tap = joue / arrete. Tenir = joue tant
 * qu'on tient. Double tap = figer / liberer. Shift + tap = fouiller encore.
 * Velocite = frappe du LPG ; pression = la voix s'ouvre.
 * Pads de droite (16) = les gestes : a jammer ensemble (etape 6).
 * Boutons de pas = choisir la voix. Gauche / Droite = page de reglages.
 * Molette = vue (Voix, Carte, Credits). Knobs 1-8 = la page de la voix choisie.
 * Back = suspendre (les voix continuent).
 */

import {
    Black, White, LightGrey, DarkGrey,
    NeonGreen, DarkMutedTeal, MutedTeal, AzureBlue, VeryDarkAzure, BrightOrange,
    MoveShift, MoveBack, MoveLeft, MoveRight, MoveUp, MoveDown, MoveMainKnob,
    MoveKnob1, MoveSteps, MoveCopy, MoveMenu, MoveUndo, MoveCapture,
    WhiteLedOff, WhiteLedDim, WhiteLedBright,
} from '/data/UserData/schwung/shared/constants.mjs';
import { setLED, setButtonLED, decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';

const NV = 16;
const W = 128;

/* ---- pads (Loopex geometry: left 4x4 = voices, bottom-left = voice 1) ---- */
function padNote(sec, i) { return 68 + Math.floor(i / 4) * 8 + sec * 4 + (i % 4); }
const LEFT = [], RIGHT = [], N2L = {}, N2R = {};
for (let i = 0; i < NV; i++) { LEFT.push(padNote(0, i)); RIGHT.push(padNote(1, i)); N2L[LEFT[i]] = i; N2R[RIGHT[i]] = i; }
const S2V = {}; for (let i = 0; i < NV; i++) S2V[MoveSteps[i]] = i;

/* ---- parameters (ids mirror engine/fe.h) ---- */
const P = { PLACE: 0, SOURCE: 1, RADIUS: 2, FLOW: 3, RISE: 4, HOLD: 5, FALL: 6, STRIKE: 7,
            SCATTER: 8, START: 9, LENGTH: 10, DIR: 11, SPEED: 12, GRAIN: 13, LOCK: 14, WEAR: 15,
            DRIFT: 16, TONE: 17, DRIVE: 18, PAN: 19, WIDTH: 20, POOL: 21, VOLUME: 22 };
const G = { VOLUME: 0, POOL_TIME: 1, POOL_FB: 2 };
let NPLACES = 20;
function steps(id) { return id === P.PLACE ? NPLACES : (id === P.SOURCE || id === P.HOLD) ? 3 : id === P.DIR ? 2 : 0; }

const PAGES = [
    { name: 'Lieu',    k: [[P.PLACE, 'Lieu'], [P.SOURCE, 'Source'], [P.RADIUS, 'Rayon'], [P.FLOW, 'Flux'],
                           null, null, null, [P.VOLUME, 'Volume']] },
    { name: 'Geste',   k: [[P.RISE, 'Rise'], [P.HOLD, 'Hold'], [P.FALL, 'Fall'], [P.STRIKE, 'Frappe'],
                           [P.SCATTER, 'Dispersion'], [P.START, 'Depart'], [P.LENGTH, 'Longueur'], [P.DIR, 'Sens']] },
    { name: 'Matiere', k: [[P.SPEED, 'Vitesse'], [P.GRAIN, 'Grain'], [P.LOCK, 'Lock'], [P.WEAR, 'Usure'],
                           [P.DRIFT, 'Derive'], [P.TONE, 'Ton'], [P.DRIVE, 'Saturation'], null] },
    { name: 'Espace',  k: [[P.PAN, 'Pan'], [P.WIDTH, 'Largeur'], [P.POOL, 'Envoi pool'], null,
                           [-1 - G.POOL_TIME, 'Pool temps'], [-1 - G.POOL_FB, 'Pool retour'], [-1 - G.VOLUME, 'Master'],
                           [P.VOLUME, 'Volume']] },
];
const VIEWS = ['Voix', 'Carte', 'Credits'];

/* ---- state ---- */
let sel = 0, page = 0, view = 0, tickN = 0, shift = false;
const st = new Array(NV).fill(0);          /* engine state 0..3 */
const busy = new Array(NV).fill(false), frozen = new Array(NV).fill(false);
const latched = new Array(NV).fill(false), downAt = new Array(NV).fill(0), consumed = new Array(NV).fill(false);
const lastTap = new Array(NV).fill(0);
let pv = [];                                /* params of the selected voice */
let gv = [0.6, 0.6, 0.45];
let card = null, wave = '', pos = 0, net = '';
let places = [];
let overlay = null, overlayUntil = 0;       /* {lbl, txt} */
let msg = '', msgUntil = 0;
const acc = new Array(8).fill(0);
const sendQ = new Map();                    /* key -> [k, v]: latest value wins, drained 3 per tick */

function now() { return (typeof Date !== 'undefined') ? Date.now() : tickN * 23; }
function sp(k, v) { try { host_module_set_param(k, v); } catch (e) {} }
function spB(k, v) {                        /* gates: ordered, must not be clobbered */
    try { if (typeof host_module_set_param_blocking === 'function') host_module_set_param_blocking(k, v, 50); else host_module_set_param(k, v); } catch (e) {}
}
function gp(k) { try { return host_module_get_param(k); } catch (e) { return null; } }
function queue(key, k, v) { sendQ.set(key, [k, v]); }
function drainQ() { let n = 3; for (const [key, kv] of sendQ) { sp(kv[0], kv[1]); sendQ.delete(key); if (--n <= 0) break; } }
function say(m, ms) { msg = m; msgUntil = now() + (ms || 1800); }
function clamp(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

/* ---- LEDs ---- */
let ledQ = [];
function enq(note, c) { ledQ.push([note, c]); }
function drainLEDs() { let n = 8; while (n-- > 0 && ledQ.length) { const e = ledQ.shift(); setLED(e[0], e[1]); } }
let blink = false;
function voiceColor(i) {
    if (busy[i] && st[i] < 2) return blink ? AzureBlue : VeryDarkAzure;            /* fouille : respire */
    if (st[i] === 0) return DarkGrey;
    if (st[i] === 3) return frozen[i] ? White : (busy[i] && blink ? MutedTeal : NeonGreen);
    return frozen[i] ? LightGrey : (busy[i] && blink ? MutedTeal : DarkMutedTeal);
}
function paintAll(force) {
    for (let i = 0; i < NV; i++) {
        if (force) { setLED(LEFT[i], voiceColor(i), true); setLED(RIGHT[i], Black, true); setLED(MoveSteps[i], i === sel ? White : Black, true); }
        else { enq(LEFT[i], voiceColor(i)); enq(MoveSteps[i], i === sel ? White : Black); }
    }
    setButtonLED(MoveLeft, page > 0 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveRight, page < PAGES.length - 1 ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveShift, shift ? WhiteLedBright : WhiteLedDim, true);
    setButtonLED(MoveBack, WhiteLedDim, true);
}
function clearAll() {
    for (let i = 0; i < NV; i++) { setLED(LEFT[i], Black, true); setLED(RIGHT[i], Black, true); setLED(MoveSteps[i], Black, true); }
    for (const b of [MoveLeft, MoveRight, MoveShift, MoveBack, MoveUp, MoveDown, MoveCopy, MoveMenu, MoveUndo, MoveCapture])
        setButtonLED(b, WhiteLedOff, true);
}

/* ---- DSP readback ---- */
function pollStates() {
    const s = gp('st');
    if (!s || s.length < NV) return;
    for (let i = 0; i < NV; i++) {
        const c = s.charCodeAt(i) - 97;
        const nst = c & 3, nb = !!(c & 4), nf = !!(c & 8);
        if (nst !== st[i] || nb !== busy[i] || nf !== frozen[i]) {
            const arrived = i === sel && busy[i] && !nb;
            st[i] = nst; busy[i] = nb; frozen[i] = nf;
            enq(LEFT[i], voiceColor(i));
            if (arrived) loadCard();
        }
        if (nst !== 3 && latched[i] && !downAt[i]) latched[i] = false;               /* the DSP is the truth */
    }
}
function loadParams() {
    const s = gp('pv:' + sel);
    if (s) pv = s.split(',').map(parseFloat);
    const g = gp('gv');
    if (g) gv = g.split(',').map(parseFloat);
}
function loadCard() {
    const c = gp('card:' + sel);
    if (c) { const f = c.split('|'); card = { place: f[0], kind: f[1], title: f[2], author: f[3], lic: f[4], secs: f[5], lat: f[6], lon: f[7], url: f[8] }; }
    const w = gp('wave:' + sel);
    if (w) wave = w;
}
function loadPlaces() {
    const n = parseInt(gp('nplaces') || '20');
    if (n > 0) NPLACES = n;
    places = [];
    for (let i = 0; i < NPLACES; i++) {
        const s = gp('place:' + i);
        if (!s) continue;
        const f = s.split('|');
        places.push({ name: f[0], lat: parseFloat(f[1]), lon: parseFloat(f[2]) });
    }
}
function selectVoice(i) {
    if (i === sel) return;
    enq(MoveSteps[sel], Black);
    sel = i;
    enq(MoveSteps[sel], White);
    loadParams(); loadCard();
}

/* ---- knobs ---- */
function knobDef(k) { return PAGES[page].k[k]; }
function knobValue(d) { return d[0] < 0 ? gv[-1 - d[0]] : pv[d[0]]; }
function showKnob(k) {
    const d = knobDef(k);
    if (!d) { overlay = { lbl: '-', txt: '' }; overlayUntil = now() + 800; return; }
    const v = knobValue(d);
    let txt;
    if (d[0] < 0) txt = Math.round(v * 100) + '%';
    else txt = gp('fmt:' + sel + ',' + d[0] + ',' + (v || 0).toFixed(4)) || (v || 0).toFixed(2);
    overlay = { lbl: d[1], txt: txt };
    overlayUntil = now() + 1500;
}
function turnKnob(k, delta) {
    const d = knobDef(k);
    if (!d) return;
    if (d[0] < 0) {                                    /* global */
        const gi = -1 - d[0];
        gv[gi] = clamp(gv[gi] + delta * (shift ? 0.002 : 0.01));
        queue('g' + gi, 'gp', gi + ',' + gv[gi].toFixed(4));
    } else {
        const id = d[0], n = steps(id);
        let v = pv[id] || 0;
        if (n > 0) {                                   /* enum: three detents per step */
            acc[k] += delta;
            if (Math.abs(acc[k]) < 3) return;
            const cur = Math.round(v * (n - 1)), nx = Math.max(0, Math.min(n - 1, cur + (acc[k] > 0 ? 1 : -1)));
            acc[k] = 0;
            if (nx === cur) return;
            v = nx / (n - 1);
        } else v = clamp(v + delta * (shift ? 0.002 : 0.01));
        pv[id] = v;
        queue('p' + id, 'p', sel + ',' + id + ',' + v.toFixed(4));
    }
    showKnob(k);
}

/* ---- pads ---- */
function padDown(i, vel) {
    selectVoice(i);
    const t = now();
    consumed[i] = false;
    if (shift) {                                       /* fouiller encore */
        spB('rf', String(i)); consumed[i] = true;
        say('fouille encore : ' + (card ? card.place : ''));
        return;
    }
    if (t - lastTap[i] < 300) {                         /* double tap: figer / liberer */
        spB('fz', i + ',' + (frozen[i] ? 0 : 1));
        say(frozen[i] ? 'voix liberee' : 'voix figee');
        consumed[i] = true; lastTap[i] = 0;
        return;
    }
    if (latched[i] || st[i] === 3) {                    /* playing: a tap stops it */
        spB('g', i + ',0,0'); latched[i] = false; consumed[i] = true;
        lastTap[i] = t;
        return;
    }
    spB('g', i + ',1,' + vel.toFixed(3));
    downAt[i] = t;
    if (st[i] === 0) say('fouille ' + (card ? card.place : '...'));
}
function padUp(i) {
    const t = now(), held = downAt[i] ? t - downAt[i] : 0;
    downAt[i] = 0;
    if (consumed[i]) { consumed[i] = false; return; }
    if (held > 350) { spB('g', i + ',0,0'); latched[i] = false; }   /* tenir = momentane */
    else { latched[i] = true; lastTap[i] = t; }                      /* tap = verrouille */
}
const lastPress = new Array(NV).fill(0);
function padPressure(i, p) {
    const t = now();
    if (t - lastPress[i] < 30) return;
    lastPress[i] = t;
    queue('pr' + i, 'pr', i + ',' + p.toFixed(3));
}

/* ---- screen ---- */
function text(x, y, s) { print(x, y, s, 1); }
function fit(s, n) { s = s || ''; return s.length > n ? s.substring(0, n - 1) + '.' : s; }
function scroll(s, n) {
    s = s || '';
    if (s.length <= n) return s;
    const pad = s + '   ', off = Math.floor(tickN / 6) % pad.length;
    return (pad + pad).substring(off, off + n);
}
function drawStrip() {
    fill_rect(0, 9, W, 1, 1);
    for (let i = 0; i < NV; i++) {
        const x = i * 8;
        if (st[i] === 3) fill_rect(x + 1, 10, 6, 3, 1);
        else if (st[i] === 2) fill_rect(x + 1, 12, 6, 1, 1);
        else if (busy[i] && blink) fill_rect(x + 3, 11, 2, 1, 1);
        if (i === sel) draw_rect(x, 10, 8, 4, 1);
    }
}
function drawHeader() {
    const name = (card && card.place) ? card.place : '';
    text(0, 0, fit((sel + 1) + ' ' + name, 12));
    const r = fit(net, 9);
    text(W - r.length * 6, 0, r);
}
function drawWave() {
    const y0 = 16, h = 22, mid = y0 + h / 2;
    if (!wave || st[sel] === 0) { text(16, y0 + 6, busy[sel] ? 'fouille en cours...' : 'voix vide'); return; }
    for (let i = 0; i < 64; i++) {
        const a = Math.max(1, Math.round((wave.charCodeAt(i) - 65) / 31 * h / 2));
        fill_rect(i * 2, Math.round(mid - a), 1, a * 2, 1);
    }
    const px = Math.round(pos * (W - 1));
    if (st[sel] === 3) fill_rect(px, y0 - 1, 1, h + 2, 1);
}
function drawBottom() {
    if (overlay && now() < overlayUntil) {
        fill_rect(0, 41, W, 23, 0);
        text(0, 43, PAGES[page].name + ' > ' + overlay.lbl);
        text(0, 53, fit(overlay.txt, 21));
        return;
    }
    if (msg && now() < msgUntil) { text(0, 43, fit(msg, 21)); return; }
    if (card && st[sel] > 0) {
        text(0, 42, scroll(card.title, 21));
        text(0, 52, fit((card.author || card.kind) + ' ' + (card.lic || ''), 21));
    } else text(0, 47, 'page ' + PAGES[page].name);
}
function drawMap() {
    /* Montreal: lat 45.44..45.57, lon -73.69..-73.53 */
    const la0 = 45.44, la1 = 45.57, lo0 = -73.69, lo1 = -73.53, y0 = 14, h = 40;
    draw_rect(0, y0 - 1, W, h + 2, 1);
    const used = {};
    for (let i = 0; i < NV; i++) if (st[i] > 0) used[Math.round((pvCache[i] || 0) * (NPLACES - 1))] = st[i];
    let selPlace = Math.round((pv[P.PLACE] || 0) * (NPLACES - 1));
    for (let p = 0; p < places.length; p++) {
        const pl = places[p];
        if (pl.lat < la0 || pl.lat > la1 || pl.lon < lo0 || pl.lon > lo1) continue;
        const x = Math.round((pl.lon - lo0) / (lo1 - lo0) * (W - 4)) + 2;
        const y = Math.round((la1 - pl.lat) / (la1 - la0) * (h - 4)) + y0 + 2;
        if (p === selPlace) { draw_rect(x - 3, y - 3, 7, 7, 1); fill_rect(x - 1, y - 1, 3, 3, 1); }
        else if (used[p] === 3) fill_rect(x - 1, y - 1, 3, 3, 1);
        else fill_rect(x, y, 1, 1, 1);
    }
    const nm = places[selPlace] ? places[selPlace].name : '';
    const inMtl = places[selPlace] && places[selPlace].lat > la0 && places[selPlace].lat < la1 && places[selPlace].lon > lo0;
    text(0, 56, fit(inMtl ? nm : nm + ' (hors carte)', 21));
}
const pvCache = new Array(NV).fill(0);               /* each voice's place, for the map */
function drawCredits() {
    if (!card || st[sel] === 0) { text(0, 20, 'rien encore'); return; }
    text(0, 12, fit(card.place + ' - ' + card.kind, 21));
    const t = card.title || '';
    text(0, 22, t.substring(0, 21)); text(0, 32, fit(t.substring(21), 21));
    text(0, 42, fit(card.author, 21));
    text(0, 52, fit(card.lic + ' ' + card.secs + 's', 21));
}
function draw() {
    clear_screen();
    drawHeader();
    drawStrip();
    if (view === 0) { drawWave(); drawBottom(); }
    else if (view === 1) drawMap();
    else drawCredits();
    host_flush_display();
}

/* ---- lifecycle ---- */
globalThis.init = function () {
    loadPlaces();
    pollStates();
    loadParams(); loadCard();
    paintAll(true);
    say('Fouille : 16 voix ecoutent', 2500);
};
globalThis.onUnload = function () { clearAll(); };
globalThis.onResume = function () { pollStates(); loadParams(); loadCard(); paintAll(true); resume = 4; };
let resume = 0;

globalThis.tick = function () {
    tickN++;
    if (tickN % 10 === 0) {
        blink = !blink;
        for (let i = 0; i < NV; i++) if (busy[i]) enq(LEFT[i], voiceColor(i));
    }
    if (tickN % 3 === 0) pollStates();
    if (view === 0 && st[sel] === 3 && tickN % 2 === 0) { const p = parseFloat(gp('pos:' + sel)); if (!isNaN(p)) pos = p; }
    if (tickN % 40 === 7) { const n = gp('net'); if (n != null) net = n; }
    if (tickN % 45 === 11) loadCard();
    if (view === 1 && tickN % 30 === 3) for (let i = 0; i < NV; i++) { const s = gp('pv:' + i); if (s) pvCache[i] = parseFloat(s.split(',')[0]); }
    if (resume > 0 && tickN % 8 === 0) { resume--; paintAll(true); }
    drainQ();
    drainLEDs();
    draw();
};

globalThis.onMidiMessageInternal = function (data) {
    const status = data[0] & 0xf0, d1 = data[1], d2 = data[2];
    if (status === 0xb0) {
        if (d1 === MoveBack && d2 > 0) { clearAll(); host_exit_module(); return; }
        if (d1 === MoveShift) { shift = d2 > 0; setButtonLED(MoveShift, shift ? WhiteLedBright : WhiteLedDim, true); return; }
        if ((d1 === MoveLeft || d1 === MoveRight) && d2 > 0) {
            page = Math.max(0, Math.min(PAGES.length - 1, page + (d1 === MoveRight ? 1 : -1)));
            overlay = { lbl: PAGES[page].name, txt: PAGES[page].k.map(d => d ? d[1].substring(0, 4) : '-').join(' ') };
            overlayUntil = now() + 1500; acc.fill(0);
            paintAll(true); return;
        }
        if ((d1 === MoveUp || d1 === MoveDown) && d2 > 0) { selectVoice((sel + (d1 === MoveUp ? 1 : NV - 1)) % NV); return; }
        if (d1 === MoveMainKnob) { const dd = decodeDelta(d2); if (dd) { view = (view + (dd > 0 ? 1 : VIEWS.length - 1)) % VIEWS.length; say('vue ' + VIEWS[view], 900); } return; }
        if (d1 >= MoveKnob1 && d1 < MoveKnob1 + 8) { turnKnob(d1 - MoveKnob1, decodeDelta(d2)); return; }
        return;
    }
    if (status === 0x90 || status === 0x80) {
        const on = status === 0x90 && d2 > 0;
        if (d1 <= 7) { if (on) showKnob(d1); return; }                         /* knob touch */
        if (d1 in N2L) { if (on) padDown(N2L[d1], d2 / 127); else padUp(N2L[d1]); return; }
        if (d1 in N2R) { if (on) say('gestes : a jammer ensemble'); return; }
        if (d1 in S2V) { if (on) selectVoice(S2V[d1]); return; }
        return;
    }
    if (status === 0xa0) { if (d1 in N2L) padPressure(N2L[d1], d2 / 127); return; }
};
globalThis.onMidiMessageExternal = function (data) { };
