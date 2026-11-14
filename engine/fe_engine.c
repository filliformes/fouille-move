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

/* ── voice helpers (audio thread) ───────────────────────────────────────── */

static void request_harvest(fe_engine *e, int vi) {
    fe_voice *v = &e->voices[vi];
    if (atomic_load(&v->want)) return;
    v->req.place = step_of(FE_P_PLACE, v->p[FE_P_PLACE]);
    v->req.source = step_of(FE_P_SOURCE, v->p[FE_P_SOURCE]);
    v->req.radius_scale = m_radius(v->p[FE_P_RADIUS]);
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

static void start_position(fe_voice *v) {
    float sc = v->p[FE_P_SCATTER] * 0.5f * fe_randf(&v->rng) * v->loop_len;
    int rev = step_of(FE_P_DIR, v->p[FE_P_DIR]);
    v->pos = rev ? (v->loop_a + v->loop_len - 1 - sc) : (v->loop_a + sc);
    for (int g = 0; g < 2; g++) { v->grain_ph[g] = 0.5f * (float)g; v->grain_org[g] = v->pos; }
}

static void do_swap(fe_engine *e, int vi) {
    fe_voice *v = &e->voices[vi];
    fe_tape *old = v->tape;
    v->tape = e->spare;
    e->spare = old;
    v->wear = 0; v->since_swap = 0;
    compute_region(e, v);
    start_position(v);
    atomic_store_explicit(&e->pending, -1, memory_order_release);   /* spare back to the worker */
}

static void apply_event(fe_engine *e, const fe_event *ev) {
    fe_voice *v = &e->voices[ev->v];
    switch (ev->type) {
    case EV_GATE_ON:
        if (v->tape->frames < 64) { request_harvest(e, ev->v); break; }
        v->vel = clampf(ev->val, 0, 1);
        if (v->env_stage == ENV_OFF) {
            v->env = 0; reset_dsp(v);
            compute_region(e, v); start_position(v);
        }
        v->env_stage = ENV_RISE;                     /* retrigger from where the envelope is: no click */
        break;
    case EV_GATE_OFF:
        if (v->env_stage == ENV_RISE || v->env_stage == ENV_SUSTAIN) v->env_stage = ENV_FALL;
        break;
    case EV_PRESS:   v->press = clampf(ev->val, 0, 1); break;
    case EV_REFETCH: request_harvest(e, ev->v); break;
    case EV_FREEZE:  v->frozen = ev->val > 0.5f; break;
    case EV_SET: {
        float old = v->p[ev->id];
        v->p[ev->id] = ev->val;
        if (ev->id == FE_P_START || ev->id == FE_P_LENGTH) { compute_region(e, v); keep_in_region(v); }
        if ((ev->id == FE_P_PLACE || ev->id == FE_P_SOURCE) && step_of(ev->id, old) != step_of(ev->id, ev->val)
            && !v->frozen)
            request_harvest(e, ev->v);   /* a new place: go and listen there */
        break; }
    case EV_GSET: e->g[ev->id] = ev->val; break;
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

/* ── one voice, one block ───────────────────────────────────────────────── */

static void render_voice(fe_engine *e, fe_voice *v, int vi, float *ml, float *mr, float *sl, float *sr_, int n) {
    const float sr = e->sr, dt = 1.0f / sr;
    const fe_tape *t = v->tape;
    if (t->frames < 64) { v->env_stage = ENV_OFF; return; }

    /* block constants */
    const int   hold = step_of(FE_P_HOLD, v->p[FE_P_HOLD]);
    const int   rev  = step_of(FE_P_DIR, v->p[FE_P_DIR]);
    const float strike = v->p[FE_P_STRIKE];
    const float rise_inc = 1.0f / (m_rise(v->p[FE_P_RISE]) * sr);
    const float fall_k = 1.0f - expf(-6.9f / (m_fall(v->p[FE_P_FALL]) * sr));
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
    if (hold == 1) {                                   /* Derive: the region itself wanders */
        float w = drift_step(&v->dpos, 0.03f) * (0.2f + drift);
        float room = (float)t->frames - v->loop_len;
        if (room > 1) {
            v->loop_a = clampf(v->loop_a + w * 0.002f * sr * (float)n / 128.0f, 0, room);
            keep_in_region(v);
        }
    }
    const float xf = fminf(0.02f * sr, v->loop_len * 0.25f);
    const float grain_len = fminf(m_grain(v->p[FE_P_GRAIN]) * sr, v->loop_len);
    const float g_inc = 1.0f / fmaxf(grain_len, 16.0f) * fabsf(spd) ;
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

    for (int i = 0; i < n; i++) {
        /* envelope */
        switch (v->env_stage) {
        case ENV_RISE: v->env += rise_inc; if (v->env >= 1) { v->env = 1; v->env_stage = ENV_SUSTAIN; } break;
        case ENV_FALL: v->env -= v->env * fall_k + 1e-7f;
                       if (v->env <= 1e-4f && v->vac < 1e-4f) { v->env = 0; v->env_stage = ENV_OFF; } break;
        default: break;
        }
        if (v->env_stage == ENV_OFF) break;
        if (v->env < 0) v->env = 0;

        /* read */
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
                    float step = (1.0f - scatter) * grain_len * 0.5f;
                    float jump = scatter * fe_randf(&v->rng) * v->loop_len;
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
            v->pos += spd;
            if (v->pos >= b) v->pos -= v->loop_len;
            if (v->pos < a) v->pos += v->loop_len;
        }

        /* vactrol LPG: amplitude and brightness from one photoresistor */
        float cv = v->env * velc * (1.0f + 0.3f * v->press);
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
            if (lock > 0.001f) {                                 /* Spectral Lock: resonance on the fragment's centroid */
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
        ml[i] += ol; mr[i] += orr;
        sl[i] += ol * send; sr_[i] += orr * send;
    }

    /* wear accrues with play time: about wear*0.02 per pass of the region */
    if (v->env_stage != ENV_OFF) {
        float region_s = fmaxf(v->loop_len / sr, 0.5f);
        v->wear = fminf(1.0f, v->wear + v->p[FE_P_WEAR] * 0.02f * ((float)n * dt) / region_s);
    }
    (void)vi;
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
    if (pv >= 0) {
        fe_voice *v = &e->voices[pv];
        if (v->env_stage == ENV_OFF) do_swap(e, pv);
        else if (v->env_stage != ENV_FALL && !v->swapping) { v->swapping = 1; }
    }

    float *ml = e->scratch_l, *mr = e->scratch_r;
    float sl[512], srr[512];
    int n = frames > 512 ? 512 : frames;
    memset(ml, 0, sizeof(float) * (size_t)n); memset(mr, 0, sizeof(float) * (size_t)n);
    memset(sl, 0, sizeof(float) * (size_t)n); memset(srr, 0, sizeof(float) * (size_t)n);

    for (int vi = 0; vi < FE_NV; vi++) {
        fe_voice *v = &e->voices[vi];
        if (v->env_stage != ENV_OFF) render_voice(e, v, vi, ml, mr, sl, srr, n);
        if (v->swapping && v->duck <= 0.0f) {                /* ducked: swap, then rise again */
            v->swapping = 0; v->duck = 1.0f;
            if (atomic_load(&e->pending) == vi) do_swap(e, vi);
            v->env = 0; v->env_stage = ENV_RISE; reset_dsp(v);
        }
        if (v->env_stage == ENV_FALL && v->swapping) { v->swapping = 0; v->duck = 1.0f; }

        /* automatic renewal (flux) */
        v->since_swap += dt;
        float flow = m_flow(v->p[FE_P_FLOW]);
        if (flow > 0 && !v->frozen && v->since_swap > flow && !atomic_load(&v->want) && pv != vi) {
            v->since_swap = 0; request_harvest(e, vi);
        }

        int st = v->tape->frames < 64 ? (atomic_load(&v->want) || atomic_load(&v->searching) ? FE_VS_SEARCHING : FE_VS_EMPTY)
               : (v->env_stage != ENV_OFF ? FE_VS_PLAYING : FE_VS_READY);
        atomic_store_explicit(&v->state_pub, st, memory_order_relaxed);
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
    e->scratch_l = calloc(512, sizeof(float));
    e->scratch_r = calloc(512, sizeof(float));
    if (!ok || !e->pool_l || !e->pool_r || !e->scratch_l || !e->scratch_r) { fe_destroy(e); return NULL; }

    e->g[FE_G_VOLUME] = 0.6f; e->g[FE_G_POOL_TIME] = 0.6f; e->g[FE_G_POOL_FB] = 0.45f;
    uint32_t seed = cfg->seed ? cfg->seed : 0x5EED1234u;
    e->wrng = seed ^ 0xA5A5A5A5u;
    for (int i = 0; i < FE_NV; i++) {
        fe_voice *v = &e->voices[i];
        v->tape = &e->tapes[i];
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
    e->spare = &e->tapes[FE_NV];
    atomic_store(&e->pending, -1);
    if (fe_harvest_start(e) != 0) { fe_destroy(e); return NULL; }
    return e;
}

void fe_destroy(fe_engine *e) {
    if (!e) return;
    fe_harvest_stop(e);
    for (int i = 0; i < FE_NTAPES; i++) free(e->tapes[i].pcm);
    free(e->pool_l); free(e->pool_r); free(e->scratch_l); free(e->scratch_r);
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
