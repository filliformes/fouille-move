/* Fouille engine — internals shared by fe_engine.c and fe_harvest.c. */
#ifndef FOUILLE_INTERNAL_H
#define FOUILLE_INTERNAL_H

#include "fe.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>

#define FE_NFUT     6                  /* free tapes for Futur (a prepared next fragment) */
#define FE_NTAPES   (2 * FE_NV + 1 + FE_NFUT)  /* current + past per voice, one spare the harvest writes into, Futur pool */
#define FE_QSIZE    512                /* control events, power of two */
#define FE_WAVE_N   128                /* waveform overview bins per tape */

typedef struct {
    float lat, lon, radius_km;
    const char *name;                  /* screen name, <= 14 chars */
    const char *words;                 /* keyword fallback, '+'-separated */
} fe_place;

extern const fe_place FE_PLACES[];
extern const int FE_NPLACES;

typedef struct {
    int16_t *pcm;                      /* interleaved stereo */
    int      frames;                   /* valid frames */
    int      cap;                      /* capacity in frames */
    fe_card  card;
    uint8_t  wave[FE_WAVE_N];
} fe_tape;

/* What the worker should look for, snapshotted by the audio thread. */
typedef struct {
    int place, source;
    float radius_scale;
    float drift_km, drift_deg;         /* Derive geosonique: offset from the place (0 = at home) */
} fe_request;

typedef struct { uint8_t type, v, id; float val; } fe_event;

enum { EV_GATE_ON = 1, EV_GATE_OFF, EV_PRESS, EV_REFETCH, EV_FREEZE, EV_SET, EV_GSET, EV_GEST, EV_XPARAM, EV_XRESET, EV_ADOPT };
enum { ENV_OFF = 0, ENV_RISE, ENV_SUSTAIN, ENV_FALL };

typedef struct {
    float x, v;                        /* brownian value and velocity */
    uint32_t rng;
} fe_drift;

typedef struct {
    fe_tape *tape;                     /* owned by the audio thread */
    float p[FE_P_COUNT];

    /* play state */
    int   env_stage;
    float env, vel, press;
    float pos;                         /* frames, fractional */
    float loop_a, loop_len;            /* region in frames */
    float grain_ph[2], grain_org[2];
    float duck;                        /* 1 = open; ramps to 0 before a swap */
    int   swapping;
    int   reseek;                      /* after the duck: back to position 0 (reopen in a tail) */
    float wear;                        /* 0 fresh .. 1 breath */
    int   frozen;
    double since_swap;                 /* seconds */

    /* DSP state */
    float vac;                         /* vactrol photoresistor */
    float lp1[2], lp2[2];              /* LPG two-pole */
    float wlp[2];                      /* wear low-pass */
    float tlp[2];                      /* tone split */
    float bp1[2], bp2[2];              /* lock SVF */
    float hiss_lp;
    fe_drift dpitch, dpan, dpos;
    uint32_t rng;

    /* gestures */
    float vx[FE_X_COUNT];              /* this voice's own amounts */
    float xt_prev[FE_X_COUNT];         /* last effective target, for onsets */
    float xs[FE_X_COUNT];              /* smoothed effective amounts */
    float bong, bong_rel, bong_min, bong_peak;   /* Bongo strike */
    int   bong_stage, bong_delay;
    float gel_mix, gel_org, gel_ph[2], gel_go[2], gel_breath; /* Gel */
    int   gel_on;
    float acc_hz, acc_lp, acc_prev, acc_cnt, acc_s1[2], acc_s2[2]; /* Accroche */
    int   acc_arm;
    float drift_km, drift_deg;         /* Derive: where the voice is now (0 = home) */
    float drift_wait, drift_peak, drift_hop;
    int   drift_armed, drift_redo;
    /* Passe / Futur: the fragment before, the fragment prepared next */
    fe_tape *past, *next;
    float past_pos, next_pos, past_mix, next_mix, past_lp[2], next_lp[2];
    int   past_delay, past_on, want_next, adopt;
    /* Pulsars */
    float pul_ph, pul_per, pul_mix;
    int   pul_mask;
    /* Corps: 6 modes x 2 channels, two-pole resonators */
    float md_y1[6][2], md_y2[6][2], md_x1[2], md_x2[2], corps_mix;
    /* Etirement: one factor for every time of the gesture */
    float tf, st_ph[2], st_org[2], st_mix;
    /* Voisin de timbre */
    int   vn_u, vn_want;
    float vn_pos, vn_mix, vn_T;
    /* Reecriture */
    float rw_lp[2], rw_sweep, rw_done; /* the sweep wears the region ahead of the head; done = passes worn */
    int   rw_hole, rw_on;
    float ghost_pos;                   /* Contagion: a closed voice still lends its tape, read here */
    /* ternes: Terne, Double hasard, Hesitation */
    float t_env, t_amp, t_rise, t_line, t_mix;
    int   t_stage, t_k, t_fam, t_wait, t_on;
    uint32_t t_rng;
    int   auto_close;                  /* Hesitation: a closed voice replayed shortly, closes itself */

    atomic_int want;                   /* worker: 1 = harvest wanted */
    fe_request req;                    /* valid when want == 1 */
    atomic_int state_pub;              /* FE_VS_* for readers */
    atomic_int open_pub;               /* the gesture is held open (rise or sustain), for the pad LED */
    atomic_int searching;
} fe_voice;

struct fe_engine {
    fe_config cfg;
    float sr;
    int   tape_cap;

    fe_tape  tapes[FE_NTAPES];
    fe_voice voices[FE_NV];
    float g[FE_G_COUNT];

    /* Lock-free hand-over of the spare tape */
    fe_tape *spare;                    /* worker writes it while pending == -1 */
    atomic_int pending;                /* voice that should take the spare, or -1 */

    /* control queue (SPSC) */
    fe_event q[FE_QSIZE];
    atomic_uint q_head, q_tail;

    /* echo pool */
    float *pool_l, *pool_r;
    int    pool_len, pool_w;
    float  pool_lp[2];

    /* worker */
    pthread_t   th;
    int         th_started;
    atomic_int  running;
    atomic_int  child_pid;             /* running curl, 0 = none */
    char        curl[160];
    char        key[96];
    atomic_int  net_ok;                /* last network attempt worked */
    atomic_int  harvests, failures;
    char        net_msg[48];
    uint32_t    wrng;

    /* gestures */
    float gx[FE_X_COUNT];              /* global amounts */
    float xp[FE_X_COUNT][FE_X_NPARAM]; /* settings, knobs 5-8 */
    int    vrend[FE_NV];               /* voice rendered (Contagion sources) */
    int    vghost[FE_NV];              /* closed voice whose tape was read into vbuf this block */
    float  vlat[FE_NV], vlon[FE_NV];   /* where each voice listens, this block */
    float *vbuf;                       /* Contagion: [FE_NV][2 kinds][2 ch][512], last block */
    fe_tape *freet[FE_NFUT];           /* Futur: free tapes (audio thread only) */
    int    nfree;
    float  hes_wait, hes_mult;         /* Hesitation clock: seconds to the next pulse */
    int    hes_n;
    uint32_t hrng;
    float *lastmix;                    /* Reecriture "le mix": previous block, mono */
    float *rs_l, *rs_r;                /* Ressac line */
    int    rs_len, rs_w;
    float  rs_lp[2], rs_hp[2], rs_ap[2], rs_amt, rs_ret;
    float  rs_ph, rs_d;                /* Ressac waves: phase, current delay (samples, glides) */

    float *scratch_l, *scratch_r;      /* render mix, max_block */
    float *dec;                        /* worker: decoded float stereo at source rate */
    int    dec_cap;
    uint8_t *net_buf;                  /* worker: download buffer */
    int    net_cap;
};

/* fe_harvest.c */
int  fe_harvest_start(fe_engine *e);
void fe_harvest_stop(fe_engine *e);
void fe_tape_analyse(fe_tape *t, float sr);
void fe_synth_fill(fe_tape *t, float sr, uint32_t *rng, int place);

static inline uint32_t fe_rand(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x ? x : 0x9E3779B9u; return x;
}
static inline float fe_randf(uint32_t *s) { return (float)(fe_rand(s) >> 8) * (1.0f / 16777216.0f); }

#endif
