/*
 * Fouille engine — portable C99 core (no host, no UI).
 *
 * 16 voices, each a stereo tape in RAM anchored to a place. A background
 * harvest worker fetches field recordings near that place (Radio Aporee on
 * Internet Archive, Freesound by GPS), decodes them to the engine rate and
 * hands them over at a note boundary. The audio path (fe_render) never
 * allocates, locks, logs or touches a file.
 *
 * Threading contract:
 *   - fe_render: the audio thread only.
 *   - fe_control_* : ONE control thread (the host's param thread). Events are
 *     queued and applied at the start of the next fe_render.
 *   - fe_get_*: any thread; they read plain fields and may be one block stale.
 *
 * License: MIT (Fouille). minimp3: CC0.
 */
#ifndef FOUILLE_ENGINE_H
#define FOUILLE_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_NV 16

typedef struct fe_engine fe_engine;

typedef struct {
    float sample_rate;          /* 44100 on Move, 48000 on Pandore */
    int   max_block;            /* largest frames per fe_render call */
    float voice_seconds;        /* tape length per voice (30) */
    const char *curl_path;      /* NULL = search the usual places */
    const char *key_paths[4];   /* Freesound key files, first readable wins */
    int   offline;              /* 1 = never touch the network (tests) */
    uint32_t seed;
} fe_config;

/* Per-voice parameters, all normalized 0..1 at the API. */
enum {
    FE_P_PLACE = 0, FE_P_SOURCE, FE_P_RADIUS, FE_P_FLOW,
    FE_P_RISE, FE_P_HOLD, FE_P_FALL, FE_P_STRIKE,
    FE_P_SCATTER, FE_P_START, FE_P_LENGTH, FE_P_DIR,
    FE_P_SPEED, FE_P_GRAIN, FE_P_LOCK, FE_P_WEAR,
    FE_P_DRIFT, FE_P_TONE, FE_P_DRIVE,
    FE_P_PAN, FE_P_WIDTH, FE_P_POOL, FE_P_VOLUME,
    FE_P_COUNT
};

/* Global parameters. */
enum { FE_G_VOLUME = 0, FE_G_POOL_TIME, FE_G_POOL_FB, FE_G_COUNT };

/* Voice states (fe_voice_state). */
enum { FE_VS_EMPTY = 0, FE_VS_SEARCHING, FE_VS_READY, FE_VS_PLAYING };

/* Source of a fragment (fe_card.kind). */
enum { FE_SRC_NONE = 0, FE_SRC_APOREE, FE_SRC_FREESOUND, FE_SRC_SYNTH };

/* The "fiche": travels with the fragment everywhere. */
typedef struct {
    int   kind;
    char  title[112];
    char  author[64];
    char  license[24];          /* short form: "CC BY 4.0", "CC0", ... */
    char  url[160];
    char  place[32];            /* atlas place it was harvested for */
    float lat, lon;             /* where it was recorded (0,0 = unknown) */
    float seconds;              /* decoded length */
    float centroid_hz;          /* spectral centroid estimate */
    float loudness;             /* RMS 0..1 */
} fe_card;

fe_engine *fe_create(const fe_config *cfg);
void       fe_destroy(fe_engine *e);          /* joins the worker, kills a stuck curl */
void       fe_render(fe_engine *e, float *out_l, float *out_r, int frames);

/* Control (one thread). */
void fe_gate(fe_engine *e, int v, int on, float velocity);   /* on: start/retrigger; off: release */
void fe_pressure(fe_engine *e, int v, float p);
void fe_refetch(fe_engine *e, int v);                        /* "fouiller encore" */
void fe_freeze(fe_engine *e, int v, int on);                 /* figée: no automatic renewal */
void fe_set(fe_engine *e, int v, int id, float value01);     /* per-voice param */
void fe_set_global(fe_engine *e, int id, float value01);

/* Readback (any thread). */
float fe_get(const fe_engine *e, int v, int id);
float fe_get_global(const fe_engine *e, int id);
int   fe_voice_state(const fe_engine *e, int v);
int   fe_voice_busy(const fe_engine *e, int v);             /* a harvest is wanted or running */
int   fe_voice_frozen(const fe_engine *e, int v);
float fe_voice_wear(const fe_engine *e, int v);
float fe_voice_pos(const fe_engine *e, int v);               /* play head 0..1 of the tape */
void  fe_voice_card(const fe_engine *e, int v, fe_card *out);
int   fe_voice_wave(const fe_engine *e, int v, uint8_t *out, int n); /* n peak bins 0..255 */
int   fe_net_status(const fe_engine *e, char *buf, int len); /* short line for the header */

/* Atlas (built-in for now; lieux.json comes at step 3). */
int         fe_place_count(void);
const char *fe_place_name(int i);
int         fe_place_info(int i, float *lat, float *lon, float *radius_km);
/* Display string for a param value ("Plateau", "1.2 km", "120 ms"...). */
int  fe_format(const fe_engine *e, int v, int id, float value01, char *buf, int len);
int  fe_steps(int id);                                        /* >0: enum with n steps */

#ifdef __cplusplus
}
#endif
#endif
