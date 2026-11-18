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
 *   state  <blob>      restore (from get_param("state"))
 *
 * get_param keys:
 *   module_id, st (16 chars), card:v, pv:v, fmt:v,id,val, wave:v, pos:v,
 *   wear:v, net, gv, nplaces, state, xready (16 chars), xpv:g, xfmt:g,k,val,
 *   op (16 chars, '1' = held open), xst:g,v (one status line)
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
    s->e = fe_create(&c);
    if (!s->e) { free(s); return NULL; }
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
    if (!strcmp(key, "state")) {
        /* "G:g0,g1,g2;V0:p0,...;V1:...;F:frozen bits;X:x0,...(16 x 4)" — whitespace tolerant */
        const char *q = val;
        while (*q) {
            while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t' || *q == ';') q++;
            if (*q == 'G' && q[1] == ':') {
                q += 2;
                for (int i = 0; i < FE_G_COUNT; i++) { char *end; float f = strtof(q, &end); if (end == q) break; fe_set_global(s->e, i, f); q = end; if (*q == ',') q++; }
            } else if (*q == 'V') {
                char *end; long vi = strtol(q + 1, &end, 10); q = end;
                if (*q == ':') q++;
                for (int i = 0; i < FE_P_COUNT; i++) {
                    float f = strtof(q, &end); if (end == q) break;
                    if (vi >= 0 && vi < FE_NV) fe_set(s->e, (int)vi, i, f);
                    q = end; if (*q == ',') q++;
                }
            } else if (*q == 'X' && q[1] == ':') {           /* gesture settings, g*4+k */
                q += 2;
                for (int i = 0; i < FE_X_COUNT * FE_X_NPARAM; i++) {
                    char *end; float f = strtof(q, &end); if (end == q) break;
                    fe_gesture_param(s->e, i / FE_X_NPARAM, i % FE_X_NPARAM, f);
                    q = end; if (*q == ',') q++;
                }
            } else if (*q == 'F' && q[1] == ':') {
                char *end; long bits = strtol(q + 2, &end, 10); q = end;
                for (int i = 0; i < FE_NV; i++) fe_freeze(s->e, i, (int)((bits >> i) & 1));
            } else if (*q) q++;
        }
        return;
    }
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
    if (!strcmp(key, "gv"))        return snprintf(buf, (size_t)len, "%.4f,%.4f,%.4f", fe_get_global(e, 0), fe_get_global(e, 1), fe_get_global(e, 2));
    if (!strcmp(key, "state")) {
        int n = snprintf(buf, (size_t)len, "G:%.4f,%.4f,%.4f", fe_get_global(e, 0), fe_get_global(e, 1), fe_get_global(e, 2));
        int bits = 0;
        for (int i = 0; i < FE_NV && n < len - 300; i++) {
            n += snprintf(buf + n, (size_t)(len - n), ";V%d:", i);
            for (int k = 0; k < FE_P_COUNT; k++) n += snprintf(buf + n, (size_t)(len - n), k ? ",%.3f" : "%.3f", fe_get(e, i, k));
            bits |= fe_voice_frozen(e, i) << i;
        }
        n += snprintf(buf + n, (size_t)(len - n), ";F:%d", bits);
        if (n < len - FE_X_COUNT * FE_X_NPARAM * 6 - 4) {
            n += snprintf(buf + n, (size_t)(len - n), ";X:");
            for (int i = 0; i < FE_X_COUNT * FE_X_NPARAM; i++)
                n += snprintf(buf + n, (size_t)(len - n), i ? ",%.3f" : "%.3f", fe_gesture_param_get(e, i / FE_X_NPARAM, i % FE_X_NPARAM));
        }
        return n;
    }
    return -1;
}

static void render_block(void *p, int16_t *out, int frames) {
    inst_t *s = p;
    if (!s) { memset(out, 0, (size_t)frames * 4); return; }
    if (frames > MOVE_FRAMES_PER_BLOCK * 4) frames = MOVE_FRAMES_PER_BLOCK * 4;
    fe_render(s->e, s->l, s->r, frames);
    for (int i = 0; i < frames; i++) {
        float l = s->l[i], r = s->r[i];
        l = l > 1 ? 1 : (l < -1 ? -1 : l);
        r = r > 1 ? 1 : (r < -1 ? -1 : r);
        out[2 * i] = (int16_t)lrintf(l * 32767.0f);
        out[2 * i + 1] = (int16_t)lrintf(r * 32767.0f);
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
