/*
 * Fouille engine — voices, DSP, control queue, readback.
 * The audio path (fe_render and what it calls) never allocates, locks,
 * logs or touches a file.
 */
#define _POSIX_C_SOURCE 200809L
#include "fe_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float flush(float x) { return (fabsf(x) < 1e-20f) ? 0.0f : x; }

/* ── parameter mappings ─────────────────────────────────────────────────── */

int fe_steps(int id) {
    switch (id) {
    case FE_P_PLACE:  return FE_NPLACES;
    case FE_P_SOURCE: return 3;          /* Les deux · Aporee · Freesound */
    case FE_P_HOLD:   return 3;          /* Boucle · Derive · Grains */
    case FE_P_DIR:    return 2;          /* Avant · Arriere */
    default:          return 0;
    }
}
static int step_of(int id, float v) {
    int n = fe_steps(id);
    int i = (int)floorf(v * (float)(n - 1) + 0.5f);
    return i < 0 ? 0 : (i >= n ? n - 1 : i);
}
static float m_rise(float v)    { return 0.0005f * powf(8000.0f, v); }   /* 0.5 ms .. 4 s */
static float m_fall(float v)    { return 0.005f * powf(1600.0f, v); }    /* 5 ms .. 8 s */
static float m_speed(float v)   { return powf(2.0f, (v - 0.5f) * 4.0f); }/* x0.25 .. x4 */
static float m_grain(float v)   { return 0.01f * powf(50.0f, v); }       /* 10 ms .. 500 ms */
static float m_radius(float v)  { return powf(2.0f, (v - 0.5f) * 4.0f); }/* x0.25 .. x4 */
static float m_flow(float v)    { return v < 0.02f ? 0.0f : 10.0f * powf(60.0f, v); } /* off, 10 s .. 10 min */
static float m_pooltime(float v){ return 0.08f * powf(25.0f, v); }       /* 80 ms .. 2 s */

static const float P_DEFAULT[FE_P_COUNT] = {
    [FE_P_PLACE] = 0, [FE_P_SOURCE] = 0, [FE_P_RADIUS] = 0.5f, [FE_P_FLOW] = 0,
    [FE_P_RISE] = 0.15f, [FE_P_HOLD] = 0, [FE_P_FALL] = 0.45f, [FE_P_STRIKE] = 0.5f,
    [FE_P_SCATTER] = 0.3f, [FE_P_START] = 0, [FE_P_LENGTH] = 1.0f, [FE_P_DIR] = 0,
    [FE_P_SPEED] = 0.5f, [FE_P_GRAIN] = 0.4f, [FE_P_LOCK] = 0, [FE_P_WEAR] = 0.15f,
    [FE_P_DRIFT] = 0.25f, [FE_P_TONE] = 0.5f, [FE_P_DRIVE] = 0,
    [FE_P_PAN] = 0.5f, [FE_P_WIDTH] = 0.67f, [FE_P_POOL] = 0, [FE_P_VOLUME] = 0.7f,
};

/* ── control queue (SPSC) ───────────────────────────────────────────────── */

static void push(fe_engine *e, int type, int v, int id, float val) {
    unsigned h = atomic_load_explicit(&e->q_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&e->q_tail, memory_order_acquire);
    if (h - t >= FE_QSIZE) return;                       /* full: drop (UI resyncs) */
    fe_event *ev = &e->q[h & (FE_QSIZE - 1)];
    ev->type = (uint8_t)type; ev->v = (uint8_t)v; ev->id = (uint8_t)id; ev->val = val;
    atomic_store_explicit(&e->q_head, h + 1, memory_order_release);
}
static int vok(int v) { return v >= 0 && v < FE_NV; }
void fe_gate(fe_engine *e, int v, int on, float vel) { if (vok(v)) push(e, on ? EV_GATE_ON : EV_GATE_OFF, v, 0, vel); }
void fe_pressure(fe_engine *e, int v, float p)        { if (vok(v)) push(e, EV_PRESS, v, 0, p); }
void fe_refetch(fe_engine *e, int v)                  { if (vok(v)) push(e, EV_REFETCH, v, 0, 0); }
void fe_freeze(fe_engine *e, int v, int on)           { if (vok(v)) push(e, EV_FREEZE, v, 0, (float)on); }
void fe_set(fe_engine *e, int v, int id, float x)     { if (vok(v) && id >= 0 && id < FE_P_COUNT) push(e, EV_SET, v, id, clampf(x, 0, 1)); }
void fe_set_global(fe_engine *e, int id, float x)     { if (id >= 0 && id < FE_G_COUNT) push(e, EV_GSET, 0, id, clampf(x, 0, 1)); }

/* ── gestures: settings, mappings, API ──────────────────────────────────── */

/* All sixteen are built (v0.5.0). */
static const uint8_t X_READY[FE_X_COUNT] = {
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
};
static const float X_DEF[FE_X_COUNT][FE_X_NPARAM] = {
    [FE_X_BONGO]     = { 0.25f, 0.40f, 0.50f, 0.00f },  /* montee, chute, non-linearite, decalage */
    [FE_X_PULSARS]   = { 0.30f, 0.80f, 0.50f, 0.00f },  /* taille, regularite, forme, masque */
    [FE_X_ACCROCHE]  = { 0.50f, 0.40f, 0.40f, 1.00f },  /* centre, largeur, vitesse de suivi, melange */
    [FE_X_CORPS]     = { 0.50f, 0.30f, 0.50f, 0.60f },  /* accord, matiere, brillance, melange */
    [FE_X_ETIREMENT] = { 0.50f, 0.60f, 0.50f, 0.00f },  /* plage min, plage max, courbe, lien a la vitesse */
    [FE_X_CONTAGION] = { 0.50f, 0.50f, 0.30f, 0.50f },  /* portee, courbe, son source, stereo */
    [FE_X_VOISIN]    = { 0.50f, 0.50f, 0.40f, 0.00f },  /* axe X (brillance), axe Y (intensite), fondu, lieu garde */
    [FE_X_DERIVE]    = { 0.00f, 0.50f, 0.00f, 1.00f },  /* direction, rayon, vitesse, retour */
    [FE_X_GEL]       = { 0.30f, 0.20f, 0.30f, 0.40f },  /* respiration, derive, fondu d'entree, fondu de sortie */
    [FE_X_RESSAC]    = { 0.50f, 0.50f, 0.30f, 0.50f },  /* temps, filtre, dephasage, plafond */
    [FE_X_PASSE]     = { 0.00f, 0.40f, 0.30f, 0.00f },  /* reprise, fondu, usure du souvenir, decalage */
    [FE_X_REECRITURE]= { 0.50f, 0.30f, 0.20f, 0.00f },  /* perte d'aigus, craquements, trous, source */
    [FE_X_TERNE]     = { 0.00f, 0.50f, 0.40f, 0.30f },  /* famille, ligne, delai, lissage */
    [FE_X_DOUBLE]    = { 0.50f, 0.00f, 0.30f, 0.20f },  /* familles permises, graine, memoire du tirage, retour */
    [FE_X_HESITATION]= { 0.40f, 0.40f, 0.00f, 0.20f },  /* tempo, instabilite, sons concernes, bascule */
    [FE_X_FUTUR]     = { 0.40f, 0.40f, 0.00f, 0.00f },  /* fondu, filtre, position, source */
};
static float mx_bong_rise(float x) { return 0.0005f * powf(100.0f, x); }   /* 0.5 .. 50 ms */
static float mx_bong_fall(float x) { return 0.04f * powf(75.0f, x); }      /* 40 ms .. 3 s */
static float mx_bong_off(float x)  { return 0.4f * x; }                    /* 0 .. 400 ms between sounds */
static float mx_acc_oct(float x)   { return (x - 0.5f) * 4.0f; }           /* -2 .. +2 octaves */
static float mx_acc_track(float x) { return 0.005f * powf(400.0f, x); }    /* 5 ms .. 2 s */
static float mx_cont_km(float x)   { return 0.3f * powf(100.0f, x); }      /* 300 m .. 30 km */
static float mx_drift_km(float a)  { return 0.3f * powf(100.0f, a); }      /* pressure: 300 m .. 30 km */
static float mx_drift_hop(float x) { return x < 0.02f ? 0.0f : 20.0f * powf(10.0f, 1.0f - x); } /* once, 200 s .. 20 s */
static float mx_gel_fade(float x)  { return 0.005f * powf(600.0f, x); }    /* 5 ms .. 3 s */
static float mx_gel_size(float a)  { return 0.02f * powf(20.0f, a); }      /* pressure: 20 .. 400 ms */
static float mx_rs_time(float x)   { return 0.06f * powf(33.0f, x); }      /* 60 ms .. 2 s */

static float mx_pul_hz(float a)    { return 1.5f * powf(130.0f, a); }      /* pressure: 1.5 .. 195 pulses/s */
static float mx_corps_t60(float a) { return 0.05f * powf(80.0f, a); }      /* pressure: 50 ms .. 4 s */
static float mx_st_min(float x)    { return powf(2.0f, -2.0f + 2.0f * x); } /* x0.25 .. x1 */
static float mx_st_max(float x)    { return powf(2.0f, 4.0f * x); }        /* x1 .. x16 */
static float mx_fade(float x)      { return 0.02f * powf(200.0f, x); }     /* 20 ms .. 4 s */
static float mx_t_delay(float x)   { return 0.03f * powf(40.0f, x); }      /* 30 ms .. 1.2 s */
static float mx_hes_bpm(float x)   { return 30.0f * powf(8.0f, x); }       /* 30 .. 240 bpm */
static int   t_fam_of(float x)     { int f = (int)floorf(x * 4.0f + 0.5f); return f < 0 ? 0 : (f > 4 ? 4 : f); }
static const char *T_FAM[5] = { "Rebond", "Rafale", "Begaiement", "Vague", "Pluie" };

static int xok(int g) { return g >= 0 && g < FE_X_COUNT; }
void fe_gesture(fe_engine *e, int v, int g, float a) {
    if (!xok(g) || v < -1 || v >= FE_NV) return;
    push(e, EV_GEST, v < 0 ? FE_NV : v, g, clampf(a, 0, 1));
}
void fe_gesture_param(fe_engine *e, int g, int k, float x) {
    if (xok(g) && k >= 0 && k < FE_X_NPARAM) push(e, EV_XPARAM, 0, g * FE_X_NPARAM + k, clampf(x, 0, 1));
}
void fe_gesture_reset(fe_engine *e, int g) { if (xok(g)) push(e, EV_XRESET, 0, g, 0); }
void fe_adopt(fe_engine *e, int v) { if (v >= -1 && v < FE_NV) push(e, EV_ADOPT, v < 0 ? FE_NV : v, 0, 0); }
int  fe_voice_has_next(const fe_engine *e, int v) { return vok(v) && e->voices[v].next != NULL; }
float fe_gesture_get(const fe_engine *e, int v, int g) {
    if (!xok(g)) return 0;
    if (v < 0) return e->gx[g];
    return vok(v) ? e->voices[v].vx[g] : 0;
}
float fe_gesture_param_get(const fe_engine *e, int g, int k) { return (xok(g) && k >= 0 && k < FE_X_NPARAM) ? e->xp[g][k] : 0; }
float fe_gesture_param_default(int g, int k) { return (xok(g) && k >= 0 && k < FE_X_NPARAM) ? X_DEF[g][k] : 0; }
int   fe_gesture_ready(int g) { return xok(g) ? X_READY[g] : 0; }

static const char *compass(float deg) {
    static const char *c[] = { "N", "NE", "E", "SE", "S", "SO", "O", "NO" };
    int i = (int)floorf(fmodf(deg + 22.5f + 360.0f, 360.0f) / 45.0f);
    return c[i & 7];
}
static int fmt_time(char *b, size_t L, float s) { return s < 1 ? snprintf(b, L, "%d ms", (int)(s * 1000 + 0.5f)) : snprintf(b, L, "%.1f s", s); }
int fe_gesture_format(int g, int k, float x, char *b, int len) {
    size_t L = (size_t)len;
    int pc = (int)(x * 100 + 0.5f);
    switch (g) {
    case FE_X_BONGO:
        if (k == 0) return fmt_time(b, L, mx_bong_rise(x));
        if (k == 1) return fmt_time(b, L, mx_bong_fall(x));
        if (k == 3) return x < 0.01f ? snprintf(b, L, "ensemble") : fmt_time(b, L, mx_bong_off(x));
        break;
    case FE_X_ACCROCHE:
        if (k == 0) return snprintf(b, L, "%+.1f oct", mx_acc_oct(x));
        if (k == 2) return fmt_time(b, L, mx_acc_track(x));
        break;
    case FE_X_CONTAGION:
        if (k == 0) { float km = mx_cont_km(x); return km < 1 ? snprintf(b, L, "%d m", (int)(km * 1000)) : snprintf(b, L, "%.1f km", km); }
        if (k == 2) return x < 0.05f ? snprintf(b, L, "matiere") : x > 0.95f ? snprintf(b, L, "son joue") : snprintf(b, L, "%d%% joue", pc);
        break;
    case FE_X_DERIVE:
        if (k == 0) { if (x < 0.04f) return snprintf(b, L, "toutes"); float d = (x - 0.04f) / 0.96f * 360.0f; return snprintf(b, L, "%s %d deg", compass(d), (int)d); }
        if (k == 1) return snprintf(b, L, "x%.2f", m_radius(x));
        if (k == 2) { float h = mx_drift_hop(x); return h <= 0 ? snprintf(b, L, "une fois") : snprintf(b, L, "tous les %d s", (int)h); }
        if (k == 3) return snprintf(b, L, "%s", x >= 0.5f ? "revient" : "reste");
        break;
    case FE_X_GEL:
        if (k == 2 || k == 3) return fmt_time(b, L, mx_gel_fade(x));
        break;
    case FE_X_RESSAC:
        if (k == 0) return fmt_time(b, L, mx_rs_time(x));
        break;
    case FE_X_PULSARS:
        if (k == 2) return snprintf(b, L, "%s", x < 0.33f ? "carree" : x < 0.67f ? "cloche" : "eclat");
        break;
    case FE_X_CORPS:
        if (k == 0) return snprintf(b, L, "%+d st", (int)floorf((x - 0.5f) * 24.0f + 0.5f));
        if (k == 1) return snprintf(b, L, "%s", x < 0.33f ? "corde" : x < 0.67f ? "barre" : "plaque");
        break;
    case FE_X_ETIREMENT:
        if (k == 0) return snprintf(b, L, "x%.2f", mx_st_min(x));
        if (k == 1) return snprintf(b, L, "x%.1f", mx_st_max(x));
        if (k == 3) return x < 0.05f ? snprintf(b, L, "hauteur gardee") : x > 0.95f ? snprintf(b, L, "bande") : snprintf(b, L, "%d%%", pc);
        break;
    case FE_X_VOISIN:
        if (k == 2) return fmt_time(b, L, mx_fade(x));
        if (k == 3) return snprintf(b, L, "%s", x >= 0.5f ? "meme lieu" : "partout");
        break;
    case FE_X_PASSE:
        if (k == 0) return snprintf(b, L, "%s", x < 0.5f ? "ou il etait" : "au Depart");
        if (k == 1) return fmt_time(b, L, mx_fade(x));
        if (k == 3) return x < 0.01f ? snprintf(b, L, "ensemble") : fmt_time(b, L, x);
        break;
    case FE_X_REECRITURE:
        if (k == 3) return snprintf(b, L, "%s", x < 0.5f ? "le son" : "le mix");
        break;
    case FE_X_TERNE:
        if (k == 0) return snprintf(b, L, "%s", T_FAM[t_fam_of(x)]);
        if (k == 2) return fmt_time(b, L, mx_t_delay(x));
        break;
    case FE_X_DOUBLE:
        if (k == 0) return snprintf(b, L, "%d familles", 1 + t_fam_of(x));
        if (k == 1) return snprintf(b, L, "%d", (int)(x * 99 + 0.5f));
        break;
    case FE_X_HESITATION:
        if (k == 0) return snprintf(b, L, "%d bpm", (int)(mx_hes_bpm(x) + 0.5f));
        if (k == 2) return x < 0.02f ? snprintf(b, L, "sons ouverts") : snprintf(b, L, "+%d%% des fermes", pc);
        break;
    case FE_X_FUTUR:
        if (k == 0) return fmt_time(b, L, mx_fade(x));
        if (k == 2) { return snprintf(b, L, "%d%% du fragment", pc); }
        if (k == 3) { static const char *S[] = { "Les deux", "Aporee", "Freesound" }; int i = (int)floorf(x * 2 + 0.5f); return snprintf(b, L, "%s", S[i < 0 ? 0 : i > 2 ? 2 : i]); }
        break;
    }
    return snprintf(b, L, "%d%%", pc);
}

/* ── voice helpers (audio thread) ───────────────────────────────────────── */

static void request_harvest(fe_engine *e, int vi) {
    fe_voice *v = &e->voices[vi];
    if (atomic_load(&v->want)) return;
    v->req.place = step_of(FE_P_PLACE, v->p[FE_P_PLACE]);
    v->req.source = step_of(FE_P_SOURCE, v->p[FE_P_SOURCE]);
    if (v->want_next) { int fs = (int)floorf(e->xp[FE_X_FUTUR][3] * 2 + 0.5f); if (fs > 0) v->req.source = fs; }
    v->req.radius_scale = m_radius(v->p[FE_P_RADIUS]);
    v->req.drift_km = v->drift_km;
    v->req.drift_deg = v->drift_deg;
    atomic_store_explicit(&v->want, 1, memory_order_release);
}

static void compute_region(fe_engine *e, fe_voice *v) {
    float F = (float)v->tape->frames;
    if (F < 2) { v->loop_a = 0; v->loop_len = 1; return; }
    float a = clampf(v->p[FE_P_START], 0, 0.98f) * F;
    float rem = F - a;
    float minlen = fminf(0.05f * e->sr, rem);
    float len = clampf(v->p[FE_P_LENGTH] * rem, minlen, rem);
    v->loop_a = a; v->loop_len = len;
}

static void keep_in_region(fe_voice *v) {
    float b = v->loop_a + v->loop_len;
    if (v->pos < v->loop_a || v->pos >= b) v->pos = v->loop_a + fmodf(fabsf(v->pos - v->loop_a), v->loop_len);
}

static void reset_dsp(fe_voice *v) {
    v->vac = 0;
    for (int c = 0; c < 2; c++) { v->lp1[c] = v->lp2[c] = v->wlp[c] = v->tlp[c] = v->bp1[c] = v->bp2[c] = 0; }
    v->hiss_lp = 0;
}

/* exact = 1: a pad opens the sound at its position 0 (Depart), no scatter. */
static void start_position(fe_voice *v, int exact) {
    float sc = exact ? 0.0f : v->p[FE_P_SCATTER] * 0.5f * fe_randf(&v->rng) * v->loop_len;
    int rev = step_of(FE_P_DIR, v->p[FE_P_DIR]);
    v->pos = rev ? (v->loop_a + v->loop_len - 1 - sc) : (v->loop_a + sc);
    for (int g = 0; g < 2; g++) { v->grain_ph[g] = 0.5f * (float)g; v->grain_org[g] = v->pos; }
}

/* A fresh fragment arrives: the current one becomes the past, the old past goes back to the worker. */
static void do_swap(fe_engine *e, int vi) {
    fe_voice *v = &e->voices[vi];
    fe_tape *old = v->tape;
    v->tape = e->spare;
    e->spare = v->past;
    v->past = old; v->past_pos = v->pos;
    v->wear = 0; v->since_swap = 0;
    compute_region(e, v);
    start_position(v, 0);
    atomic_store_explicit(&e->pending, -1, memory_order_release);   /* spare back to the worker */
}

/* Futur adopted: the prepared fragment plays now, the current one becomes the past. */
static void do_adopt(fe_engine *e, fe_voice *v) {
    if (!v->next) return;
    if (e->nfree < FE_NFUT) e->freet[e->nfree++] = v->past;
    v->past = v->tape; v->past_pos = v->pos;
    v->tape = v->next; v->next = NULL; v->next_mix = 0;
    v->wear = 0; v->since_swap = 0;
    compute_region(e, v);
    start_position(v, 1);
}
static void drop_next(fe_engine *e, fe_voice *v) {
    if (v->next && e->nfree < FE_NFUT) { e->freet[e->nfree++] = v->next; v->next = NULL; v->next_mix = 0; }
    v->want_next = 0;
}
/* renew: a prepared Futur is what the voice plays next; otherwise listen again */
static void renew(fe_engine *e, int vi) {
    fe_voice *v = &e->voices[vi];
    if (v->next) {
        if (v->env_stage == ENV_OFF) do_adopt(e, v);
        else { v->adopt = 1; v->swapping = 1; }
    } else request_harvest(e, vi);
}

static void apply_event(fe_engine *e, const fe_event *ev) {
    fe_voice *v = &e->voices[ev->v < FE_NV ? ev->v : 0];
    switch (ev->type) {
    case EV_GATE_ON:
        if (v->tape->frames < 64) { request_harvest(e, ev->v); break; }
        v->vel = clampf(ev->val, 0, 1);
        if (v->env_stage == ENV_OFF) {
            v->env = 0; reset_dsp(v);
            compute_region(e, v); start_position(v, 1);
            v->gel_on = 0; v->gel_mix = 0;
            v->env_stage = ENV_RISE;
            v->auto_close = 0;
        } else if (v->env_stage == ENV_FALL) {        /* reopened in its tail: dip, then back to position 0 */
            v->reseek = 1; v->swapping = 1;
            v->env_stage = ENV_SUSTAIN;
        } else v->env_stage = ENV_RISE;               /* retrigger from where the envelope is: no click */
        break;
    case EV_GATE_OFF:
        if (v->env_stage == ENV_RISE || v->env_stage == ENV_SUSTAIN) v->env_stage = ENV_FALL;
        break;
    case EV_PRESS:   v->press = clampf(ev->val, 0, 1); break;
    case EV_REFETCH: renew(e, ev->v); break;
    case EV_ADOPT:
        for (int k = 0; k < FE_NV; k++) {
            if (ev->v < FE_NV && k != ev->v) continue;
            fe_voice *w = &e->voices[k];
            if (!w->next) continue;
            if (w->env_stage == ENV_OFF) do_adopt(e, w); else { w->adopt = 1; w->swapping = 1; }
        }
        break;
    case EV_FREEZE:  v->frozen = ev->val > 0.5f; break;
    case EV_SET: {
        float old = v->p[ev->id];
        v->p[ev->id] = ev->val;
        if (ev->id == FE_P_START || ev->id == FE_P_LENGTH) { compute_region(e, v); keep_in_region(v); }
        if ((ev->id == FE_P_PLACE || ev->id == FE_P_SOURCE) && step_of(ev->id, old) != step_of(ev->id, ev->val)
            && !v->frozen) {
            if (ev->id == FE_P_PLACE) v->drift_km = 0;          /* a new home */
            drop_next(e, v);
            request_harvest(e, ev->v);   /* a new place: go and listen there */
        }
        break; }
    case EV_GSET: e->g[ev->id] = ev->val; break;
    case EV_GEST:
        if (ev->v >= FE_NV) e->gx[ev->id] = ev->val;
        else v->vx[ev->id] = ev->val;
        break;
    case EV_XPARAM: e->xp[ev->id / FE_X_NPARAM][ev->id % FE_X_NPARAM] = ev->val; break;
    case EV_XRESET: memcpy(e->xp[ev->id], X_DEF[ev->id], sizeof X_DEF[0]); break;
    }
}

/* ── tape read ──────────────────────────────────────────────────────────── */

static inline void rd(const fe_tape *t, float pos, float *l, float *r) {
    int n = t->frames;
    if (pos < 0) pos = 0;
    int i = (int)pos;
    if (i >= n - 1) { i = n - 2; pos = (float)i + 1.0f; }
    float f = pos - (float)i;
    const int16_t *p = t->pcm + 2 * i;
    const float k = 1.0f / 32768.0f;
    *l = ((float)p[0] + ((float)p[2] - (float)p[0]) * f) * k;
    *r = ((float)p[1] + ((float)p[3] - (float)p[1]) * f) * k;
}

static float drift_step(fe_drift *d, float k) {
    d->v += (fe_randf(&d->rng) - 0.5f) * k;
    d->v *= 0.995f;
    d->x += d->v;
    if (d->x > 1) { d->x = 1; d->v = -fabsf(d->v); }
    if (d->x < -1) { d->x = -1; d->v = fabsf(d->v); }
    return d->x;
}

/* ── gestures, once per block per voice ─────────────────────────────────── */

static float tape_len_ok(const fe_tape *t) { return (float)(t->frames > 64 ? t->frames : 64); }

/* Targets, smoothing and onsets. Runs for every voice, playing or not. */
/* Terne lines: our five families. Each strike k of n gives the gap to the next one and its height. */
static void t_line_step(int fam, float line, int k, int n, float d, uint32_t *rng, float *ivl, float *amp) {
    switch (fam) {
    case 0: { float r = 0.45f + 0.45f * line;                                   /* Rebond: closer and lower */
              *ivl = d * powf(r, (float)k); *amp = powf(0.55f + 0.4f * line, (float)k); break; }
    case 1:   *ivl = d * (0.2f + 0.6f * line); *amp = k == 0 ? 1.0f : 0.7f; break;   /* Rafale: even */
    case 2: { int gs = 2 + (int)(line * 2.99f), p = k % gs;                     /* Begaiement: groups, then a gap */
              *ivl = p == gs - 1 ? d * (1.5f + 2.0f * line) : d * 0.18f;
              *amp = p == 0 ? 1.0f : 0.6f * powf(0.8f, (float)p); break; }
    case 3:   *ivl = d * (0.6f + 0.8f * line);                                  /* Vague: swells and ebbs */
              *amp = 0.2f + 0.8f * sinf((float)M_PI * ((float)k + 0.5f) / (float)(n > 0 ? n : 1)); break;
    default:  *ivl = d * -logf(1.0f - 0.98f * fe_randf(rng)) * (0.4f + line);   /* Pluie: scattered */
              *amp = 0.3f + 0.7f * fe_randf(rng); break;
    }
    if (*ivl < 0.012f) *ivl = 0.012f;
}
static float timbre_dist(const fe_card *a, const fe_card *b, float wx, float wy) {
    float ca = log2f(fmaxf(a->centroid_hz, 50.0f) / 1000.0f) / 3.0f, cb = log2f(fmaxf(b->centroid_hz, 50.0f) / 1000.0f) / 3.0f;
    float dc = ca - cb, dl = (a->loudness - b->loudness) * 5.0f;
    return sqrtf(wx * dc * dc + wy * dl * dl);
}
static float eff(const fe_engine *e, const fe_voice *v, int g) { return fmaxf(e->gx[g], v->vx[g]); }

static void gestures_block(fe_engine *e, fe_voice *v, int vi, int n, int *bong_rank, int *past_rank, int pv) {
    const float sr = e->sr, dt = (float)n / sr;
    const float ks = 1.0f - expf(-(float)n / (0.02f * sr));          /* ~20 ms */
    const int playing = v->env_stage != ENV_OFF;
    for (int g = 0; g < FE_X_COUNT; g++) {
        if (!X_READY[g]) continue;
        float T = fmaxf(e->gx[g], v->vx[g]);
        float prev = v->xt_prev[g];
        v->xt_prev[g] = T;
        v->xs[g] += (T - v->xs[g]) * ks;
        if (v->xs[g] < 1e-4f && T <= 0) v->xs[g] = 0;

        switch (g) {
        case FE_X_BONGO:
            if (T > 0 && prev <= 0 && playing) {                       /* a hit */
                v->bong_peak = T; v->bong_min = T; v->bong_stage = 1;
                v->bong_delay = (int)(mx_bong_off(e->xp[g][3]) * (float)(*bong_rank)++ * sr);
            } else if (T > 0 && playing) {                             /* pressing harder hits again */
                if (T < v->bong_min) v->bong_min = T;
                if (T > v->bong_min + 0.25f) { v->bong_peak = T; v->bong_min = T; v->bong_stage = 1; v->bong_delay = 0; }
            }
            break;
        case FE_X_GEL:
            if (T > 0 && playing && !v->gel_on && v->tape->frames > 64) {   /* hold the instant */
                v->gel_on = 1;
                v->gel_org = v->pos;
                v->gel_ph[0] = 0; v->gel_ph[1] = 0.5f;
                v->gel_go[0] = v->gel_go[1] = v->pos;
            } else if (T <= 0) v->gel_on = 0;
            break;
        case FE_X_DERIVE:
            if (T > 0 && prev <= 0 && playing && !v->frozen) {
                v->drift_armed = 1; v->drift_wait = 0.3f; v->drift_peak = T;  /* let the pressure settle */
                v->drift_hop = 0;
            }
            if (v->drift_armed && T > 0) {
                v->drift_peak = fmaxf(v->drift_peak, T);
                v->drift_wait -= dt;
                if (v->drift_wait <= 0) {
                    float km = mx_drift_km(v->drift_peak);
                    float dir = e->xp[g][0];
                    float deg = dir < 0.04f ? 360.0f * fe_randf(&v->rng)
                                            : (dir - 0.04f) / 0.96f * 360.0f + (fe_randf(&v->rng) - 0.5f) * 30.0f;
                    /* the first hop leaves from home; later hops walk on from where the voice is */
                    const float rad = (float)(M_PI / 180.0);
                    float x = km * sinf(deg * rad), y = km * cosf(deg * rad);
                    if (v->drift_hop > 0) { x += v->drift_km * sinf(v->drift_deg * rad); y += v->drift_km * cosf(v->drift_deg * rad); }
                    v->drift_km = sqrtf(x * x + y * y);
                    v->drift_deg = atan2f(x, y) * (float)(180.0 / M_PI);
                    if (v->drift_deg < 0) v->drift_deg += 360.0f;
                    v->drift_redo = 1;                                 /* asked as soon as no harvest is waiting */
                    float hop = mx_drift_hop(e->xp[g][2]);
                    if (hop > 0) { v->drift_wait = hop; v->drift_hop += 1; v->drift_peak = T; }
                    else v->drift_armed = 0;
                }
            }
            if (T <= 0 && prev > 0) {                                  /* let go */
                v->drift_armed = 0;
                if (e->xp[g][3] >= 0.5f && v->drift_km > 0 && !v->frozen) {
                    v->drift_km = 0;
                    v->drift_redo = 1;                                 /* back home */
                }
            }
            break;
        case FE_X_PASSE:
            if (T > 0 && prev <= 0) {
                v->past_on = 1;
                v->past_delay = (int)(e->xp[g][3] * (float)(*past_rank)++ * sr);
                if (e->xp[g][0] >= 0.5f && v->past) v->past_pos = clampf(v->p[FE_P_START], 0, 0.98f) * (float)v->past->frames;
            } else if (T <= 0) v->past_on = 0;
            if (v->past_delay > 0) v->past_delay -= n;
            break;
        case FE_X_FUTUR:
            if (T > 0 && !v->next && !v->want_next && !atomic_load(&v->want) && !atomic_load(&v->searching)
                && pv != vi && v->tape->frames > 64 && e->nfree > 0) {
                v->want_next = 1;                                      /* go and prepare what comes next */
                request_harvest(e, vi);
            }
            break;
        case FE_X_VOISIN:
            if (T > 0 && playing && (prev <= 0 || fabsf(T - v->vn_T) > 0.05f)) {   /* choose the neighbour in timbre */
                v->vn_T = T;
                const float wx = 0.05f + e->xp[g][0], wy = 0.05f + e->xp[g][1];
                const int same = e->xp[g][3] >= 0.5f;
                float d[FE_NV], dmax = 0; int any = 0;
                for (int u = 0; u < FE_NV; u++) {
                    d[u] = -1;
                    if (u == vi || e->voices[u].tape->frames < 64) continue;
                    if (same) {
                        float dx = (e->vlon[u] - e->vlon[vi]) * 78.0f, dy = (e->vlat[u] - e->vlat[vi]) * 110.6f;
                        if (dx * dx + dy * dy > 9.0f) continue;               /* within 3 km */
                    }
                    d[u] = timbre_dist(&v->tape->card, &e->voices[u].tape->card, wx, wy);
                    if (d[u] > dmax) dmax = d[u];
                    any = 1;
                }
                if (any) {
                    float want = T * dmax; int best = -1; float bd = 1e9f;
                    for (int u = 0; u < FE_NV; u++) if (d[u] >= 0 && fabsf(d[u] - want) < bd) { bd = fabsf(d[u] - want); best = u; }
                    v->vn_want = best;
                    if (v->vn_u < 0 || v->vn_mix <= 0) v->vn_u = best;
                }
            }
            break;
        default: break;
        }
    }

    /* Etirement: one factor for every time of the gesture (1 = untouched) */
    {
        float a = v->xs[FE_X_ETIREMENT];
        if (a > 0.001f) {
            const float *X = e->xp[FE_X_ETIREMENT];
            float lo = mx_st_min(X[0]), hi = mx_st_max(X[1]);
            float ac = powf(a, 0.25f + 3.0f * X[2] * X[2] * 1.2f);
            float m = fminf(1.0f, a * 10.0f);
            v->tf = 1.0f + (lo * powf(hi / lo, ac) - 1.0f) * m;
        } else v->tf = 1.0f;
    }

    /* the terne engine: Terne articulates, Double hasard draws the lines */
    {
        const float Tt = eff(e, v, FE_X_TERNE), Td = eff(e, v, FE_X_DOUBLE);
        const float *XT = e->xp[FE_X_TERNE], *XD = e->xp[FE_X_DOUBLE];
        const int active = (Tt > 0 || Td > 0) && playing;
        if (active && !v->t_on) {
            v->t_on = 1; v->t_k = 0; v->t_wait = 0;
            v->t_fam = t_fam_of(XT[0]); v->t_line = XT[1];
            v->t_rng = Td > 0 ? 0x9E3779B9u + (uint32_t)(XD[1] * 99.0f + 0.5f) * 7919u + (uint32_t)vi * 104729u
                              : v->rng ^ 0x5bd1e995u;
        }
        if (!active) v->t_on = 0;
        if (v->t_on) {
            v->t_wait -= n;
            if (v->t_wait <= 0) {
                const int N = Tt > 0 ? 1 + (int)(Tt * 11.0f + 0.5f) : 6;
                if (v->t_k >= N) v->t_k = 0;
                if (Td > 0) {                                          /* at each strike, a draw */
                    float r = fe_randf(&v->t_rng);
                    if (r >= XD[2]) {                                  /* the draw's memory: keep the last one */
                        if (fe_randf(&v->t_rng) < XD[3]) { v->t_fam = t_fam_of(XT[0]); v->t_line = XT[1]; }
                        else {
                            v->t_line = clampf(XT[1] + (fe_randf(&v->t_rng) * 2.0f - 1.0f) * Td, 0, 1);
                            if (fe_randf(&v->t_rng) < (Td - 0.35f) / 0.65f)
                                v->t_fam = (int)(fe_rand(&v->t_rng) % (uint32_t)(1 + t_fam_of(XD[0])));
                        }
                    }
                } else { v->t_fam = t_fam_of(XT[0]); v->t_line = XT[1]; }
                const float d = mx_t_delay(XT[2]) * v->tf;
                float ivl, amp;
                t_line_step(v->t_fam, v->t_line, v->t_k, N, d, &v->t_rng, &ivl, &amp);
                v->t_amp = amp; v->t_stage = 1;
                v->t_k++;
                if (v->t_k >= N) ivl += 2.0f * d;                      /* a breath between two lines */
                v->t_wait += (int)(ivl * sr);
                if (v->t_wait < 1) v->t_wait = 1;
            }
        }
    }
}

/* Hesitation: one pulse for everyone, which hesitates; at each pulse a voice may play again */
static void hesitation_pulse(fe_engine *e) {
    const float *X = e->xp[FE_X_HESITATION];
    for (int vi = 0; vi < FE_NV; vi++) {
        fe_voice *v = &e->voices[vi];
        float dens = eff(e, v, FE_X_HESITATION);
        if (dens <= 0 || v->tape->frames < 64) continue;
        if (v->env_stage == ENV_RISE || v->env_stage == ENV_SUSTAIN) {
            if (fe_randf(&e->hrng) >= dens) continue;
            if (v->t_on) { v->t_k = 0; v->t_wait = 0; }                 /* a terne line starts again */
            else if (!v->swapping) { v->reseek = 1; v->swapping = 1; }  /* a short dip, then from Depart */
        } else if (v->env_stage == ENV_OFF && !v->frozen) {
            if (fe_randf(&e->hrng) >= dens * X[2]) continue;           /* closed sounds join, if allowed */
            v->env = 0; reset_dsp(v);
            compute_region(e, v); start_position(v, 1);
            v->vel = 0.45f + 0.4f * fe_randf(&e->hrng);
            v->env_stage = ENV_RISE;
            v->auto_close = (int)(0.25f * v->tf * e->sr);
        }
    }
    float P = 60.0f / mx_hes_bpm(X[0]) * e->hes_mult;
    P *= 1.0f + X[1] * (fe_randf(&e->hrng) * 2.0f - 1.0f) * 0.6f;
    if (fe_randf(&e->hrng) < X[1] * 0.25f) P *= 2.0f;                  /* it hesitates */
    if (++e->hes_n % 4 == 0) {                                          /* it tips: half or double time */
        if (fe_randf(&e->hrng) < X[3] * 0.5f) { static const float M[3] = { 0.5f, 1.0f, 2.0f }; e->hes_mult = M[fe_rand(&e->hrng) % 3]; }
        else if (fe_randf(&e->hrng) < 0.5f) e->hes_mult = 1.0f;
    }
    e->hes_wait += P;
}

/* ── one voice, one block ───────────────────────────────────────────────── */

static void render_voice(fe_engine *e, fe_voice *v, int vi, float *ml, float *mr, float *sl, float *sr_,
                         float *rl, float *rr, int n) {
    const float sr = e->sr, dt = 1.0f / sr;
    fe_tape *t = v->tape;
    if (t->frames < 64) { v->env_stage = ENV_OFF; return; }
    const float tf = v->tf;                            /* Etirement: every time of the gesture */

    /* block constants */
    const int   hold = step_of(FE_P_HOLD, v->p[FE_P_HOLD]);
    const int   rev  = step_of(FE_P_DIR, v->p[FE_P_DIR]);
    const float strike = v->p[FE_P_STRIKE];
    const float rise_inc = 1.0f / (m_rise(v->p[FE_P_RISE]) * tf * sr);
    const float fall_k = 1.0f - expf(-6.9f / (m_fall(v->p[FE_P_FALL]) * tf * sr));
    const float vac_up = 1.0f - expf(-1.0f / ((0.004f + 0.02f * (1 - strike)) * sr));
    const float vac_dn = 1.0f - expf(-1.0f / ((0.06f + 0.2f * (1 - strike)) * sr));
    const float amp_exp = 1.6f - 0.9f * strike;
    const float velc = 0.35f + 0.65f * v->vel;
    const float drift = v->p[FE_P_DRIFT];

    /* per block drifts: pitch ±3 %, pan ±0.4, region wander */
    float dp = drift_step(&v->dpitch, 0.02f) * drift;
    float dpan = drift_step(&v->dpan, 0.02f) * drift;
    float spd = m_speed(v->p[FE_P_SPEED]) * (1.0f + 0.03f * dp);
    if (rev) spd = -spd;
    const float st_link = e->xp[FE_X_ETIREMENT][3];
    const float spd_adv = spd / tf;                    /* how fast the head moves through the fragment */
    const float spd_pitch = spd / powf(tf, st_link);   /* how high it sounds: kept unless linked */
    const int   st_live = fabsf(tf - 1.0f) > 0.01f && st_link < 0.98f;
    if (hold == 1) {                                   /* Derive: the region itself wanders */
        float w = drift_step(&v->dpos, 0.03f) * (0.2f + drift) / tf;
        float room = (float)t->frames - v->loop_len;
        if (room > 1) {
            v->loop_a = clampf(v->loop_a + w * 0.002f * sr * (float)n / 128.0f, 0, room);
            keep_in_region(v);
        }
    }
    const float xf = fminf(0.02f * sr, v->loop_len * 0.25f);
    const float grain_len = fminf(m_grain(v->p[FE_P_GRAIN]) * tf * sr, v->loop_len);
    const float g_inc = 1.0f / fmaxf(grain_len, 16.0f) * fabsf(spd_pitch);
    const float scatter = v->p[FE_P_SCATTER];

    /* colour */
    const float lock = v->p[FE_P_LOCK];
    float lock_hz = clampf(t->card.centroid_hz, 180.0f, 5000.0f);
    const float lock_f = 2.0f * sinf((float)M_PI * lock_hz / sr);
    const float lock_q = 1.0f / (1.0f + lock * 24.0f);
    const float wear = v->wear;
    const float wlp_a = 1.0f - expf(-2.0f * (float)M_PI * 18000.0f * powf(0.06f, wear) / sr);
    const float tone = (v->p[FE_P_TONE] - 0.5f) * 2.0f;
    const float tlp_a = 1.0f - expf(-2.0f * (float)M_PI * 800.0f / sr);
    const float g_lo = powf(4.0f, -tone * 0.5f), g_hi = powf(4.0f, tone * 0.5f);
    const float drive_k = v->p[FE_P_DRIVE] * 8.0f;
    const float width = v->p[FE_P_WIDTH] * 1.5f;
    const float pan = clampf((v->p[FE_P_PAN] - 0.5f) * 2.0f + 0.4f * dpan, -1, 1);
    const float pg_l = fminf(1.0f, 1.0f - pan), pg_r = fminf(1.0f, 1.0f + pan);
    const float vol = v->p[FE_P_VOLUME] * v->p[FE_P_VOLUME];
    const float send = v->p[FE_P_POOL];
    const float duck_step = 1.0f / (0.015f * sr);
    float amp = 0, lpa = 0;

    /* ── gestures, block constants ── */
    const float (*XP)[FE_X_NPARAM] = e->xp;
    /* Bongo: while held the voice sits low in its LPG, and each hit strikes it */
    const float b_duck = 1.0f - 0.75f * v->xs[FE_X_BONGO];
    const float b_rise = 1.0f / (mx_bong_rise(XP[FE_X_BONGO][0]) * tf * sr);
    const float b_fall = expf(-6.9f / (mx_bong_fall(XP[FE_X_BONGO][1]) * tf * sr));
    const float b_gain = 0.3f + 1.0f * powf(fmaxf(v->bong_peak, 1e-3f), 1.8f - 1.5f * XP[FE_X_BONGO][2]);
    /* Gel */
    const int   gel_live = v->gel_on || v->gel_mix > 0;
    if (v->xs[FE_X_GEL] > 0.01f) v->gel_breath = v->xs[FE_X_GEL];     /* the size stays where the hand left it */
    const float gel_len = fminf(mx_gel_size(v->gel_breath) * sr, tape_len_ok(t) * 0.5f);
    const float gel_inc = fmaxf(fabsf(spd), 0.25f) / fmaxf(gel_len, 16.0f);
    const float gel_in = 1.0f / (mx_gel_fade(XP[FE_X_GEL][2]) * tf * sr), gel_out = 1.0f / (mx_gel_fade(XP[FE_X_GEL][3]) * tf * sr);
    static const float gel_rate = 0.21f;
    float gel_amp = 1.0f;
    if (gel_live) {
        v->gel_org += (fe_randf(&v->rng) - 0.5f) * XP[FE_X_GEL][1] * gel_len * 0.05f;   /* the held instant wanders a little */
        v->gel_org = clampf(v->gel_org, 0, (float)t->frames - gel_len - 2);
        float ph = (float)(v->since_swap * (gel_rate + 0.013f * (float)vi)) * 2.0f * (float)M_PI;
        gel_amp = 1.0f - XP[FE_X_GEL][0] * 0.7f * (0.5f + 0.5f * sinf(ph));
    }
    /* Accroche: a band-pass that follows a frequency present in the fragment */
    const float acc_a = v->xs[FE_X_ACCROCHE];
    const int   acc_live = acc_a > 0.001f;
    float acc_w = 0, acc_k = 1, acc_g1 = 0, acc_g2 = 0, acc_g3 = 0, acc_mk = 0, acc_tk = 0, acc_lpk = 0;
    if (acc_live) {
        if (v->acc_hz < 40) v->acc_hz = clampf(t->card.centroid_hz, 80.0f, 4000.0f);
        float fc = clampf(v->acc_hz * powf(2.0f, mx_acc_oct(XP[FE_X_ACCROCHE][0])), 40.0f, 0.42f * sr);
        float qmax = 3.0f + 57.0f * (1.0f - XP[FE_X_ACCROCHE][1]);
        float q = 0.7f + acc_a * (qmax - 0.7f);
        float gg = tanf((float)M_PI * fc / sr);
        acc_k = 1.0f / q;
        acc_g1 = 1.0f / (1.0f + gg * (gg + acc_k)); acc_g2 = gg * acc_g1; acc_g3 = gg * acc_g2;
        acc_mk = acc_k * fminf(4.0f, 0.8f * sqrtf(q));                 /* unity peak, a little make-up */
        acc_w = XP[FE_X_ACCROCHE][3] * fminf(1.0f, acc_a * 4.0f);
        acc_tk = mx_acc_track(XP[FE_X_ACCROCHE][2]) * sr;
        acc_lpk = 1.0f - expf(-2.0f * (float)M_PI * 2000.0f / sr);
    }
    /* Contagion: borrow from the other voices, weighted by their distance on the map */
    const float c_a = v->xs[FE_X_CONTAGION];
    const int   c_live = c_a > 0.001f && e->vbuf;
    float bl_raw[512], br_raw[512], bl_post[512], br_post[512];
    float c_raw = 0, c_post = 0;
    if (c_live) {
        memset(bl_raw, 0, sizeof(float) * (size_t)n); memset(br_raw, 0, sizeof(float) * (size_t)n);
        memset(bl_post, 0, sizeof(float) * (size_t)n); memset(br_post, 0, sizeof(float) * (size_t)n);
        const float range = mx_cont_km(XP[FE_X_CONTAGION][0]);
        const float cexp = 0.5f + 2.5f * XP[FE_X_CONTAGION][1];
        const float stereo = XP[FE_X_CONTAGION][3];
        const float coslat = cosf(e->vlat[vi] * (float)(M_PI / 180.0));
        float wsum = 0;
        for (int u = 0; u < FE_NV; u++) {
            if (u == vi || !e->vrend[u]) continue;
            float dx = (e->vlon[u] - e->vlon[vi]) * coslat * 111.32f, dy = (e->vlat[u] - e->vlat[vi]) * 110.57f;
            float d = sqrtf(dx * dx + dy * dy);
            float w = expf(-powf(d / range, cexp));
            if (w < 0.01f) continue;
            wsum += w;
            float p = clampf(dx / range, -1, 1) * stereo;              /* east sounds come from the right */
            float gl = w * (1.0f - 0.7f * p), gr = w * (1.0f + 0.7f * p), gs = w * (1.0f - 0.5f * fabsf(p));
            for (int k = 0; k < 2; k++) {
                const float *L = e->vbuf + ((size_t)(u * 2 + k) * 2 + 0) * 512, *R = L + 512;
                float *OL = k ? bl_post : bl_raw, *OR = k ? br_post : br_raw;
                for (int i = 0; i < n; i++) {
                    float m = (L[i] + R[i]) * 0.5f, s = (L[i] - R[i]) * 0.5f;
                    OL[i] += m * gl + s * gs; OR[i] += m * gr - s * gs;
                }
            }
        }
        if (wsum > 1.0f) {
            float k = 1.0f / wsum;
            for (int i = 0; i < n; i++) { bl_raw[i] *= k; br_raw[i] *= k; bl_post[i] *= k; br_post[i] *= k; }
        }
        float share = 0.85f * c_a * fminf(1.0f, wsum), src = XP[FE_X_CONTAGION][2];   /* nobody near: nothing taken */
        c_raw = share * (1.0f - src); c_post = share * src;
    }
    float *my_raw_l = e->vbuf ? e->vbuf + ((size_t)(vi * 2 + 0) * 2) * 512 : NULL;
    float *my_post_l = e->vbuf ? e->vbuf + ((size_t)(vi * 2 + 1) * 2) * 512 : NULL;
    /* Ressac: what this voice gives to the returning wave */
    const float rs_send = v->xs[FE_X_RESSAC];

    /* Etirement: two grains read at the kept pitch while the head crawls */
    const float st_len = 0.09f * sr;
    const float st_inc = fmaxf(fabsf(spd_pitch), 0.05f) / st_len;
    const float st_step = 1.0f / (0.015f * sr);
    /* Pulsars */
    const float pul_a = v->xs[FE_X_PULSARS];
    const int   pul_live = pul_a > 0.001f || v->pul_mix > 0;
    const float *XPu = XP[FE_X_PULSARS];
    const float pul_duty = 0.05f + 0.95f * XPu[0];
    const float pul_comp = fminf(3.0f, 1.0f / sqrtf(pul_duty * (XPu[2] < 0.33f ? 1.0f : 0.45f)));
    const float pul_tgt = pul_a > 0.001f ? 1.0f : 0.0f, pul_step = 1.0f / (0.01f * sr);
    /* Corps: six modes tuned on the place */
    const float co_a = v->xs[FE_X_CORPS];
    const int   co_live = co_a > 0.001f || v->corps_mix > 0;
    float co_c[6] = {0}, co_r2[6] = {0}, co_b0[6] = {0}, co_amp[6] = {0};
    if (co_live) {
        static const float RAT[3][6] = { { 1, 2, 3, 4, 5, 6 },                          /* corde */
                                         { 1, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f },  /* barre */
                                         { 1, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f } };/* plaque */
        const float *XC = XP[FE_X_CORPS];
        int pl = step_of(FE_P_PLACE, v->p[FE_P_PLACE]);
        float f0 = 110.0f * powf(2.0f, (float)((pl * 7) % 24) / 12.0f) * powf(2.0f, floorf((XC[0] - 0.5f) * 24.0f + 0.5f) / 12.0f);
        float mt = XC[1] * 2.0f; int m0 = mt >= 2 ? 1 : (int)mt; float mf = mt - (float)m0;
        float t60 = mx_corps_t60(co_a > 0.001f ? co_a : 0.3f) * tf;
        for (int m = 0; m < 6; m++) {
            float ratio = RAT[m0][m] + (RAT[m0 + 1 > 2 ? 2 : m0 + 1][m] - RAT[m0][m]) * mf;
            float f = f0 * ratio;
            if (f > 0.45f * sr) continue;
            float tm = t60 * powf(0.55f + 0.4f * XC[2], (float)m);
            float r = expf(-6.9f / (tm * sr));
            co_c[m] = 2.0f * r * cosf(2.0f * (float)M_PI * f / sr);
            co_r2[m] = r * r;
            co_b0[m] = (1.0f - r * r) * 0.5f;
            co_amp[m] = powf(0.35f + 0.6f * XC[2], (float)m);
        }
    }
    const float co_tgt = co_a > 0.001f ? XP[FE_X_CORPS][3] : 0.0f, co_step = 1.0f / (0.3f * sr);
    /* Passe */
    const fe_tape *pt = v->past;
    const int   pa_live = pt && pt->frames > 64 && (v->past_mix > 0 || v->past_on);
    const float pa_tgt = (v->past_on && v->past_delay <= 0) ? eff(e, v, FE_X_PASSE) : 0.0f;
    const float pa_step = 1.0f / (mx_fade(XP[FE_X_PASSE][1]) * tf * sr);
    const float pa_lpa = 1.0f - expf(-2.0f * (float)M_PI * 16000.0f * powf(0.04f, XP[FE_X_PASSE][2]) / sr);
    const float pa_lvl = 1.0f - 0.5f * XP[FE_X_PASSE][2];
    /* Futur */
    const fe_tape *nt = v->next;
    const float fu_tgt = nt ? eff(e, v, FE_X_FUTUR) : 0.0f;
    const int   fu_live = nt && nt->frames > 64 && (v->next_mix > 0 || fu_tgt > 0);
    const float fu_step = 1.0f / (mx_fade(XP[FE_X_FUTUR][0]) * tf * sr);
    const float fu_lpa = 1.0f - expf(-2.0f * (float)M_PI * fminf(300.0f * powf(60.0f, XP[FE_X_FUTUR][1]), 0.45f * sr) / sr);
    /* Voisin de timbre */
    const fe_tape *vt = v->vn_u >= 0 ? e->voices[v->vn_u].tape : NULL;
    const float vn_tgt = (eff(e, v, FE_X_VOISIN) > 0 && v->vn_u >= 0 && v->vn_want == v->vn_u) ? 1.0f : 0.0f;
    const int   vn_live = vt && vt->frames > 64 && (v->vn_mix > 0 || vn_tgt > 0);
    const float vn_step = 1.0f / (mx_fade(XP[FE_X_VOISIN][2]) * tf * sr);
    /* Reecriture */
    const float rw_a = v->xs[FE_X_REECRITURE];
    const int   rw_live = rw_a > 0.001f && hold != 2;
    const float *XR = XP[FE_X_REECRITURE];
    const float rw_lpa = 1.0f - expf(-2.0f * (float)M_PI * 18000.0f * powf(0.05f, XR[0]) / sr);
    const float rw_rate = rw_a * 0.25f;
    const int   rw_mix = XR[3] >= 0.5f && e->lastmix;
    /* ternes */
    const int   t_live = v->t_on || v->t_mix > 0;
    const float t_rise = 1.0f / (0.001f * powf(60.0f, XP[FE_X_TERNE][3]) * tf * sr);
    const float t_fall = expf(-6.9f / (0.04f * powf(25.0f, XP[FE_X_TERNE][3]) * tf * sr));
    const float t_tgt = v->t_on ? 1.0f : 0.0f, t_step = 1.0f / (0.01f * sr);

    for (int i = 0; i < n; i++) {
        /* envelope */
        switch (v->env_stage) {
        case ENV_RISE: v->env += rise_inc; if (v->env >= 1) { v->env = 1; v->env_stage = ENV_SUSTAIN; } break;
        case ENV_FALL: v->env -= v->env * fall_k + 1e-7f;
                       if (v->env <= 1e-4f && v->vac < 1e-4f) { v->env = 0; v->env_stage = ENV_OFF; } break;
        default: break;
        }
        if (v->env_stage == ENV_OFF) {                           /* silent from here: keep the borrow buffers clean */
            if (my_raw_l) for (int j = i; j < n; j++) { my_raw_l[j] = my_raw_l[512 + j] = my_post_l[j] = my_post_l[512 + j] = 0; }
            break;
        }
        if (v->env < 0) v->env = 0;

        /* read */
        const float gm = v->gel_mix;
        float l, r;
        if (hold == 2) {                                         /* Grains: two Hann windows, half a grain apart */
            l = r = 0;
            for (int g = 0; g < 2; g++) {
                float off = v->grain_ph[g] * grain_len;
                float p = rev ? v->grain_org[g] - off : v->grain_org[g] + off;
                float a = v->loop_a, b = v->loop_a + v->loop_len;
                while (p >= b) p -= v->loop_len;
                while (p < a) p += v->loop_len;
                float gl, gr; rd(t, p, &gl, &gr);
                float x = v->grain_ph[g], q = x * (1.0f - x);
                float s = 16.0f * q / (5.0f - 4.0f * q); s *= s;      /* Bhaskara sin(pi x), squared: Hann */
                l += gl * s; r += gr * s;
                v->grain_ph[g] += g_inc;
                if (v->grain_ph[g] >= 1.0f) {
                    v->grain_ph[g] -= 1.0f;
                    float step = (1.0f - scatter) * grain_len * 0.5f * (1.0f - gm) / tf;
                    float jump = scatter * fe_randf(&v->rng) * v->loop_len * (1.0f - gm);
                    float o = v->grain_org[g] + (rev ? -(step + jump) : (step + jump));
                    while (o >= b) o -= v->loop_len;
                    while (o < a) o += v->loop_len;
                    v->grain_org[g] = o;
                }
            }
        } else {                                                 /* Boucle / Derive: crossfaded loop */
            float a = v->loop_a, b = v->loop_a + v->loop_len;
            rd(t, v->pos, &l, &r);
            float d = rev ? (v->pos - a) : (b - v->pos);
            if (d < xf && xf > 1) {
                float k = d / xf, l2, r2;
                rd(t, rev ? v->pos + v->loop_len : v->pos - v->loop_len, &l2, &r2);
                float g1 = sqrtf(k), g2 = sqrtf(1 - k);
                l = l * g1 + l2 * g2; r = r * g1 + r2 * g2;
            }
            if (rw_live) {                                       /* Reecriture: the head writes back what it read, worn */
                int idx = (int)v->pos;
                if (idx >= 0 && idx < t->frames) {
                    int16_t *pc = t->pcm + 2 * idx;
                    for (int c = 0; c < 2; c++) {
                        float src = rw_mix ? e->lastmix[i] * 2.0f : (float)pc[c] * (1.0f / 32768.0f);
                        v->rw_lp[c] = flush(v->rw_lp[c] + rw_lpa * (src - v->rw_lp[c]));
                        float d = v->rw_lp[c];
                        if (v->rw_hole > 0) d *= 0.05f;
                        float nw = (float)pc[c] * (1.0f / 32768.0f) * (1.0f - rw_rate) + d * rw_rate;
                        nw = clampf(nw, -1.0f, 1.0f);
                        pc[c] = (int16_t)(nw * 32767.0f);
                    }
                    if (fe_randf(&v->rng) < XR[1] * XR[1] * 0.004f * rw_a) {        /* a crackle */
                        float k = (fe_randf(&v->rng) - 0.5f) * 1.2f * rw_a;
                        pc[0] = (int16_t)clampf((float)pc[0] + k * 32767.0f, -32767, 32767);
                        pc[1] = (int16_t)clampf((float)pc[1] + k * 26000.0f, -32767, 32767);
                    }
                    if (v->rw_hole > 0) v->rw_hole--;
                    else if (fe_randf(&v->rng) < XR[2] * 0.0004f * rw_a) v->rw_hole = (int)((0.005f + 0.075f * fe_randf(&v->rng)) * sr);
                }
            }
            if (st_live || v->st_mix > 0) {                      /* Etirement: grains at the kept pitch */
                v->st_mix += ((st_live ? 1.0f : 0.0f) - v->st_mix) > 0 ? fminf(st_step, 1.0f - v->st_mix) : -fminf(st_step, v->st_mix);
                float gl = 0, gr = 0;
                for (int g = 0; g < 2; g++) {
                    float off = v->st_ph[g] * st_len;
                    float p = spd_pitch < 0 ? v->st_org[g] - off : v->st_org[g] + off;
                    while (p >= b) p -= v->loop_len;
                    while (p < a) p += v->loop_len;
                    float x1, x2; rd(t, p, &x1, &x2);
                    float x = v->st_ph[g], q = x * (1.0f - x);
                    float w = 16.0f * q / (5.0f - 4.0f * q); w *= w;
                    gl += x1 * w; gr += x2 * w;
                    v->st_ph[g] += st_inc;
                    if (v->st_ph[g] >= 1.0f) { v->st_ph[g] -= 1.0f; v->st_org[g] = v->pos; }
                }
                float k = v->st_mix, g1 = sqrtf(1.0f - k), g2 = sqrtf(k);
                l = l * g1 + gl * g2; r = r * g1 + gr * g2;
            }
            v->pos += spd_adv * (1.0f - gm);                     /* Gel holds the head still; Etirement slows it */
            if (v->pos >= b) v->pos -= v->loop_len;
            if (v->pos < a) v->pos += v->loop_len;
        }

        /* Voisin de timbre: glide to a fragment close in timbre, read at the same place in it */
        if (vn_live) {
            v->vn_mix += vn_tgt > v->vn_mix ? fminf(vn_step, vn_tgt - v->vn_mix) : -fminf(vn_step, v->vn_mix - vn_tgt);
            float F = (float)vt->frames;
            while (v->vn_pos >= F - 1) v->vn_pos -= F - 1;
            while (v->vn_pos < 0) v->vn_pos += F - 1;
            float x1, x2; rd(vt, v->vn_pos, &x1, &x2);
            v->vn_pos += spd_adv;
            float k = v->vn_mix, g1 = sqrtf(1.0f - k), g2 = sqrtf(k);
            l = l * g1 + x1 * g2; r = r * g1 + x2 * g2;
        }
        /* Passe: the fragment it played before comes back */
        if (pa_live) {
            v->past_mix += pa_tgt > v->past_mix ? fminf(pa_step, pa_tgt - v->past_mix) : -fminf(pa_step, v->past_mix - pa_tgt);
            float F = (float)pt->frames;
            while (v->past_pos >= F - 1) v->past_pos -= F - 1;
            while (v->past_pos < 0) v->past_pos += F - 1;
            float x[2]; rd(pt, v->past_pos, &x[0], &x[1]);
            v->past_pos += spd_adv;
            for (int c = 0; c < 2; c++) { v->past_lp[c] = flush(v->past_lp[c] + pa_lpa * (x[c] - v->past_lp[c])); x[c] = v->past_lp[c] * pa_lvl; }
            float k = v->past_mix, g1 = sqrtf(1.0f - k), g2 = sqrtf(k);
            l = l * g1 + x[0] * g2; r = r * g1 + x[1] * g2;
        }
        /* Futur: the fragment prepared next, heard through, a little veiled */
        if (fu_live) {
            v->next_mix += fu_tgt > v->next_mix ? fminf(fu_step, fu_tgt - v->next_mix) : -fminf(fu_step, v->next_mix - fu_tgt);
            float F = (float)nt->frames;
            while (v->next_pos >= F - 1) v->next_pos -= F - 1;
            while (v->next_pos < 0) v->next_pos += F - 1;
            float x[2]; rd(nt, v->next_pos, &x[0], &x[1]);
            v->next_pos += spd_adv;
            for (int c = 0; c < 2; c++) { v->next_lp[c] = flush(v->next_lp[c] + fu_lpa * (x[c] - v->next_lp[c])); x[c] = v->next_lp[c]; }
            float k = v->next_mix;
            l = l * (1.0f - 0.5f * k) + x[0] * k; r = r * (1.0f - 0.5f * k) + x[1] * k;
        }

        /* Gel: two Hann grains on the held instant, crossfaded over the playing sound */
        if (gel_live) {
            if (v->gel_on) { v->gel_mix += gel_in; if (v->gel_mix > 1) v->gel_mix = 1; }
            else { v->gel_mix -= gel_out; if (v->gel_mix < 0) v->gel_mix = 0; }
            float gl = 0, gr = 0;
            for (int g = 0; g < 2; g++) {
                float p = v->gel_go[g] + v->gel_ph[g] * gel_len;
                float a, b; rd(t, p, &a, &b);
                float x = v->gel_ph[g], q = x * (1.0f - x);
                float s = 16.0f * q / (5.0f - 4.0f * q); s *= s;
                gl += a * s; gr += b * s;
                v->gel_ph[g] += gel_inc;
                if (v->gel_ph[g] >= 1.0f) {
                    v->gel_ph[g] -= 1.0f;
                    float jit = (fe_randf(&v->rng) - 0.5f) * XP[FE_X_GEL][1] * gel_len;
                    v->gel_go[g] = clampf(v->gel_org + jit, 0, (float)t->frames - gel_len - 2);
                }
            }
            float k = v->gel_mix, g1 = sqrtf(1.0f - k), g2 = sqrtf(k) * gel_amp;
            l = l * g1 + gl * g2; r = r * g1 + gr * g2;
        }

        /* Pulsars: trains of short windows cut into the sound */
        if (pul_live) {
            v->pul_mix += pul_tgt > v->pul_mix ? fminf(pul_step, pul_tgt - v->pul_mix) : -fminf(pul_step, v->pul_mix - pul_tgt);
            v->pul_ph += 1.0f / fmaxf(v->pul_per, 2.0f);
            if (v->pul_ph >= 1.0f || v->pul_per <= 0) {
                v->pul_ph = v->pul_per <= 0 ? 0 : v->pul_ph - 1.0f;
                float f = mx_pul_hz(fmaxf(pul_a, 0.001f));
                v->pul_per = sr / f * (1.0f + (1.0f - XPu[1]) * 0.8f * (fe_randf(&v->rng) * 2.0f - 1.0f));
                v->pul_mask = fe_randf(&v->rng) < XPu[3] * 0.9f;
            }
            float x = v->pul_ph / pul_duty, w = 0;
            if (x < 1.0f && !v->pul_mask) {
                float edge = fminf(x, 1.0f - x) * pul_duty * v->pul_per / (0.0007f * sr);
                if (XPu[2] < 0.33f) w = fminf(1.0f, edge);                               /* carree */
                else if (XPu[2] < 0.67f) { float q = x * (1.0f - x); w = 16.0f * q / (5.0f - 4.0f * q); w *= w; } /* cloche */
                else w = expf(-5.0f * x) * fminf(1.0f, x * pul_duty * v->pul_per / (0.0003f * sr));        /* eclat */
            }
            float g = 1.0f - v->pul_mix + v->pul_mix * w * pul_comp;
            l *= g; r *= g;
        }

        /* Accroche: follow the period of the fragment (zero crossings of a low-passed copy) */
        if (acc_live) {
            float m = (l + r) * 0.5f;
            v->acc_lp += acc_lpk * (m - v->acc_lp);
            v->acc_cnt += 1.0f;
            if (!v->acc_arm) { if (v->acc_lp < -0.002f) v->acc_arm = 1; }
            else if (v->acc_lp > 0.002f) {
                float f = sr / fmaxf(v->acc_cnt, 2.0f);
                if (f > 50.0f && f < 5000.0f) {
                    float k = 1.0f - expf(-v->acc_cnt / acc_tk);
                    v->acc_hz = expf(logf(v->acc_hz) + (logf(f) - logf(v->acc_hz)) * k);
                }
                v->acc_arm = 0; v->acc_cnt = 0;
            }
            if (v->acc_cnt > sr * 0.05f) { v->acc_cnt = 0; v->acc_arm = 0; }
        }

        /* Contagion, raw side: the neighbours' matter enters this voice's gesture */
        if (my_raw_l) { my_raw_l[i] = l; my_raw_l[512 + i] = r; }
        if (c_raw > 0) { l += (bl_raw[i] - l) * c_raw; r += (br_raw[i] - r) * c_raw; }

        /* vactrol LPG: amplitude and brightness from one photoresistor */
        float bong_add = 0;
        if (v->bong_stage) {
            if (v->bong_delay > 0) v->bong_delay--;
            else if (v->bong_stage == 1) { v->bong += b_rise; if (v->bong >= 1) { v->bong = 1; v->bong_stage = 2; } }
            else { v->bong *= b_fall; if (v->bong < 1e-4f) { v->bong = 0; v->bong_stage = 0; } }
            bong_add = v->bong * b_gain * fminf(1.0f, v->env * 4.0f);
        }
        float tmul = 1.0f;
        if (t_live) {                                            /* ternes: strikes through the LPG, silences between */
            v->t_mix += t_tgt > v->t_mix ? fminf(t_step, t_tgt - v->t_mix) : -fminf(t_step, v->t_mix - t_tgt);
            if (v->t_stage == 1) { v->t_env += t_rise; if (v->t_env >= v->t_amp) { v->t_env = v->t_amp; v->t_stage = 2; } }
            else if (v->t_stage == 2) { v->t_env = flush(v->t_env * t_fall); }
            tmul = 1.0f - v->t_mix + v->t_mix * v->t_env;
        }
        float cv = v->env * velc * (1.0f + 0.3f * v->press) * b_duck * tmul + bong_add;
        v->vac += (cv - v->vac) * (cv > v->vac ? vac_up : vac_dn);
        v->vac = flush(v->vac);
        if ((i & 7) == 0) {                                      /* the photoresistor is slow: control rate */
            float vac = clampf(v->vac, 0, 1.3f);
            amp = powf(vac, amp_exp);
            float fc = 120.0f * powf(150.0f, powf(fminf(vac, 1.0f), 0.8f)) * (0.55f + 0.45f * v->vel);
            lpa = 1.0f - expf(-2.0f * (float)M_PI * fminf(fc, 0.45f * sr) / sr);
        }

        float in[2] = { l * amp, r * amp };
        float out[2];
        for (int c = 0; c < 2; c++) {
            float x = in[c];
            v->lp1[c] = flush(v->lp1[c] + lpa * (x - v->lp1[c]));
            v->lp2[c] = flush(v->lp2[c] + lpa * (v->lp1[c] - v->lp2[c]));
            x = v->lp2[c];
            if (acc_live) {                                      /* Accroche: TPT state-variable band-pass */
                float v3 = x - v->acc_s2[c];
                float v1 = acc_g1 * v->acc_s1[c] + acc_g2 * v3;
                float v2 = v->acc_s2[c] + acc_g2 * v->acc_s1[c] + acc_g3 * v3;
                v->acc_s1[c] = flush(2.0f * v1 - v->acc_s1[c]);
                v->acc_s2[c] = flush(2.0f * v2 - v->acc_s2[c]);
                x += (v1 * acc_mk - x) * acc_w;
            }
            if (co_live) {                                       /* Corps: the sound strikes a resonant body */
                if (c == 0) v->corps_mix += co_tgt > v->corps_mix ? fminf(co_step * 30.0f, co_tgt - v->corps_mix) : -fminf(co_step, v->corps_mix - co_tgt);
                float wet = 0, x2 = v->md_x2[c];
                for (int m = 0; m < 6; m++) {
                    if (co_amp[m] <= 0) continue;
                    float y = co_b0[m] * (x - x2) + co_c[m] * v->md_y1[m][c] - co_r2[m] * v->md_y2[m][c];
                    v->md_y2[m][c] = v->md_y1[m][c]; v->md_y1[m][c] = flush(y);
                    wet += y * co_amp[m];
                }
                v->md_x2[c] = v->md_x1[c]; v->md_x1[c] = x;
                wet *= 3.0f;
                wet = wet / (1.0f + fabsf(wet));
                x += (wet - x) * v->corps_mix;
            }
            if (lock > 0.001f) {                                 /* Lock: resonance on the fragment's centroid */
                float hp = x - v->bp2[c] - lock_q * v->bp1[c];
                v->bp1[c] = flush(v->bp1[c] + lock_f * hp);
                v->bp2[c] = flush(v->bp2[c] + lock_f * v->bp1[c]);
                x = x + (v->bp1[c] * lock_q * 2.0f - x) * lock;
            }
            if (wear > 0.001f) {                                 /* wear: the tape loses its top */
                v->wlp[c] = flush(v->wlp[c] + wlp_a * (x - v->wlp[c]));
                x = v->wlp[c] * (1.0f - 0.45f * wear);
            }
            v->tlp[c] = flush(v->tlp[c] + tlp_a * (x - v->tlp[c]));
            x = v->tlp[c] * g_lo + (x - v->tlp[c]) * g_hi;
            if (drive_k > 0.01f) x = (1.0f + drive_k) * x / (1.0f + drive_k * fabsf(x));
            out[c] = x;
        }
        if (wear > 0.05f) {                                      /* ... and ends in breath */
            float nz = fe_randf(&v->rng) * 2.0f - 1.0f;
            v->hiss_lp += 0.2f * (nz - v->hiss_lp);
            float h = v->hiss_lp * 0.03f * wear * wear * v->env;
            if (fe_randf(&v->rng) < wear * wear * 0.0006f) h += (nz > 0 ? 0.25f : -0.25f) * wear * v->env;
            out[0] += h; out[1] += h * 0.8f;
        }

        /* duck before a swap */
        if (v->swapping) { v->duck -= duck_step; if (v->duck < 0) v->duck = 0; }

        float m = (out[0] + out[1]) * 0.5f, s = (out[0] - out[1]) * 0.5f * width;
        float g = vol * v->duck;
        float ol = (m + s) * pg_l * g, orr = (m - s) * pg_r * g;
        if (my_post_l) { my_post_l[i] = ol; my_post_l[512 + i] = orr; }
        if (c_post > 0) {                                        /* Contagion, played side: over the top */
            ol = ol * (1.0f - 0.5f * c_post) + bl_post[i] * c_post * v->duck;
            orr = orr * (1.0f - 0.5f * c_post) + br_post[i] * c_post * v->duck;
        }
        ml[i] += ol; mr[i] += orr;
        sl[i] += ol * send; sr_[i] += orr * send;
        if (rs_send > 0) { rl[i] += ol * rs_send; rr[i] += orr * rs_send; }
    }

    /* wear accrues with play time: about wear*0.02 per pass of the region */
    if (v->env_stage != ENV_OFF) {
        float region_s = fmaxf(v->loop_len / sr, 0.5f);
        v->wear = fminf(1.0f, v->wear + v->p[FE_P_WEAR] * 0.02f * ((float)n * dt) / region_s);
    }
}

/* ── render ─────────────────────────────────────────────────────────────── */

void fe_render(fe_engine *e, float *out_l, float *out_r, int frames) {
    if (frames > e->cfg.max_block) frames = e->cfg.max_block;
    const float dt = (float)frames / e->sr;

    /* control events */
    unsigned t = atomic_load_explicit(&e->q_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&e->q_head, memory_order_acquire);
    while (t != h) { apply_event(e, &e->q[t & (FE_QSIZE - 1)]); t++; }
    atomic_store_explicit(&e->q_tail, t, memory_order_release);

    /* hand-over of a fresh fragment, at a note boundary */
    int pv = atomic_load_explicit(&e->pending, memory_order_acquire);
    if (pv >= 0 && e->voices[pv].want_next) {        /* Futur: keep it aside instead of playing it */
        fe_voice *v = &e->voices[pv];
        v->want_next = 0;
        if (e->nfree > 0) {
            v->next = e->spare; e->spare = e->freet[--e->nfree];
            v->next_pos = clampf(e->xp[FE_X_FUTUR][2], 0, 0.98f) * (float)v->next->frames;
            v->next_mix = 0;
        }
        atomic_store_explicit(&e->pending, -1, memory_order_release);
        pv = -1;
    }
    if (pv >= 0) {
        fe_voice *v = &e->voices[pv];
        if (v->env_stage == ENV_OFF) do_swap(e, pv);
        else if (v->env_stage != ENV_FALL && !v->swapping) { v->swapping = 1; }
    }

    float *ml = e->scratch_l, *mr = e->scratch_r;
    float sl[512], srr[512], rl[512], rr[512];
    int n = frames > 512 ? 512 : frames;
    memset(ml, 0, sizeof(float) * (size_t)n); memset(mr, 0, sizeof(float) * (size_t)n);
    memset(sl, 0, sizeof(float) * (size_t)n); memset(srr, 0, sizeof(float) * (size_t)n);
    memset(rl, 0, sizeof(float) * (size_t)n); memset(rr, 0, sizeof(float) * (size_t)n);

    /* where each voice listens: the fragment's own spot if known, else its place (plus a drift) */
    for (int vi = 0; vi < FE_NV; vi++) {
        const fe_voice *v = &e->voices[vi];
        const fe_card *c = &v->tape->card;
        if (c->lat != 0 || c->lon != 0) { e->vlat[vi] = c->lat; e->vlon[vi] = c->lon; }
        else {
            const fe_place *pl = &FE_PLACES[step_of(FE_P_PLACE, v->p[FE_P_PLACE])];
            float la = pl->lat, lo = pl->lon;
            if (v->drift_km > 0) {
                float a = v->drift_deg * (float)(M_PI / 180.0);
                la += v->drift_km * cosf(a) / 110.57f;
                lo += v->drift_km * sinf(a) / (111.32f * cosf(la * (float)(M_PI / 180.0)));
            }
            e->vlat[vi] = la; e->vlon[vi] = lo;
        }
    }

    /* Hesitation: the shared pulse */
    float hes_any = 0;
    for (int vi = 0; vi < FE_NV; vi++) hes_any = fmaxf(hes_any, eff(e, &e->voices[vi], FE_X_HESITATION));
    if (hes_any > 0) { e->hes_wait -= dt; if (e->hes_wait <= 0) hesitation_pulse(e); }
    else { e->hes_wait = 0; e->hes_mult = 1.0f; }

    int bong_rank = 0, past_rank = 0;
    float rs_target = 0;
    for (int vi = 0; vi < FE_NV; vi++) {
        fe_voice *v = &e->voices[vi];
        gestures_block(e, v, vi, n, &bong_rank, &past_rank, pv);
        if (v->auto_close > 0) {                              /* Hesitation replayed a closed sound: it closes itself */
            v->auto_close -= n;
            if (v->auto_close <= 0) { v->auto_close = 0; if (v->env_stage == ENV_RISE || v->env_stage == ENV_SUSTAIN) v->env_stage = ENV_FALL; }
        }
        rs_target = fmaxf(rs_target, fmaxf(e->gx[FE_X_RESSAC], v->vx[FE_X_RESSAC]));
        if (v->drift_redo && !atomic_load(&v->want)) { v->drift_redo = 0; request_harvest(e, vi); }
        if (v->env_stage != ENV_OFF) render_voice(e, v, vi, ml, mr, sl, srr, rl, rr, n);
        e->vrend[vi] = v->env_stage != ENV_OFF;
        if (v->swapping && v->duck <= 0.0f) {                /* ducked: swap or reseek, then rise again */
            v->swapping = 0; v->duck = 1.0f;
            if (atomic_load(&e->pending) == vi) do_swap(e, vi);
            if (v->adopt) { v->adopt = 0; do_adopt(e, v); }
            if (v->reseek) { v->reseek = 0; compute_region(e, v); start_position(v, 1); v->gel_on = 0; v->gel_mix = 0; }
            v->env = 0; v->env_stage = ENV_RISE; reset_dsp(v);
        }
        if (v->env_stage == ENV_FALL && v->swapping) {
            v->swapping = 0; v->duck = 1.0f; v->reseek = 0;
            if (v->adopt) { v->adopt = 0; do_adopt(e, v); }      /* closing anyway: take the future now */
        }

        /* automatic renewal (flux) */
        v->since_swap += dt;
        float flow = m_flow(v->p[FE_P_FLOW]);
        if (flow > 0 && !v->frozen && v->since_swap > flow && !atomic_load(&v->want) && pv != vi) {
            v->since_swap = 0; renew(e, vi);
        }

        int st = v->tape->frames < 64 ? (atomic_load(&v->want) || atomic_load(&v->searching) ? FE_VS_SEARCHING : FE_VS_EMPTY)
               : (v->env_stage != ENV_OFF ? FE_VS_PLAYING : FE_VS_READY);
        atomic_store_explicit(&v->state_pub, st, memory_order_relaxed);
    }

    /* Ressac: the output comes back as a source, delayed, filtered, phase-turned, bounded */
    const float rk = 1.0f - expf(-(float)n / (0.03f * e->sr));
    const float rk_ret = 1.0f - expf(-(float)n / ((rs_target > e->rs_ret ? 0.05f : 1.5f) * e->sr));
    e->rs_amt += (rs_target - e->rs_amt) * rk;
    e->rs_ret += (rs_target - e->rs_ret) * rk_ret;
    if (e->rs_amt < 1e-5f && rs_target <= 0) e->rs_amt = 0;
    const int rs_live = e->rs_l && (e->rs_ret > 1e-4f || rs_target > 0);
    int RD = 1, RD2 = 1;
    float rs_fb = 0, rs_lpa = 0, rs_hpa = 0, rs_apc = 0, rs_ceil = 1;
    if (rs_live) {
        const float *X = e->xp[FE_X_RESSAC];
        RD = (int)(mx_rs_time(X[0]) * e->sr);
        RD2 = RD + (int)(X[2] * 0.03f * e->sr);                       /* the right side comes back later */
        if (RD2 >= e->rs_len) RD2 = e->rs_len - 1;
        if (RD >= e->rs_len) RD = e->rs_len - 1;
        if (RD < 1) RD = 1;
        rs_fb = e->rs_amt * 0.85f;
        rs_lpa = 1.0f - expf(-2.0f * (float)M_PI * (400.0f * powf(25.0f, X[1])) / e->sr);
        rs_hpa = 1.0f - expf(-2.0f * (float)M_PI * 90.0f / e->sr);
        rs_apc = X[2] * 0.7f;
        rs_ceil = 0.08f + 0.6f * X[3];
    }

    /* echo pool: a dark feedback delay the voices send into */
    const float fb = e->g[FE_G_POOL_FB] * 0.92f;
    int D = (int)(m_pooltime(e->g[FE_G_POOL_TIME]) * e->sr);
    if (D >= e->pool_len) D = e->pool_len - 1;
    if (D < 1) D = 1;
    const float plp = 1.0f - expf(-2.0f * (float)M_PI * 5000.0f / e->sr);
    const float mv = e->g[FE_G_VOLUME];
    for (int i = 0; i < n; i++) {
        int rp = e->pool_w - D; if (rp < 0) rp += e->pool_len;
        float pl = e->pool_l[rp], pr = e->pool_r[rp];
        e->pool_lp[0] = flush(e->pool_lp[0] + plp * (pl - e->pool_lp[0]));
        e->pool_lp[1] = flush(e->pool_lp[1] + plp * (pr - e->pool_lp[1]));
        e->pool_l[e->pool_w] = flush(sl[i] + e->pool_lp[1] * fb);      /* cross-fed: it wanders across */
        e->pool_r[e->pool_w] = flush(srr[i] + e->pool_lp[0] * fb);
        if (++e->pool_w >= e->pool_len) e->pool_w = 0;
        float l = ml[i] + pl * 0.8f, r = mr[i] + pr * 0.8f;
        if (e->lastmix) e->lastmix[i] = (l + r) * 0.5f;
        if (rs_live) {
            int a = e->rs_w - RD; if (a < 0) a += e->rs_len;
            int b = e->rs_w - RD2; if (b < 0) b += e->rs_len;
            float y[2] = { e->rs_l[a], e->rs_r[b] };
            for (int c = 0; c < 2; c++) {
                e->rs_lp[c] = flush(e->rs_lp[c] + rs_lpa * (y[c] - e->rs_lp[c]));
                float x = e->rs_lp[c];
                e->rs_hp[c] = flush(e->rs_hp[c] + rs_hpa * (x - e->rs_hp[c]));
                x -= e->rs_hp[c];
                float ap = -rs_apc * x + e->rs_ap[c];                  /* first-order all-pass: the phase turns */
                e->rs_ap[c] = flush(x + rs_apc * ap);
                y[c] = rs_ceil * tanhf(ap / rs_ceil);                  /* a ceiling: it never runs away */
            }
            e->rs_l[e->rs_w] = flush(rl[i] + y[1] * rs_fb);            /* crossed, like the pool */
            e->rs_r[e->rs_w] = flush(rr[i] + y[0] * rs_fb);
            if (++e->rs_w >= e->rs_len) e->rs_w = 0;
            l += y[0] * e->rs_ret; r += y[1] * e->rs_ret;
        }
        out_l[i] = tanhf(l) * mv;
        out_r[i] = tanhf(r) * mv;
    }
    for (int i = n; i < frames; i++) out_l[i] = out_r[i] = 0;
}

/* ── lifecycle ──────────────────────────────────────────────────────────── */

fe_engine *fe_create(const fe_config *cfg) {
    fe_engine *e = calloc(1, sizeof(fe_engine));
    if (!e) return NULL;
    e->cfg = *cfg;
    e->sr = cfg->sample_rate > 8000 ? cfg->sample_rate : 44100.0f;
    if (e->cfg.max_block <= 0 || e->cfg.max_block > 512) e->cfg.max_block = 512;
    if (e->cfg.voice_seconds <= 0) e->cfg.voice_seconds = 30;

    /* tapes: largest length calloc grants, stepping down */
    static const float ladder[] = { 1.0f, 0.66f, 0.4f, 0.2f };
    int ok = 0;
    for (int li = 0; li < 4 && !ok; li++) {
        int cap = (int)(e->cfg.voice_seconds * ladder[li] * e->sr);
        ok = 1;
        for (int i = 0; i < FE_NTAPES; i++) {
            e->tapes[i].pcm = calloc((size_t)cap * 2, sizeof(int16_t));
            e->tapes[i].cap = cap;
            if (!e->tapes[i].pcm) { ok = 0; break; }
        }
        if (!ok) for (int i = 0; i < FE_NTAPES; i++) { free(e->tapes[i].pcm); e->tapes[i].pcm = NULL; }
        else e->tape_cap = cap;
    }
    e->pool_len = (int)(2.1f * e->sr);
    e->pool_l = calloc((size_t)e->pool_len, sizeof(float));
    e->pool_r = calloc((size_t)e->pool_len, sizeof(float));
    e->rs_len = (int)(2.1f * e->sr);
    e->rs_l = calloc((size_t)e->rs_len, sizeof(float));
    e->rs_r = calloc((size_t)e->rs_len, sizeof(float));
    e->vbuf = calloc((size_t)FE_NV * 2 * 2 * 512, sizeof(float));
    e->lastmix = calloc(512, sizeof(float));
    e->hes_mult = 1.0f; e->hrng = (cfg->seed ? cfg->seed : 0x5EED1234u) ^ 0x3C6EF372u;
    for (int g = 0; g < FE_X_COUNT; g++) memcpy(e->xp[g], X_DEF[g], sizeof X_DEF[0]);
    e->scratch_l = calloc(512, sizeof(float));
    e->scratch_r = calloc(512, sizeof(float));
    if (!ok || !e->pool_l || !e->pool_r || !e->rs_l || !e->rs_r || !e->vbuf || !e->lastmix || !e->scratch_l || !e->scratch_r) { fe_destroy(e); return NULL; }

    e->g[FE_G_VOLUME] = 0.6f; e->g[FE_G_POOL_TIME] = 0.6f; e->g[FE_G_POOL_FB] = 0.45f;
    uint32_t seed = cfg->seed ? cfg->seed : 0x5EED1234u;
    e->wrng = seed ^ 0xA5A5A5A5u;
    for (int i = 0; i < FE_NV; i++) {
        fe_voice *v = &e->voices[i];
        v->tape = &e->tapes[i];
        v->past = &e->tapes[FE_NV + i];                  /* empty until a first renewal */
        v->vn_u = -1; v->vn_want = -1; v->tf = 1.0f;
        memcpy(v->p, P_DEFAULT, sizeof(P_DEFAULT));
        v->p[FE_P_PLACE] = (float)(i % FE_NPLACES) / (float)(FE_NPLACES - 1);
        v->p[FE_P_PAN] = 0.5f + ((float)(i % 4) - 1.5f) * 0.12f;
        v->duck = 1.0f;
        v->rng = seed + 7919u * (uint32_t)(i + 1);
        v->dpitch.rng = seed * 3u + 104729u * (uint32_t)(i + 1);
        v->dpan.rng = seed * 5u + 1299709u * (uint32_t)(i + 1);
        v->dpos.rng = seed * 7u + 15485863u * (uint32_t)(i + 1);
        atomic_store(&v->want, 0);
        request_harvest(e, i);                       /* every voice starts by listening to its place */
    }
    e->spare = &e->tapes[2 * FE_NV];
    for (int k = 0; k < FE_NFUT; k++) e->freet[k] = &e->tapes[2 * FE_NV + 1 + k];
    e->nfree = FE_NFUT;
    atomic_store(&e->pending, -1);
    if (fe_harvest_start(e) != 0) { fe_destroy(e); return NULL; }
    return e;
}

void fe_destroy(fe_engine *e) {
    if (!e) return;
    fe_harvest_stop(e);
    for (int i = 0; i < FE_NTAPES; i++) free(e->tapes[i].pcm);
    free(e->pool_l); free(e->pool_r); free(e->rs_l); free(e->rs_r); free(e->vbuf); free(e->lastmix); free(e->scratch_l); free(e->scratch_r);
    free(e->dec); free(e->net_buf);
    free(e);
}

/* ── readback ───────────────────────────────────────────────────────────── */

float fe_get(const fe_engine *e, int v, int id) { return (vok(v) && id >= 0 && id < FE_P_COUNT) ? e->voices[v].p[id] : 0; }
float fe_get_global(const fe_engine *e, int id) { return (id >= 0 && id < FE_G_COUNT) ? e->g[id] : 0; }
int   fe_voice_state(const fe_engine *e, int v) { return vok(v) ? atomic_load_explicit(&((fe_engine *)e)->voices[v].state_pub, memory_order_relaxed) : 0; }
int   fe_voice_busy(const fe_engine *e, int v) {
    if (!vok(v)) return 0;
    fe_voice *x = &((fe_engine *)e)->voices[v];
    return atomic_load(&x->want) || atomic_load(&x->searching) || atomic_load(&((fe_engine *)e)->pending) == v;
}
int   fe_voice_searching(const fe_engine *e, int v) { return vok(v) ? atomic_load(&((fe_engine *)e)->voices[v].searching) : 0; }
int   fe_voice_frozen(const fe_engine *e, int v) { return vok(v) ? e->voices[v].frozen : 0; }
float fe_voice_wear(const fe_engine *e, int v) { return vok(v) ? e->voices[v].wear : 0; }
float fe_voice_pos(const fe_engine *e, int v) {
    if (!vok(v)) return 0;
    const fe_voice *x = &e->voices[v];
    return x->tape->frames > 0 ? clampf(x->pos / (float)x->tape->frames, 0, 1) : 0;
}
void fe_voice_card(const fe_engine *e, int v, fe_card *out) {
    if (!vok(v)) { memset(out, 0, sizeof(*out)); return; }
    *out = e->voices[v].tape->card;
}
int fe_voice_wave(const fe_engine *e, int v, uint8_t *out, int n) {
    if (!vok(v) || n <= 0) return 0;
    const fe_tape *t = e->voices[v].tape;
    for (int i = 0; i < n; i++) out[i] = t->frames ? t->wave[(i * FE_WAVE_N) / n] : 0;
    return n;
}
int fe_net_status(const fe_engine *e, char *buf, int len) {
    return snprintf(buf, (size_t)len, "%s", e->net_msg);
}

int fe_format(const fe_engine *e, int v, int id, float x, char *b, int len) {
    (void)e; (void)v;
    size_t L = (size_t)len;
    switch (id) {
    case FE_P_PLACE:  return snprintf(b, L, "%s", fe_place_name(step_of(id, x)));
    case FE_P_SOURCE: { static const char *s[] = { "Les deux", "Aporee", "Freesound" }; return snprintf(b, L, "%s", s[step_of(id, x)]); }
    case FE_P_HOLD:   { static const char *s[] = { "Boucle", "Derive", "Grains" }; return snprintf(b, L, "%s", s[step_of(id, x)]); }
    case FE_P_DIR:    return snprintf(b, L, "%s", step_of(id, x) ? "Arriere" : "Avant");
    case FE_P_RADIUS: {
        int p = vok(v) ? step_of(FE_P_PLACE, e->voices[v].p[FE_P_PLACE]) : 0;
        float km = FE_PLACES[p].radius_km * m_radius(x);
        return km < 1 ? snprintf(b, L, "%d m", (int)(km * 1000)) : snprintf(b, L, "%.1f km", km); }
    case FE_P_FLOW:   { float s = m_flow(x); if (s <= 0) return snprintf(b, L, "Off");
                        return s < 60 ? snprintf(b, L, "%d s", (int)s) : snprintf(b, L, "%.1f min", s / 60); }
    case FE_P_RISE:   { float s = m_rise(x); return s < 1 ? snprintf(b, L, "%d ms", (int)(s * 1000)) : snprintf(b, L, "%.1f s", s); }
    case FE_P_FALL:   { float s = m_fall(x); return s < 1 ? snprintf(b, L, "%d ms", (int)(s * 1000)) : snprintf(b, L, "%.1f s", s); }
    case FE_P_GRAIN:  return snprintf(b, L, "%d ms", (int)(m_grain(x) * 1000));
    case FE_P_SPEED:  return snprintf(b, L, "x%.2f", m_speed(x));
    case FE_P_PAN:    { int p = (int)((x - 0.5f) * 200); return p == 0 ? snprintf(b, L, "C") : snprintf(b, L, "%c%d", p < 0 ? 'L' : 'R', abs(p)); }
    case FE_P_TONE:   { int p = (int)((x - 0.5f) * 200); return snprintf(b, L, "%+d", p); }
    case FE_P_START: case FE_P_LENGTH: {
        float F = vok(v) ? (float)e->voices[v].tape->frames / e->sr : 0;
        float s = id == FE_P_START ? x * F : x * F * (1 - e->voices[v].p[FE_P_START]);
        return snprintf(b, L, "%.1f s", s); }
    default:          return snprintf(b, L, "%d%%", (int)(x * 100 + 0.5f));
    }
}
