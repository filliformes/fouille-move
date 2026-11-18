/*
 * Fouille — Move host (Schwung Overtake). Wraps the portable engine in
 * plugin_api_v2 and speaks the string-param bridge that ui.js uses.
 *
 * set_param keys (from ui.js):
 *   g   "v,on,vel"     gate a voice (vel 0..1)
 *   pr  "v,p"          pad pressure 0..1
 *   rf  "v"            fouiller encore (new fragment, swapped at a note boundary)
 *   fz  "v,on"         figer / liberer
 *   p   "v,id,val"     voice parameter (normalized)
 *   gp  "id,val"       global parameter
 *   x   "v,g,amt"      gesture amount (v = -1: all voices), right pads
 *   xp  "g,k,val"      gesture setting (knobs 5-8)
 *   xr  "g"            gesture settings back to defaults
 *   xa  "v"            Futur: adopt the prepared fragment now (v = -1: all)
 *   th  "v,hold"       tape transport, hold -1/0/1 (v = -1: all)
 *   session "save:N" | "load:N" | "delete:N" | "scan"   (disk I/O on the worker)
 *   clearAll "1"       every sound closed and emptied
 *   resetSel "v"       that sound's settings back to defaults (place and source stay)
 *   resetAll "1"       the same for all sixteen
 *   state  <blob>      restore (from get_param("state"))
 *
 * get_param keys:
 *   module_id, st (16 chars), card:v, pv:v, fmt:v,id,val, wave:v, pos:v,
 *   wear:v, net, gv, nplaces, state, xready (16 chars), xpv:g, xfmt:g,k,val,
 *   op (16 chars, '1' = held open), xst:g,v (one status line),
 *   sessStatus ("", "Saving..", "Loading..", "OK", "Empty"), sessNames (64 names joined by ';')
 * Unknown keys return -1.
 */
#define _POSIX_C_SOURCE 200809L
#include "plugin_api_v1.h"
#include "../engine/fe.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static const host_api_v1_t *g_host = NULL;

typedef struct {
    fe_engine *e;
    uint32_t dith;
    float l[MOVE_FRAMES_PER_BLOCK * 4], r[MOVE_FRAMES_PER_BLOCK * 4];
} inst_t;

static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;
    inst_t *s = calloc(1, sizeof(inst_t));
    if (!s) return NULL;
    static char kp[3][300];
    snprintf(kp[0], sizeof kp[0], "/data/UserData/UserLibrary/Fouille/freesound.key");
    snprintf(kp[1], sizeof kp[1], "%s/freesound.key", module_dir ? module_dir : ".");
    snprintf(kp[2], sizeof kp[2], "/data/UserData/schwung/modules/sound_generators/fouille/freesound.key");
    fe_config c = {0};
    c.sample_rate = (float)MOVE_SAMPLE_RATE;
    c.max_block = MOVE_FRAMES_PER_BLOCK * 4;
    c.voice_seconds = 30;
    c.key_paths[0] = kp[0]; c.key_paths[1] = kp[1]; c.key_paths[2] = kp[2];
    c.seed = 0x464F55u;
    c.session_dir = "/data/UserData/UserLibrary/Fouille";
    s->e = fe_create(&c);
    if (!s->e) { free(s); return NULL; }
    s->dith = 0x12345u;
    return s;
}

static void destroy_instance(void *p) {
    inst_t *s = p;
    if (!s) return;
    fe_destroy(s->e);
    free(s);
}

static void on_midi(void *p, const uint8_t *msg, int len, int source) {
    (void)p; (void)msg; (void)len; (void)source;   /* control lives in ui.js */
}

static int get_param(void *p, const char *key, char *buf, int len);
/* quiet = a session's settings: a new place must not send its sound away for another */
static void apply_state(inst_t *s, const char *val, int quiet) {
    /* "R:3;G:g0..g9;V0:p0,...;V1:...;F:frozen bits;X:x0,...(16 x 4)" — whitespace tolerant */
    const char *q = val;
    int rev = 0;                                   /* R:2 = coupe haut stored with 1 = open (v0.5.3); R:3 = G0 is Out (v0.6.0) */
    while (*q) {
        while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t' || *q == ';') q++;
        if (*q == 'R' && q[1] == ':') {
            char *end; rev = (int)strtol(q + 2, &end, 10); q = end;
        } else if (*q == 'G' && q[1] == ':') {
            q += 2;
            for (int i = 0; i < FE_G_COUNT; i++) { char *end; float f = strtof(q, &end); if (end == q) break; if (i == FE_G_VOLUME && rev < 3) f = f / 0.6f / 1.5f;   /* was the level after the bus, 0.6 = Out x1 */
                                                   fe_set_global(s->e, i, f); q = end; if (*q == ',') q++; }
        } else if (*q == 'V') {
            char *end; long vi = strtol(q + 1, &end, 10); q = end;
            if (*q == ':') q++;
            for (int i = 0; i < FE_P_COUNT; i++) {
                float f = strtof(q, &end); if (end == q) break;
                if (i == FE_P_HICUT && rev < 2) f = 1.0f - f;   /* v0.5.2 stored it the other way round */
                if (vi >= 0 && vi < FE_NV) { if (quiet) fe_set_quiet(s->e, (int)vi, i, f); else fe_set(s->e, (int)vi, i, f); }
                q = end; if (*q == ',') q++;
            }
        } else if (*q == 'X' && q[1] == ':') {           /* gesture settings, g*4+k */
            q += 2;
            for (int i = 0; i < FE_X_COUNT * FE_X_NPARAM; i++) {
                char *end; float f = strtof(q, &end); if (end == q) break;
                fe_gesture_param(s->e, i / FE_X_NPARAM, i % FE_X_NPARAM, f);
                q = end; if (*q == ',') q++;
            }
        } else if (*q == 'A' && q[1] == ':') {           /* latched gesture amounts: "v.g=amt,..." (v = -1: all) */
            q += 2;
            for (int g = 0; g < FE_X_COUNT; g++) { fe_gesture(s->e, -1, g, 0); for (int i = 0; i < FE_NV; i++) fe_gesture(s->e, i, g, 0); }
            while (*q && *q != ';') {
                char *end; long vv = strtol(q, &end, 10); if (end == q || *end != '.') break; q = end + 1;
                long g = strtol(q, &end, 10); if (end == q || *end != '=') break; q = end + 1;
                float f = strtof(q, &end); if (end == q) break; q = end;
                if (g >= 0 && g < FE_X_COUNT && vv >= -1 && vv < FE_NV) fe_gesture(s->e, (int)vv, (int)g, f);
                if (*q == ',') q++;
            }
        } else if (*q == 'F' && q[1] == ':') {
            char *end; long bits = strtol(q + 2, &end, 10); q = end;
            for (int i = 0; i < FE_NV; i++) fe_freeze(s->e, i, (int)((bits >> i) & 1));
        } else if (*q) q++;
    }
}

static void set_param(void *p, const char *key, const char *val) {
    inst_t *s = p;
    if (!s || !key || !val) return;
    int v = 0, a = 0; float x = 0;
    if (!strcmp(key, "g"))  { if (sscanf(val, "%d,%d,%f", &v, &a, &x) == 3) fe_gate(s->e, v, a, x); return; }
    if (!strcmp(key, "pr")) { if (sscanf(val, "%d,%f", &v, &x) == 2) fe_pressure(s->e, v, x); return; }
    if (!strcmp(key, "rf")) { if (sscanf(val, "%d", &v) == 1) fe_refetch(s->e, v); return; }
    if (!strcmp(key, "fz")) { if (sscanf(val, "%d,%d", &v, &a) == 2) fe_freeze(s->e, v, a); return; }
    if (!strcmp(key, "p"))  { if (sscanf(val, "%d,%d,%f", &v, &a, &x) == 3) fe_set(s->e, v, a, x); return; }
    if (!strcmp(key, "gp")) { if (sscanf(val, "%d,%f", &a, &x) == 2) fe_set_global(s->e, a, x); return; }
    if (!strcmp(key, "x"))  { if (sscanf(val, "%d,%d,%f", &v, &a, &x) == 3) fe_gesture(s->e, v, a, x); return; }
    if (!strcmp(key, "xp")) { if (sscanf(val, "%d,%d,%f", &v, &a, &x) == 3) fe_gesture_param(s->e, v, a, x); return; }
    if (!strcmp(key, "xr")) { if (sscanf(val, "%d", &v) == 1) fe_gesture_reset(s->e, v); return; }
    if (!strcmp(key, "xa")) { if (sscanf(val, "%d", &v) == 1) fe_adopt(s->e, v); return; }
    if (!strcmp(key, "state")) { apply_state(s, val, 0); return; }
    if (!strcmp(key, "th"))  { if (sscanf(val, "%d,%d", &v, &a) == 2) fe_tape_hold(s->e, v, a); return; }
    if (!strcmp(key, "clearAll")) { fe_clear_all(s->e); return; }
    if (!strcmp(key, "resetSel") || !strcmp(key, "resetAll")) {
        int all = key[5] == 'A', one = atoi(val);
        for (int i = 0; i < FE_NV; i++) {
            if (!all && i != one) continue;
            for (int id = 0; id < FE_P_COUNT; id++)
                if (id != FE_P_PLACE && id != FE_P_SOURCE) fe_set(s->e, i, id, fe_param_default(i, id));
        }
        return;
    }
    if (!strcmp(key, "session")) {
        const char *c = strchr(val, ':'); int n = c ? atoi(c + 1) : 1;
        if (!strncmp(val, "save", 4)) {
            static char st[12000];
            get_param(s, "state", st, (int)sizeof st);
            fe_session(s->e, FE_SS_SAVE, n, st);
        } else if (!strncmp(val, "load", 4)) fe_session(s->e, FE_SS_LOAD, n, NULL);
        else if (!strncmp(val, "delete", 6)) fe_session(s->e, FE_SS_DELETE, n, NULL);
        else fe_session(s->e, FE_SS_SCAN, 1, NULL);
        return;
    }
}

/* the gesture amounts that are on (latched, or held right now): "v.g=amt,..." */
static int latches(fe_engine *e, char *buf, int len) {
    int n = 0; buf[0] = 0;
    for (int v = -1; v < FE_NV; v++)
        for (int g = 0; g < FE_X_COUNT; g++) {
            float a = fe_gesture_get(e, v, g);
            if (a > 0.0005f && n < len - 16) n += snprintf(buf + n, (size_t)(len - n), "%s%d.%d=%.3f", n ? "," : "", v, g, a);
        }
    return n;
}

static int get_param(void *p, const char *key, char *buf, int len) {
    inst_t *s = p;
    if (!key || !buf || len < 2) return -1;
    if (!strcmp(key, "module_id")) return snprintf(buf, (size_t)len, "fouille");
    if (!s) return -1;
    fe_engine *e = s->e;
    int v = 0, id = 0; float x = 0;

    if (!strcmp(key, "st")) {                      /* per voice: '0' + state | busy<<2 | frozen<<3 | searching<<4 | next ready<<5 */
        if (len < FE_NV + 1) return -1;
        for (int i = 0; i < FE_NV; i++)
            buf[i] = (char)('0' + (fe_voice_state(e, i) | (fe_voice_busy(e, i) << 2) | (fe_voice_frozen(e, i) << 3)
                                  | (fe_voice_searching(e, i) << 4) | (fe_voice_has_next(e, i) << 5)));
        buf[FE_NV] = 0;
        return FE_NV;
    }
    if (!strncmp(key, "card:", 5)) {
        v = atoi(key + 5);
        fe_card c; fe_voice_card(e, v, &c);
        static const char *K[] = { "", "Aporee", "Freesound", "Synthese" };
        return snprintf(buf, (size_t)len, "%s|%s|%s|%s|%s|%.1f|%.4f|%.4f|%s",
                        c.place, K[c.kind & 3], c.title, c.author, c.license, c.seconds, c.lat, c.lon, c.url);
    }
    if (!strncmp(key, "pv:", 3)) {
        v = atoi(key + 3);
        int n = 0;
        for (int i = 0; i < FE_P_COUNT && n < len - 8; i++) n += snprintf(buf + n, (size_t)(len - n), i ? ",%.4f" : "%.4f", fe_get(e, v, i));
        return n;
    }
    if (!strncmp(key, "fmt:", 4)) {
        if (sscanf(key + 4, "%d,%d,%f", &v, &id, &x) != 3) return -1;
        return fe_format(e, v, id, x, buf, len);
    }
    if (!strncmp(key, "wave:", 5)) {                /* 64 bins as 'A'+0..31 */
        v = atoi(key + 5);
        uint8_t w[64];
        if (len < 65) return -1;
        fe_voice_wave(e, v, w, 64);
        for (int i = 0; i < 64; i++) buf[i] = (char)('A' + (w[i] >> 3));
        buf[64] = 0;
        return 64;
    }
    if (!strncmp(key, "pos:", 4))  return snprintf(buf, (size_t)len, "%.4f", fe_voice_pos(e, atoi(key + 4)));
    if (!strncmp(key, "wear:", 5)) return snprintf(buf, (size_t)len, "%.3f", fe_voice_wear(e, atoi(key + 5)));
    if (!strcmp(key, "net"))       return fe_net_status(e, buf, len);
    if (!strncmp(key, "place:", 6)) {
        int i = atoi(key + 6);
        float la, lo, km;
        if (!fe_place_info(i, &la, &lo, &km)) return -1;
        return snprintf(buf, (size_t)len, "%s|%.4f|%.4f|%.2f", fe_place_name(i), la, lo, km);
    }
    if (!strcmp(key, "op")) {                      /* per voice: '1' = held open (not closed, not in its tail) */
        if (len < FE_NV + 1) return -1;
        for (int i = 0; i < FE_NV; i++) buf[i] = fe_voice_open(e, i) ? '1' : '0';
        buf[FE_NV] = 0;
        return FE_NV;
    }
    if (!strncmp(key, "xst:", 4)) {                /* what gesture g does to voice v now */
        int g;
        if (sscanf(key + 4, "%d,%d", &g, &v) != 2) return -1;
        return fe_gesture_status(e, g, v, buf, len);
    }
    if (!strcmp(key, "xready")) {                  /* '1' = this pad is built */
        if (len < FE_X_COUNT + 1) return -1;
        for (int g = 0; g < FE_X_COUNT; g++) buf[g] = fe_gesture_ready(g) ? '1' : '0';
        buf[FE_X_COUNT] = 0;
        return FE_X_COUNT;
    }
    if (!strncmp(key, "xpv:", 4)) {
        int g = atoi(key + 4);
        if (g < 0 || g >= FE_X_COUNT) return -1;
        return snprintf(buf, (size_t)len, "%.4f,%.4f,%.4f,%.4f", fe_gesture_param_get(e, g, 0), fe_gesture_param_get(e, g, 1),
                        fe_gesture_param_get(e, g, 2), fe_gesture_param_get(e, g, 3));
    }
    if (!strncmp(key, "xfmt:", 5)) {
        if (sscanf(key + 5, "%d,%d,%f", &v, &id, &x) != 3) return -1;
        return fe_gesture_format(v, id, x, buf, len);
    }
    if (!strcmp(key, "nplaces"))   return snprintf(buf, (size_t)len, "%d", fe_place_count());
    if (!strcmp(key, "gv")) {
        int n = 0;
        for (int i = 0; i < FE_G_COUNT && n < len - 10; i++) n += snprintf(buf + n, (size_t)(len - n), i ? ",%.5f" : "%.5f", fe_get_global(e, i));
        return n;
    }
    if (!strcmp(key, "sessStatus")) {
        const char *st = fe_session_take_state(e);
        if (st) apply_state(s, st, 1);                 /* a load finished: its settings, on this thread */
        static const char *S[] = { "", "Saving..", "Loading..", "OK", "Empty" };
        int i = fe_session_status(e); if (i < 0 || i > 4) i = 0;
        return snprintf(buf, (size_t)len, "%s", S[i]);
    }
    if (!strcmp(key, "sessNames")) return fe_session_names(e, buf, len);
    if (!strcmp(key, "lat")) return latches(e, buf, len);
    if (!strncmp(key, "char:", 5)) return snprintf(buf, (size_t)len, "%s", fe_char_name(atoi(key + 5)));
    if (!strcmp(key, "state")) {
        int n = snprintf(buf, (size_t)len, "R:3;G:");
        for (int i = 0; i < FE_G_COUNT; i++) n += snprintf(buf + n, (size_t)(len - n), i ? ",%.5f" : "%.5f", fe_get_global(e, i));
        int bits = 0;
        for (int i = 0; i < FE_NV && n < len - 300; i++) {
            n += snprintf(buf + n, (size_t)(len - n), ";V%d:", i);
            for (int k = 0; k < FE_P_COUNT; k++) n += snprintf(buf + n, (size_t)(len - n), k ? ",%.3f" : "%.3f", fe_get(e, i, k));
            bits |= fe_voice_frozen(e, i) << i;
        }
        n += snprintf(buf + n, (size_t)(len - n), ";F:%d", bits);
        if (n < len - 1200) { n += snprintf(buf + n, (size_t)(len - n), ";A:"); n += latches(e, buf + n, len - n); }
        if (n < len - FE_X_COUNT * FE_X_NPARAM * 6 - 4) {
            n += snprintf(buf + n, (size_t)(len - n), ";X:");
            for (int i = 0; i < FE_X_COUNT * FE_X_NPARAM; i++)
                n += snprintf(buf + n, (size_t)(len - n), i ? ",%.3f" : "%.3f", fe_gesture_param_get(e, i / FE_X_NPARAM, i % FE_X_NPARAM));
        }
        return n;
    }
    return -1;
}

/* Loopex lb_quant16: round to nearest + TPDF dither, exact silence passes undithered */
static inline double rnd_pm1(uint32_t *rng) {
    *rng = 214013u * (*rng) + 2531011u;
    return ((double)((*rng >> 16) & 0x7FFF) / 32768.0) * 2.0 - 1.0;
}
static inline int16_t quant16(float xf, uint32_t *rng) {
    double x = xf;
    if (x != x) x = 0;
    if (x > 1.0) x = 1.0; else if (x < -1.0) x = -1.0;
    if (x == 0.0) return 0;
    double q = floor(x * 32767.0 + (rnd_pm1(rng) + rnd_pm1(rng)) * 0.5 + 0.5);
    if (q > 32767.0) q = 32767.0; else if (q < -32767.0) q = -32767.0;
    return (int16_t)q;
}

static void render_block(void *p, int16_t *out, int frames) {
    inst_t *s = p;
    if (!s) { memset(out, 0, (size_t)frames * 4); return; }
    if (frames > MOVE_FRAMES_PER_BLOCK * 4) frames = MOVE_FRAMES_PER_BLOCK * 4;
    fe_render(s->e, s->l, s->r, frames);
    for (int i = 0; i < frames; i++) {               /* the final quantise is dithered, like Loopex */
        out[2 * i] = quant16(s->l[i], &s->dith);
        out[2 * i + 1] = quant16(s->r[i], &s->dith);
    }
}

static plugin_api_v2_t api = {
    .api_version = 2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .get_error = NULL,
    .render_block = render_block,
};

__attribute__((visibility("default")))
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) { g_host = host; return &api; }
