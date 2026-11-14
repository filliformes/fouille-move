/*
 * Fouille engine — the harvest worker.
 *
 * One background thread. For a voice that wants material it asks the place's
 * sources for a recording made nearby, downloads part of it with curl
 * (posix_spawn, no shell: the URL never meets a shell, and a stuck curl can
 * be killed), decodes the MP3, resamples it to the engine rate into the
 * spare tape and hands it to the audio thread.
 *
 *   Radio Aporee (Internet Archive): items carry latitude/longitude. The IA
 *     search takes a latitude band; longitude is filtered here (a negative
 *     number in an IA range query is refused).
 *   Freesound: {!geofilt sfield=geotag pt=lat,lon d=km}, needs a key.
 *
 * Nothing here runs on the audio thread, so it may allocate and block.
 */
#define _GNU_SOURCE
#include "fe_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <sys/wait.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"

extern char **environ;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *CURLS[] = {
    "/data/UserData/schwung/bin/curl", "/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", NULL
};

static void msleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

/* ── curl, without a shell ──────────────────────────────────────────────── */

/* Runs curl with `args` (NULL-terminated, curl itself not included) and reads
 * its stdout into buf. Returns bytes read, -1 on failure to start. */
static int run_curl(fe_engine *e, const char **args, uint8_t *buf, int cap) {
    if (!e->curl[0]) return -1;
    const char *argv[24];
    int a = 0;
    argv[a++] = e->curl;
    for (int i = 0; args[i] && a < 23; i++) argv[a++] = args[i];
    argv[a] = NULL;

    int fd[2];
    if (pipe(fd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    posix_spawn_file_actions_addclose(&fa, fd[1]);
    pid_t pid;
    int rc = posix_spawn(&pid, e->curl, &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (rc != 0) { close(fd[0]); return -1; }
    atomic_store(&e->child_pid, (int)pid);

    int total = 0;
    struct pollfd p = { fd[0], POLLIN, 0 };
    while (total < cap) {
        if (!atomic_load(&e->running)) { kill(pid, SIGKILL); break; }
        int r = poll(&p, 1, 200);
        if (r < 0) break;
        if (r == 0) continue;
        ssize_t n = read(fd[0], buf + total, (size_t)(cap - total));
        if (n <= 0) break;
        total += (int)n;
    }
    if (total >= cap) kill(pid, SIGKILL);          /* got what we wanted */
    close(fd[0]);
    int st; waitpid(pid, &st, 0);
    atomic_store(&e->child_pid, 0);
    return total;
}

/* ── a small JSON reader (structure-aware, unlike strstr) ───────────────── */

static const char *ws(const char *p) { while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }

static const char *skip_str(const char *p) {          /* p at '"' */
    p++;
    while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
    return *p ? p + 1 : NULL;
}
static const char *skip_val(const char *p) {
    p = ws(p);
    if (!p || !*p) return NULL;
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = skip_str(p); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}
/* Value of member `key` of the object at obj (top level only), or NULL. */
static const char *jget(const char *obj, const char *key) {
    obj = ws(obj);
    if (!obj || *obj != '{') return NULL;
    const char *p = ws(obj + 1);
    size_t kl = strlen(key);
    while (p && *p == '"') {
        const char *k = p + 1, *ke = skip_str(p);
        if (!ke) return NULL;
        p = ws(ke);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if ((size_t)(ke - 1 - k) == kl && strncmp(k, key, kl) == 0) return p;
        p = ws(skip_val(p));
        if (!p || *p != ',') return NULL;
        p = ws(p + 1);
    }
    return NULL;
}
static char fold(unsigned u) {
    static const char *L = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (u < 0x80) return (char)u;
    if (u >= 0xC0 && u <= 0xFF) return L[u - 0xC0];
    if (u == 0x2019 || u == 0x2018) return 0x27;
    if (u == 0x2013 || u == 0x2014) return '-';
    return '?';
}
/* Copy a string (unescaped) or a bare token. Returns length, -1 if none. */
static int jstr(const char *v, char *out, int cap) {
    if (!v || cap < 1) return -1;
    v = ws(v);
    int n = 0;
    if (*v == '"') {
        v++;
        while (*v && *v != '"' && n < cap - 1) {
            char c = *v++;
            if (c == '\\' && *v) {
                char x = *v++;
                switch (x) {
                case 'n': c = ' '; break; case 't': c = ' '; break; case 'r': c = ' '; break;
                case 'u': {                            /* fold Latin-1 to ASCII: the screen font is ASCII */
                    unsigned u = 0;
                    for (int i = 0; i < 4 && isxdigit((unsigned char)*v); i++, v++)
                        u = u * 16 + (unsigned)(isdigit((unsigned char)*v) ? *v - '0' : (tolower((unsigned char)*v) - 'a' + 10));
                    c = fold(u);
                    break; }
                default:  c = x; break;
                }
            }
            else if ((unsigned char)c == 0xC3 && *v) { c = fold(0xC0 + ((unsigned char)*v++ & 0x3F)); }
            else if ((unsigned char)c >= 0x80) { while (((unsigned char)*v & 0xC0) == 0x80) v++; c = '?'; }
            out[n++] = c;
        }
    } else {
        const char *end = skip_val(v);
        if (!end) return -1;
        while (v < end && n < cap - 1) out[n++] = *v++;
        if (n == 4 && strncmp(out, "null", 4) == 0) n = 0;
    }
    out[n] = 0;
    return n;
}
static double jnum(const char *v) { char b[48]; return jstr(v, b, sizeof b) > 0 ? atof(b) : 0.0; }
static const char *jfirst(const char *arr) { arr = ws(arr); if (!arr || *arr != '[') return NULL; arr = ws(arr + 1); return *arr == ']' ? NULL : arr; }
static const char *jnext(const char *el) { const char *p = ws(skip_val(el)); if (!p || *p != ',') return NULL; return ws(p + 1); }

/* ── helpers ────────────────────────────────────────────────────────────── */

static void urlenc(const char *s, char *out, int cap) {
    static const char hx[] = "0123456789ABCDEF";
    int n = 0;
    for (; *s && n < cap - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hx[c >> 4]; out[n++] = hx[c & 15]; }
    }
    out[n] = 0;
}
static int safe_url(const char *u) {                   /* we pass argv, but stay strict anyway */
    if (strncmp(u, "https://", 8) && strncmp(u, "http://", 7)) return 0;
    for (; *u; u++) if (*u == '\n' || *u == '\r' || *u == ' ') return 0;
    return 1;
}
static float km_between(float la1, float lo1, float la2, float lo2) {
    const float R = 6371.0f, d2r = (float)M_PI / 180.0f;
    float dla = (la2 - la1) * d2r, dlo = (lo2 - lo1) * d2r;
    float a = sinf(dla / 2) * sinf(dla / 2) + cosf(la1 * d2r) * cosf(la2 * d2r) * sinf(dlo / 2) * sinf(dlo / 2);
    return 2 * R * asinf(sqrtf(fminf(1.0f, a)));
}
static void short_license(const char *url, char *out, int cap) {
    const char *p = strstr(url, "publicdomain/zero");
    if (p) { snprintf(out, (size_t)cap, "CC0"); return; }
    if (strstr(url, "publicdomain")) { snprintf(out, (size_t)cap, "Dom. public"); return; }
    if (strstr(url, "sampling+")) { snprintf(out, (size_t)cap, "Sampling+"); return; }
    p = strstr(url, "/licenses/");
    if (!p) { snprintf(out, (size_t)cap, "%s", url[0] ? "?" : ""); return; }
    p += 10;
    char kind[24] = {0}, ver[8] = {0};
    int i = 0;
    while (*p && *p != '/' && i < 23) { kind[i++] = (char)toupper((unsigned char)*p); p++; }
    if (*p == '/') { p++; i = 0; while (*p && *p != '/' && i < 7) ver[i++] = *p++; }
    snprintf(out, (size_t)cap, "CC %s%s%s", kind, ver[0] ? " " : "", ver);
}
static void set_msg(fe_engine *e, const char *fmt, const char *arg) {
    snprintf(e->net_msg, sizeof e->net_msg, fmt, arg ? arg : "");
}

/* ── decode + resample into a tape ──────────────────────────────────────── */

/* Decodes mp3 bytes into e->dec (float stereo at the file's rate). */
static int decode_mp3(fe_engine *e, const uint8_t *mp3, int size, int *rate) {
    static mp3dec_t dec;                               /* worker-only */
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int frames = 0, off = 0;
    *rate = 0;
    while (off < size && frames < e->dec_cap) {
        int s = mp3dec_decode_frame(&dec, mp3 + off, size - off, pcm, &info);
        if (s == 0 && info.frame_bytes == 0) break;
        off += info.frame_bytes;
        if (s <= 0) continue;
        if (!*rate) *rate = info.hz;
        if (info.hz != *rate) continue;                /* never mix rates in one tape */
        for (int i = 0; i < s && frames < e->dec_cap; i++, frames++) {
            float l = info.channels == 2 ? pcm[2 * i] : pcm[i];
            float r = info.channels == 2 ? pcm[2 * i + 1] : pcm[i];
            e->dec[2 * frames] = l; e->dec[2 * frames + 1] = r;
        }
    }
    return frames;
}

/* Resample e->dec (src_rate) to the engine rate into tape (fixes v0.2 bug 3:
 * a 48 kHz file no longer plays 1.5 semitones low). Normalizes gently. */
static int fill_tape(fe_engine *e, fe_tape *t, int src_frames, int src_rate) {
    if (src_frames < 2 || src_rate < 8000) return 0;
    double ratio = (double)src_rate / (double)e->sr;
    int out = (int)((double)(src_frames - 1) / ratio);
    if (out > t->cap) out = t->cap;
    if (out < (int)(0.5f * e->sr)) return 0;           /* under half a second: not worth a voice */

    float peak = 1e-6f;
    for (int i = 0; i < src_frames * 2; i++) { float a = fabsf(e->dec[i]); if (a > peak) peak = a; }
    float gain = fminf(0.89f / peak, 10.0f);           /* to -1 dBFS, at most +20 dB */

    int fade = (int)(0.005f * e->sr);
    for (int i = 0; i < out; i++) {
        double sp = i * ratio;
        int k = (int)sp; float f = (float)(sp - k);
        if (k >= src_frames - 1) { k = src_frames - 2; f = 1; }
        float l = e->dec[2 * k] + (e->dec[2 * k + 2] - e->dec[2 * k]) * f;
        float r = e->dec[2 * k + 1] + (e->dec[2 * k + 3] - e->dec[2 * k + 1]) * f;
        float g = gain;
        if (i < fade) g *= (float)i / (float)fade;
        if (i > out - fade) g *= (float)(out - i) / (float)fade;
        l *= g; r *= g;
        l = l > 1 ? 1 : (l < -1 ? -1 : l);
        r = r > 1 ? 1 : (r < -1 ? -1 : r);
        t->pcm[2 * i] = (int16_t)(l * 32767.0f);
        t->pcm[2 * i + 1] = (int16_t)(r * 32767.0f);
    }
    t->frames = out;
    return out;
}

void fe_tape_analyse(fe_tape *t, float sr) {
    int n = t->frames;
    double sum = 0; int zc = 0;
    int prev = 0;
    for (int i = 0; i < n; i++) {
        int m = ((int)t->pcm[2 * i] + (int)t->pcm[2 * i + 1]) / 2;
        sum += (double)m * m;
        if (i && ((m >= 0) != (prev >= 0))) zc++;
        prev = m;
    }
    t->card.loudness = n ? (float)sqrt(sum / n) / 32768.0f : 0;
    t->card.centroid_hz = n ? (float)zc / (float)n * sr * 0.5f : 0;
    t->card.seconds = (float)n / sr;
    for (int b = 0; b < FE_WAVE_N; b++) {
        int a = (int)((long long)b * n / FE_WAVE_N), z = (int)((long long)(b + 1) * n / FE_WAVE_N);
        int pk = 0;
        for (int i = a; i < z; i++) { int x = abs(t->pcm[2 * i]); if (x > pk) pk = x; }
        t->wave[b] = (uint8_t)(sqrtf((float)pk / 32768.0f) * 255.0f);
    }
}

/* Synthetic texture: the fallback when there is nothing to listen to. */
void fe_synth_fill(fe_tape *t, float sr, uint32_t *rng, int place) {
    int n = (int)(sr * (6.0f + 6.0f * fe_randf(rng)));
    if (n > t->cap) n = t->cap;
    float fc = 200.0f + 3000.0f * fe_randf(rng);
    float a = 1.0f - expf(-2.0f * (float)M_PI * fc / sr);
    float zl = 0, zr = 0, env = 0.5f, dec = 1.0f - 1.0f / (sr * (0.05f + 0.6f * fe_randf(rng)));
    for (int i = 0; i < n; i++) {
        float nl = fe_randf(rng) * 2 - 1, nr = fe_randf(rng) * 2 - 1;
        zl += a * (nl - zl); zr += a * (nr * 0.6f + nl * 0.4f - zr);
        env *= dec;
        if ((fe_rand(rng) & 0x1FFF) < 3) env = 0.3f + 0.5f * fe_randf(rng);
        float g = env;
        int fade = (int)(0.005f * sr);
        if (i < fade) g *= (float)i / fade;
        if (i > n - fade) g *= (float)(n - i) / fade;
        t->pcm[2 * i] = (int16_t)(fmaxf(-1, fminf(1, zl * g * 2.5f)) * 32767);
        t->pcm[2 * i + 1] = (int16_t)(fmaxf(-1, fminf(1, zr * g * 2.5f)) * 32767);
    }
    t->frames = n;
    memset(&t->card, 0, sizeof t->card);
    t->card.kind = FE_SRC_SYNTH;
    snprintf(t->card.title, sizeof t->card.title, "Texture de synthese");
    snprintf(t->card.author, sizeof t->card.author, "Fouille");
    snprintf(t->card.place, sizeof t->card.place, "%s", fe_place_name(place));
    fe_tape_analyse(t, sr);
}

/* ── sources ────────────────────────────────────────────────────────────── */

#define JSON_CAP (1 << 20)

static int download_decode(fe_engine *e, fe_tape *t, const char *url, const char *range) {
    if (!safe_url(url)) return 0;
    const char *args[16]; int a = 0;
    args[a++] = "-sS"; args[a++] = "-L"; args[a++] = "--connect-timeout"; args[a++] = "8";
    args[a++] = "--max-time"; args[a++] = "40"; args[a++] = "-A"; args[a++] = "Fouille/0.3";
    if (range) { args[a++] = "-r"; args[a++] = range; }
    args[a++] = url; args[a] = NULL;
    int n = run_curl(e, args, e->net_buf, e->net_cap);
    if (n < 4096) return 0;
    int rate = 0;
    int f = decode_mp3(e, e->net_buf, n, &rate);
    return fill_tape(e, t, f, rate);
}

static int api_get(fe_engine *e, const char *url) {
    if (!safe_url(url)) return 0;
    const char *args[] = { "-sS", "-L", "--connect-timeout", "8", "--max-time", "15", "-A", "Fouille/0.3", url, NULL };
    int n = run_curl(e, args, e->net_buf, e->net_cap - 1);
    if (n < 2) return 0;
    if (n > JSON_CAP) n = JSON_CAP;
    e->net_buf[n] = 0;
    return n;
}

/* Radio Aporee, by position. */
static int src_aporee(fe_engine *e, fe_tape *t, const fe_place *pl, float rkm) {
    char q[256], qe[512], url[900];
    float dlat = rkm / 111.0f;
    /* A latitude band circles the planet (45.5 N is also Lyon and Turin), so
     * narrow it by country text; every built-in place is in Quebec. The
     * lieux.json atlas (step 3) will carry the country per place. */
    snprintf(q, sizeof q, "collection:radio-aporee-maps AND latitude:[%.4f TO %.4f] AND coverage:Canada",
             pl->lat - dlat, pl->lat + dlat);
    urlenc(q, qe, sizeof qe);
    snprintf(url, sizeof url, "https://archive.org/advancedsearch.php?q=%s"
             "&fl%%5B%%5D=identifier&fl%%5B%%5D=latitude&fl%%5B%%5D=longitude&rows=800&output=json", qe);
    if (!api_get(e, url)) return 0;
    atomic_store(&e->net_ok, 1);

    const char *docs = jget(jget((char *)e->net_buf, "response"), "docs");
    char ids[64][96]; float la[64], lo[64]; int n = 0, seen = 0;
    for (const char *d = jfirst(docs); d; d = jnext(d)) {
        float dla = (float)jnum(jget(d, "latitude")), dlo = (float)jnum(jget(d, "longitude"));
        if (km_between(pl->lat, pl->lon, dla, dlo) > rkm) continue;
        seen++;
        int slot = n < 64 ? n++ : (int)(fe_rand(&e->wrng) % (uint32_t)seen);   /* reservoir */
        if (slot >= 64) continue;
        if (jstr(jget(d, "identifier"), ids[slot], sizeof ids[slot]) <= 0) { if (slot == n - 1) n--; continue; }
        la[slot] = dla; lo[slot] = dlo;
    }
    if (n == 0) return 0;
    int pick = (int)(fe_rand(&e->wrng) % (uint32_t)n);

    snprintf(url, sizeof url, "https://archive.org/metadata/%s", ids[pick]);
    if (!api_get(e, url)) return 0;
    const char *root = (char *)e->net_buf, *md = jget(root, "metadata");
    fe_card c; memset(&c, 0, sizeof c);
    c.kind = FE_SRC_APOREE; c.lat = la[pick]; c.lon = lo[pick];
    jstr(jget(md, "title"), c.title, sizeof c.title);
    jstr(jget(md, "creator"), c.author, sizeof c.author);
    char lic[160] = ""; jstr(jget(md, "licenseurl"), lic, sizeof lic); short_license(lic, c.license, sizeof c.license);
    snprintf(c.url, sizeof c.url, "https://archive.org/details/%s", ids[pick]);

    char fname[256] = ""; long size = 0;
    for (const char *f = jfirst(jget(root, "files")); f; f = jnext(f)) {
        char nm[256];
        if (jstr(jget(f, "name"), nm, sizeof nm) <= 4) continue;
        size_t L = strlen(nm);
        if (strcasecmp(nm + L - 4, ".mp3")) continue;
        snprintf(fname, sizeof fname, "%s", nm);
        size = (long)jnum(jget(f, "size"));
        break;
    }
    if (!fname[0]) return 0;
    char fe_[600]; urlenc(fname, fe_, sizeof fe_);
    snprintf(url, sizeof url, "https://archive.org/download/%s/%s", ids[pick], fe_);

    long want = 900000;                              /* ~30 s at 192 kbps; the Move's wifi gives ~80 KB/s */
    char range[48];
    long start = 0;
    if (size > want) start = (long)(fe_randf(&e->wrng) * (float)(size - want));   /* somewhere inside the walk */
    snprintf(range, sizeof range, "%ld-%ld", start, start + want - 1);
    set_msg(e, "ecoute %s", pl->name);
    if (!download_decode(e, t, url, range)) return 0;
    t->card = c;
    return 1;
}

/* Freesound, by position. */
static int src_freesound(fe_engine *e, fe_tape *t, const fe_place *pl, float rkm) {
    if (!e->key[0]) return 0;
    char f[160], fe_[400], url[1000];
    snprintf(f, sizeof f, "{!geofilt sfield=geotag pt=%.5f,%.5f d=%.2f}", pl->lat, pl->lon, rkm);
    urlenc(f, fe_, sizeof fe_);
    int page = 1;
    for (int pass = 0; pass < 2; pass++) {
        snprintf(url, sizeof url, "https://freesound.org/apiv2/search/text/?query=&filter=%s"
                 "&fields=id,name,username,license,previews,duration,geotag&page_size=40&page=%d&token=%s",
                 fe_, page, e->key);
        if (!api_get(e, url)) return 0;
        atomic_store(&e->net_ok, 1);
        int count = (int)jnum(jget((char *)e->net_buf, "count"));
        int pages = (count + 39) / 40;
        if (pass == 0 && pages > 1) { page = 1 + (int)(fe_rand(&e->wrng) % (uint32_t)(pages > 25 ? 25 : pages)); if (page > 1) continue; }
        break;
    }
    const char *res = jget((char *)e->net_buf, "results");
    const char *cand[40]; int n = 0;
    for (const char *r = jfirst(res); r && n < 40; r = jnext(r))
        if (jnum(jget(r, "duration")) >= 3.0) cand[n++] = r;
    if (n == 0) return 0;
    const char *r = cand[fe_rand(&e->wrng) % (uint32_t)n];

    fe_card c; memset(&c, 0, sizeof c);
    c.kind = FE_SRC_FREESOUND;
    jstr(jget(r, "name"), c.title, sizeof c.title);
    jstr(jget(r, "username"), c.author, sizeof c.author);
    char lic[160] = ""; jstr(jget(r, "license"), lic, sizeof lic); short_license(lic, c.license, sizeof c.license);
    char id[24] = ""; jstr(jget(r, "id"), id, sizeof id);
    snprintf(c.url, sizeof c.url, "https://freesound.org/s/%s/", id);
    char geo[64] = ""; if (jstr(jget(r, "geotag"), geo, sizeof geo) > 0) sscanf(geo, "%f %f", &c.lat, &c.lon);
    char prev[300] = "";
    jstr(jget(jget(r, "previews"), "preview-hq-mp3"), prev, sizeof prev);
    if (!prev[0]) return 0;
    set_msg(e, "ecoute %s", pl->name);
    if (!download_decode(e, t, prev, NULL)) return 0;
    t->card = c;
    return 1;
}

static int harvest(fe_engine *e, const fe_request *rq, fe_tape *t) {
    const fe_place *pl = &FE_PLACES[rq->place < 0 || rq->place >= FE_NPLACES ? 0 : rq->place];
    float rkm = pl->radius_km * rq->radius_scale;
    int order[2];
    switch (rq->source) {
    case 1:  order[0] = 0; order[1] = -1; break;            /* Aporee only */
    case 2:  order[0] = 1; order[1] = -1; break;            /* Freesound only */
    default: order[0] = (int)(fe_rand(&e->wrng) & 1); order[1] = !order[0]; break;
    }
    for (int widen = 0; widen < 2; widen++) {               /* nothing within the radius: look a bit further */
        float r = rkm * (widen ? 3.0f : 1.0f);
        for (int k = 0; k < 2; k++) {
            if (order[k] < 0 || !atomic_load(&e->running)) continue;
            if (order[k] == 0 ? src_aporee(e, t, pl, r) : src_freesound(e, t, pl, r)) {
                snprintf(t->card.place, sizeof t->card.place, "%s", pl->name);
                return 1;
            }
        }
    }
    return 0;
}

/* ── the worker ─────────────────────────────────────────────────────────── */

static void *worker(void *arg) {
    fe_engine *e = arg;
    int rr = 0;
    double last = 0;
    while (atomic_load(&e->running)) {
        if (atomic_load_explicit(&e->pending, memory_order_acquire) != -1) { msleep(20); continue; }
        int vi = -1;
        for (int k = 0; k < FE_NV; k++) {
            int c = (rr + k) % FE_NV;
            if (atomic_load_explicit(&e->voices[c].want, memory_order_acquire)) { vi = c; break; }
        }
        if (vi < 0) { msleep(40); continue; }
        rr = (vi + 1) % FE_NV;
        fe_voice *v = &e->voices[vi];
        atomic_store(&v->searching, 1);
        fe_request rq = v->req;
        atomic_store(&v->want, 0);

        double wait = 1.2 - (now_s() - last);                /* stay well under Freesound's 60/min */
        if (wait > 0 && !e->cfg.offline) msleep((int)(wait * 1000));
        last = now_s();

        fe_tape *t = e->spare;                               /* ours while pending == -1 */
        int ok = 0;
        if (!e->cfg.offline && e->curl[0]) {
            set_msg(e, "fouille %s", FE_PLACES[rq.place].name);
            ok = harvest(e, &rq, t);
            if (ok) { atomic_fetch_add(&e->harvests, 1); set_msg(e, "en ligne", NULL); }
            else {
                atomic_fetch_add(&e->failures, 1);
                set_msg(e, atomic_load(&e->net_ok) ? "rien trouve: %s" : "hors ligne", FE_PLACES[rq.place].name);
            }
        }
        if (!ok && (e->cfg.offline || v->tape->frames < 64)) { /* an empty voice still gets something to play */
            fe_synth_fill(t, e->sr, &e->wrng, rq.place);
            ok = 1;
        }
        if (ok && atomic_load(&e->running)) {
            fe_tape_analyse(t, e->sr);
            atomic_store_explicit(&e->pending, vi, memory_order_release);
        }
        atomic_store(&v->searching, 0);
    }
    return NULL;
}

static void read_key(fe_engine *e) {
    for (int i = 0; i < 4; i++) {
        const char *p = e->cfg.key_paths[i];
        if (!p) continue;
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char b[128] = "";
        if (fgets(b, sizeof b, f)) {
            int n = 0;
            for (char *c = b; *c && n < (int)sizeof e->key - 1; c++) if (isalnum((unsigned char)*c)) e->key[n++] = *c;
            e->key[n] = 0;
        }
        fclose(f);
        if (e->key[0]) return;
    }
}

int fe_harvest_start(fe_engine *e) {
    e->dec_cap = (int)(e->cfg.voice_seconds * 48000.0f * 1.2f) + 4096;
    e->dec = malloc(sizeof(float) * 2 * (size_t)e->dec_cap);
    e->net_cap = 1600000;
    e->net_buf = malloc((size_t)e->net_cap + 1);
    if (!e->dec || !e->net_buf) return -1;
    e->curl[0] = 0;
    if (e->cfg.curl_path && access(e->cfg.curl_path, X_OK) == 0) snprintf(e->curl, sizeof e->curl, "%s", e->cfg.curl_path);
    for (int i = 0; !e->curl[0] && CURLS[i]; i++) if (access(CURLS[i], X_OK) == 0) snprintf(e->curl, sizeof e->curl, "%s", CURLS[i]);
    read_key(e);
    set_msg(e, e->cfg.offline ? "hors ligne (test)" : (e->curl[0] ? "demarrage" : "pas de curl"), NULL);
    atomic_store(&e->running, 1);
    if (pthread_create(&e->th, NULL, worker, e) != 0) return -1;
    e->th_started = 1;
    return 0;
}

void fe_harvest_stop(fe_engine *e) {
    if (!e->th_started) return;
    atomic_store(&e->running, 0);
    int pid = atomic_load(&e->child_pid);
    if (pid > 0) kill((pid_t)pid, SIGKILL);              /* a stuck curl must never hold the exit */
    pthread_join(e->th, NULL);
    e->th_started = 0;
}
