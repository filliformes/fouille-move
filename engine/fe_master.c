/*
 * Fouille engine — the Output stage (Track 4), ported from Loopex
 * (overtake-shell/src/loopex.c: master_eq_update, master_character, master_glue,
 * tape_limiter, global_saturate, the LoCut/HiCut cascades and the output dither).
 * Loopex is by the same author (filliformes); this port keeps its numbers and order:
 *   gSat -> LoCut / HiCut (pole count per Character) -> Character -> Glue -> x Out -> Limit.
 * The only change is the sample rate, taken from the engine instead of a constant.
 * Audio thread only: no allocation, no lock, no file.
 */
#include "fe_internal.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWOPI (2.0 * M_PI)

static inline double lb_clampd(double x, double lo, double hi) { if (x != x) return lo; return x < lo ? lo : (x > hi ? hi : x); }
static inline double lb_tanh(double x) {
    if (x > 3.0) return 1.0;
    if (x < -3.0) return -1.0;
    double x2 = x * x; return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

/* ---- biquads (transposed DF2; reset = unity pass-through, never silence) ---- */
static void bq_reset(fe_bq *f) { memset(f, 0, sizeof *f); f->b0 = 1.0; }
static void bq_set_lp(fe_bq *f, double freq, double Q, double SR) {
    double w0 = TWOPI * freq / SR, cosW = cos(w0), alpha = sin(w0) / (2.0 * Q), a0 = 1.0 + alpha;
    f->b0 = (1.0 - cosW) / 2.0 / a0; f->b1 = (1.0 - cosW) / a0; f->b2 = f->b0; f->a1 = (-2.0 * cosW) / a0; f->a2 = (1.0 - alpha) / a0;
}
static void bq_set_hp(fe_bq *f, double freq, double Q, double SR) {
    double w0 = TWOPI * freq / SR, cosW = cos(w0), alpha = sin(w0) / (2.0 * Q), a0 = 1.0 + alpha;
    f->b0 = (1.0 + cosW) / 2.0 / a0; f->b1 = -(1.0 + cosW) / a0; f->b2 = f->b0; f->a1 = (-2.0 * cosW) / a0; f->a2 = (1.0 - alpha) / a0;
}
static void bq_set_lowshelf(fe_bq *f, double freq, double db, double S, double SR) {
    double A = pow(10.0, db / 40.0), w0 = TWOPI * freq / SR, cosW = cos(w0), sinW = sin(w0);
    double al = sinW / 2.0 * sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0), sqA = 2.0 * sqrt(A) * al;
    double a0 = (A + 1.0) + (A - 1.0) * cosW + sqA;
    f->b0 = (A * ((A + 1.0) - (A - 1.0) * cosW + sqA)) / a0; f->b1 = (2.0 * A * ((A - 1.0) - (A + 1.0) * cosW)) / a0;
    f->b2 = (A * ((A + 1.0) - (A - 1.0) * cosW - sqA)) / a0; f->a1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cosW)) / a0;
    f->a2 = ((A + 1.0) + (A - 1.0) * cosW - sqA) / a0;
}
static void bq_set_highshelf(fe_bq *f, double freq, double db, double S, double SR) {
    double A = pow(10.0, db / 40.0), w0 = TWOPI * freq / SR, cosW = cos(w0), sinW = sin(w0);
    double al = sinW / 2.0 * sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0), sqA = 2.0 * sqrt(A) * al;
    double a0 = (A + 1.0) - (A - 1.0) * cosW + sqA;
    f->b0 = (A * ((A + 1.0) + (A - 1.0) * cosW + sqA)) / a0; f->b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cosW)) / a0;
    f->b2 = (A * ((A + 1.0) + (A - 1.0) * cosW - sqA)) / a0; f->a1 = (2.0 * ((A - 1.0) - (A + 1.0) * cosW)) / a0;
    f->a2 = ((A + 1.0) - (A - 1.0) * cosW - sqA) / a0;
}
static void bq_set_peak(fe_bq *f, double freq, double db, double Q, double SR) {
    double A = pow(10.0, db / 40.0), w0 = TWOPI * freq / SR, cosW = cos(w0), alpha = sin(w0) / (2.0 * Q);
    double a0 = 1.0 + alpha / A;
    f->b0 = (1.0 + alpha * A) / a0; f->b1 = (-2.0 * cosW) / a0; f->b2 = (1.0 - alpha * A) / a0; f->a1 = f->b1; f->a2 = (1.0 - alpha / A) / a0;
}
static inline double bq_tick(fe_bq *f, double x, double *z1, double *z2) {
    double out = f->b0 * x + *z1; *z1 = f->b1 * x - f->a1 * out + *z2; *z2 = f->b2 * x - f->a2 * out; return out;
}
#define bq_L(f, x) bq_tick(f, x, &(f)->z1L, &(f)->z2L)
#define bq_R(f, x) bq_tick(f, x, &(f)->z1R, &(f)->z2R)

static inline double global_saturate(double x, double amt) {
    if (amt < 0.005) return x;
    double x0 = x, gain = 1.0 + amt * 4.0; x *= gain;
    double br = fabs(x); if (br > 1.5707963) br = 1.5707963; br = sin(br); x = (x > 0.0) ? br : -br;
    double bump = sin(x * 0.3) * amt * 0.08; x = (x + bump) / (gain * 0.45 + 0.55);
    double w = (amt < 0.05) ? amt / 0.05 : 1.0; return x0 + (x - x0) * w;
}
static inline double lb_mulaw_quant(double x, double q) {
    const double MU = 255.0, K = 5.5451774444795625;
    double sg = (x < 0.0) ? -1.0 : 1.0, a = fabs(x); if (a > 1.0) a = 1.0;
    double c = log1p(MU * a) / K;
    c = floor(c * q + 0.5) / q;
    return sg * (expm1(K * c) / MU);
}
#define ADAA_EPS 1e-5
static inline double adaa_F_clip(double u) { double a = fabs(u); return (a <= 1.0) ? 0.5 * u * u : a - 0.5; }
static inline double adaa_F_tanh(double u) { double a = fabs(u); return a + log1p(exp(-2.0 * a)) - 0.69314718055994531; }
static inline double adaa_run(double u1, double *u0, double *F0, int hard) {
    double F1 = hard ? adaa_F_clip(u1) : adaa_F_tanh(u1);
    double d = u1 - *u0, y;
    if (fabs(d) < ADAA_EPS) { double m = 0.5 * (u1 + *u0); y = hard ? lb_clampd(m, -1.0, 1.0) : tanh(m); }
    else y = (F1 - *F0) / d;
    *u0 = u1; *F0 = F1; return y;
}

/* Character voicings: Loopex MEQ_DEF, same order (a grittiness ramp). Columns:
 * loHz,loDb, midHz,midDb,midQ, hiHz,hiDb, sat, crushHz, makeupDb, grit, bits, clip, asym, compand, bwHz,
 * poles, glueAtk ms, glueRel ms, glueKnee, glueDist, limCeil, thrust. (grit drives Loopex's loop reader: unused here.) */
static const char *MEQ_NAMES[FE_NCHAR] = { "Off", "962", "Air", "SSL", "Neve", "Trident", "Studer", "API", "Ampex", "MPC", "S950", "SP12", "Emu" };
static const double MEQ_DEF[FE_NCHAR][23] = {
    {   0,0,     0,0,0,        0,0,      0.00,     0, 0.0 , 0.00,     0,0,0.00,0,0,2,10,250, 6,0.00,0.90,0.00 },
    {  40,-0.5,    0, 0.0,0.7, 16000,-0.7, 0.03,     0, -0.1 , 0.00,     0,0,0.00,0,0,2,3,250, 4,0.00,0.94,0.00 },
    {  95, 1.0, 2200,-0.8,0.8, 15000, 3.0, 0.06,     0, -0.6 , 0.00,     0,0,0.10,0,0,3,5,400, 8,0.00,0.94,0.00 },
    { 200,-1.0, 1500, 1.5,0.9,  9000, 1.5, 0.10,     0, -0.8 , 0.00,     0,0,0.05,0,0,3,3,300, 5,0.00,0.92,0.00 },
    { 110, 2.5,  700,-1.0,0.7, 12000, 3.5, 0.14,     0, -1.8 , 0.00,     0,0,0.08,0,0,3,3,400, 6,0.35,0.90,0.00 },
    {  80, 1.5, 1000, 1.5,1.3, 12000, 2.0, 0.16,     0, -1.2 , 0.00,     0,0,0.10,0,0,2,1,200, 2,0.15,0.90,0.00 },
    {  60, 1.5, 3500, 1.0,0.8, 12000, 2.5, 0.18,     0, -1.5 , 0.00,     0,0,0.10,0,0,2,30,600,10,0.00,0.85,0.00 },
    { 100, 1.0,  800, 2.5,1.1,  7000, 1.0, 0.20,     0, -1.6 , 0.00,     0,0,0.10,0,0,2,    3,200, 6,-0.05,0.96,1.00 },
    {  70, 2.0,  400, 0.5,0.6, 10000, 1.0, 0.40,     0, -2.4 , 0.00,     0,0,0.12,0,0,2,35,700,12,0.00,0.82,0.00 },
    { 110, 3.0,  700, 0.5,0.7,  8000,-2.0, 0.24, 40000, -1.2 , 0.25,    12,1,0.00,1,18000,2,10,250, 6,0.00,0.98,0.00 },
    {  90, 0.5, 1200, 0.0,0.8, 12000,-1.0, 0.26, 25000, -1.0 , 0.60,    12,1,0.00,0,10000,6,10,250, 6,0.00,0.98,0.00 },
    {  80, 2.0, 1800,-1.5,0.8,  7000,-3.0, 0.30, 26040, -1.0 , 1.00,    12,1,0.00,0,0,2,10,250, 6,0.00,1.00,0.00 },
    {  70, 1.5, 2200,-1.0,0.9,  5500,-4.5, 0.34, 26040, -0.6 , 1.00,    12,1,0.00,0,0,2,10,250, 6,0.00,1.00,0.00 },
};
const char *fe_char_name(int i) { return (i >= 0 && i < FE_NCHAR) ? MEQ_NAMES[i] : ""; }

static void master_eq_update(fe_master *s, int p, double SR) {
    if (p < 0) p = 0;
    if (p >= FE_NCHAR) p = FE_NCHAR - 1;
    const double *d = MEQ_DEF[p];
    if (fabs(d[1]) > 0.05) bq_set_lowshelf(&s->loSh, d[0], d[1], 0.7, SR); else bq_reset(&s->loSh);
    if (fabs(d[3]) > 0.05) bq_set_peak(&s->midPk, d[2], d[3], d[4], SR); else bq_reset(&s->midPk);
    if (fabs(d[6]) > 0.05) bq_set_highshelf(&s->hiSh, d[5], d[6], 0.7, SR); else bq_reset(&s->hiSh);
    s->eqSat = d[7];
    s->eqBits = (d[11] > 1.0) ? (int)(d[11] + 0.5) : 0;
    s->eqClip = (d[12] > 0.5) ? 1 : 0;
    s->eqCompand = (d[14] > 0.5) ? 1 : 0;
    s->eqBwHz = d[15];
    s->eqPoles = (int)(d[16] + 0.5);
    if (s->eqPoles < 2) s->eqPoles = 2;
    if (s->eqPoles > 6) s->eqPoles = 6;
    s->glueAtk = d[17] * 0.001; s->glueRel = d[18] * 0.001; s->glueKnee = d[19];
    s->glueDist = d[20]; s->limCeil = d[21];
    if (s->glueAtk < 1e-6) s->glueAtk = 1e-6;
    if (s->glueRel < 1e-4) s->glueRel = 1e-4;
    if (s->limCeil < 0.5) s->limCeil = 0.90;
    s->glueThrust = d[22]; s->thrK = 1.0 - exp(-TWOPI * 632.0 / SR);
    if (s->eqBwHz > 100.0) {
        static const double BQ[3] = { 0.51763809, 0.70710678, 1.93185165 };
        double f = s->eqBwHz; if (f > SR * 0.45) f = SR * 0.45;
        for (int k = 0; k < 3; k++) bq_set_lp(&s->bw[k], f, BQ[k], SR);
    } else for (int k = 0; k < 3; k++) bq_reset(&s->bw[k]);
    s->eqAsym = d[13];
    s->adaaFL = s->eqClip ? adaa_F_clip(s->adaaUL) : adaa_F_tanh(s->adaaUL);
    s->adaaFR = s->eqClip ? adaa_F_clip(s->adaaUR) : adaa_F_tanh(s->adaaUR);
    s->eqCrush = (d[8] > 1.0) ? (int)(SR / d[8] + 0.5) : 0;
    if (s->eqCrush < 1) s->eqCrush = 0;
    s->eqMakeup = pow(10.0, d[9] / 20.0);
    s->chr = p;
}

void fe_master_init(fe_master *m, double sr) {
    memset(m, 0, sizeof *m);
    for (int k = 0; k < 3; k++) { bq_reset(&m->lo[k]); bq_reset(&m->hi[k]); }
    m->chr = -1; m->lo_f = m->hi_f = -1; m->dith = 0x9E3779B9u;
    master_eq_update(m, 1, sr);
}

/* Per block: coefficients follow Character and the cut frequencies (Loopex recomputes them every block). */
void fe_master_block(fe_master *s, int chr, double locut, double hicut, double SR) {
    if (chr != s->chr) master_eq_update(s, chr, SR);
    static const double Q2[1] = { 0.70710678 }, Q3[1] = { 1.0 }, Q6[3] = { 0.51763809, 0.70710678, 1.93185165 };
    int np = s->eqPoles, nb = (np == 6) ? 3 : 1; const double *qq = (np == 6) ? Q6 : ((np == 3) ? Q3 : Q2);
    if (locut > 21.0 && (locut != s->lo_f || np != s->lo_np)) {
        for (int k = 0; k < nb; k++) bq_set_hp(&s->lo[k], locut, qq[k], SR);
        s->loOneK = 1.0 - exp(-TWOPI * locut / SR); s->lo_f = locut; s->lo_np = np;
    }
    if (hicut < 19999.0 && (hicut != s->hi_f || np != s->hi_np)) {
        for (int k = 0; k < nb; k++) bq_set_lp(&s->hi[k], hicut, qq[k], SR);
        s->hiOneK = 1.0 - exp(-TWOPI * hicut / SR); s->hi_f = hicut; s->hi_np = np;
    }
}

static inline void master_character(fe_master *s, int chr, double *l, double *r) {
    if (chr <= 0) return;
    double L = *l, R = *r;
    if (s->eqCrush > 1) { if (++s->eqCrushCnt >= s->eqCrush) { s->eqCrushHoldL = L; s->eqCrushHoldR = R; s->eqCrushCnt = 0; } L = s->eqCrushHoldL; R = s->eqCrushHoldR; }
    if (s->eqBits > 0) { double q = (double)(1 << (s->eqBits - 1));
        if (s->eqCompand) { L = lb_mulaw_quant(L, q); R = lb_mulaw_quant(R, q); }
        else              { L = floor(L * q + 0.5) / q; R = floor(R * q + 0.5) / q; } }
    if (s->eqBwHz > 100.0) { L = bq_L(&s->bw[0], L); L = bq_L(&s->bw[1], L); L = bq_L(&s->bw[2], L);
                             R = bq_R(&s->bw[0], R); R = bq_R(&s->bw[1], R); R = bq_R(&s->bw[2], R); }
    if (s->eqSat > 0.001) { double dr = 1.0 + s->eqSat * 3.0, a = s->eqAsym * 0.25;
        int hard = s->eqClip;
        double c0 = hard ? lb_clampd(a, -1.0, 1.0) : tanh(a);
        double den = (hard ? lb_clampd(dr + a, -1.0, 1.0) : tanh(dr + a)) - c0;
        if (fabs(den) < 1e-6) den = 1e-6;
        L = (adaa_run(L * dr + a, &s->adaaUL, &s->adaaFL, hard) - c0) / den;
        R = (adaa_run(R * dr + a, &s->adaaUR, &s->adaaFR, hard) - c0) / den; }
    L = bq_L(&s->loSh, L); L = bq_L(&s->midPk, L); L = bq_L(&s->hiSh, L);
    R = bq_R(&s->loSh, R); R = bq_R(&s->midPk, R); R = bq_R(&s->hiSh, R);
    *l = L * s->eqMakeup; *r = R * s->eqMakeup;
}

static inline void master_glue(fe_master *s, double amt, double *l, double *r, double SR) {
    if (amt < 0.01) return;
    double det = fmax(fabs(*l), fabs(*r));
    if (s->glueThrust > 0.001) {
        double m = 0.5 * (*l + *r);
        s->thrLp += (m - s->thrLp) * s->thrK;
        double hi = m - s->thrLp;
        double tl = fabs(hi * 2.37 + s->thrLp * 0.42);
        det += (tl - det) * s->glueThrust; }
    double atk = exp(-1.0 / (SR * s->glueAtk)), rel = exp(-1.0 / (SR * s->glueRel));
    double env = s->glueEnv;
    env = (det > env) ? atk * env + (1.0 - atk) * det : rel * env + (1.0 - rel) * det;
    s->glueEnv = env;
    double thDb = -12.0 * amt, db = 20.0 * log10(env + 1e-9), ratio = 1.5 + amt * 2.5;
    double over = db - thDb, kn = s->glueKnee, slope = 1.0 - 1.0 / ratio, grDb = 0.0;
    if (kn > 0.01) { if (over >= kn) grDb = (over - kn * 0.5) * slope;
                     else if (over > 0.0) grDb = (over * over / (2.0 * kn)) * slope; }
    else if (over > 0.0) grDb = over * slope;
    double g = pow(10.0, -grDb / 20.0);
    double mk = pow(10.0, (-thDb) * (1.0 - 1.0 / ratio) * 0.45 / 20.0 * amt);
    *l *= g * mk; *r *= g * mk;
    if (fabs(s->glueDist) > 0.001 && grDb > 0.05) {
        double gd = fabs(s->glueDist), d = gd * (grDb / 10.0); if (d > gd) d = gd;
        double b = (s->glueDist < 0.0) ? 0.18 : 0.0, t0 = lb_tanh(b);
        *l += ((lb_tanh(*l * 1.6 + b) - t0) * 0.92 - *l) * d;
        *r += ((lb_tanh(*r * 1.6 + b) - t0) * 0.92 - *r) * d; }
}

static inline void tape_limiter(fe_master *s, int chr, double lim, double *l, double *r) {
    if (lim <= 0.001) return;
    double drv = 1.0 + lim * 2.5;
    double det = fmax(fabs(*l), fabs(*r)) * drv;
    const double atk = 0.9285, rel = 0.99977;
    s->limEnv = (det > s->limEnv) ? atk * s->limEnv + (1.0 - atk) * det : rel * s->limEnv + (1.0 - rel) * det;
    double ceil_ = s->limCeil; double gr = (s->limEnv > ceil_) ? ceil_ / s->limEnv : 1.0;
    double nrm = 1.0 / fmax(1.0, lb_tanh(drv) * 1.287);
    if (s->eqClip && chr > 0) { *l = lb_clampd(*l * drv * gr, -ceil_, ceil_); *r = lb_clampd(*r * drv * gr, -ceil_, ceil_); }
    else { *l = lb_tanh(*l * drv * gr) * nrm; *r = lb_tanh(*r * drv * gr) * nrm; }
}

static inline double flushd(double x) { return fabs(x) < 1e-30 ? 0.0 : x; }

void fe_master_run(fe_master *s, float *outl, float *outr, int n, int chr, double locut, double hicut,
                   double gsat, double glue, double limit, double out, double SR) {
    int np = s->eqPoles, nb = (np == 6) ? 3 : 1;
    for (int i = 0; i < n; i++) {
        double mixL = outl[i], mixR = outr[i];
        if (gsat >= 0.005 && chr > 0) {
            double amt = gsat, dr = 1.0 + amt * 4.0, a = s->eqAsym * 0.25;
            double w = (amt < 0.05) ? amt / 0.05 : 1.0;
            double c0, den, yL, yR;
            if (s->eqClip) { c0 = lb_clampd(a, -1.0, 1.0); den = lb_clampd(dr + a, -1.0, 1.0) - c0;
                yL = (lb_clampd(mixL * dr + a, -1.0, 1.0) - c0); yR = (lb_clampd(mixR * dr + a, -1.0, 1.0) - c0); }
            else { c0 = tanh(a); den = tanh(dr + a) - c0;
                yL = (tanh(mixL * dr + a) - c0); yR = (tanh(mixR * dr + a) - c0); }
            if (fabs(den) < 1e-6) den = 1e-6;
            mixL += (yL / den - mixL) * w; mixR += (yR / den - mixR) * w;
        } else { mixL = global_saturate(mixL, gsat); mixR = global_saturate(mixR, gsat); }
        if (locut > 21.0) {
            for (int k = 0; k < nb; k++) { mixL = bq_L(&s->lo[k], mixL); mixR = bq_R(&s->lo[k], mixR); }
            if (np == 3) { s->loOneL += (mixL - s->loOneL) * s->loOneK; mixL -= s->loOneL;
                           s->loOneR += (mixR - s->loOneR) * s->loOneK; mixR -= s->loOneR; } }
        if (hicut < 19999.0) {
            for (int k = 0; k < nb; k++) { mixL = bq_L(&s->hi[k], mixL); mixR = bq_R(&s->hi[k], mixR); }
            if (np == 3) { s->hiOneL += (mixL - s->hiOneL) * s->hiOneK; mixL = s->hiOneL;
                           s->hiOneR += (mixR - s->hiOneR) * s->hiOneK; mixR = s->hiOneR; } }
        master_character(s, chr, &mixL, &mixR);
        master_glue(s, glue, &mixL, &mixR, SR);
        mixL *= out; mixR *= out;
        tape_limiter(s, chr, limit, &mixL, &mixR);
        outl[i] = (float)mixL; outr[i] = (float)mixR;
    }
    s->thrLp = flushd(s->thrLp); s->loOneL = flushd(s->loOneL); s->loOneR = flushd(s->loOneR);
    s->hiOneL = flushd(s->hiOneL); s->hiOneR = flushd(s->hiOneR);
}
