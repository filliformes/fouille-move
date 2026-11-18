/*
 * Fouille engine — sessions (64 slots), after Loopex's session worker.
 *
 * Layout: <session_dir>/sessionNN/
 *   state.txt      the settings text the host gave (get_param("state"))
 *   meta.txt       one line per sound: frames, then the fiche (tab separated)
 *   name.txt       NN_YYYYMMDD_HHMM
 *   sNN_sonMM.wav  the sound's fragment, 16-bit stereo at the engine rate
 *
 * Everything here runs on the worker thread, between harvests. Saving reads the tapes
 * the audio thread plays (only pointers move while we read: memory stays valid, and the
 * worker is the one that fills the spare, so nothing is rewritten under us). Loading
 * writes into the spare, which is ours while pending == -1, and hands each fragment over
 * the same way a harvest does (pend_load = play it, never keep it as a Futur).
 */
#define _POSIX_C_SOURCE 200809L
#include "fe_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static void ms_sleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

int fe_session(fe_engine *e, int op, int slot, const char *state) {
    if (!e->cfg.session_dir || !e->ss_state) return 0;
    if (atomic_load(&e->ss_req)) return 0;
    int st = atomic_load(&e->ss_status);
    if (st == FE_SST_SAVING || st == FE_SST_LOADING) return 0;
    if (slot < 1) slot = 1;
    if (slot > FE_NSLOTS) slot = FE_NSLOTS;
    if (op == FE_SS_SAVE) snprintf(e->ss_state, (size_t)e->ss_state_cap, "%s", state ? state : "");
    atomic_store(&e->ss_state_ready, 0);
    atomic_store(&e->ss_slot, slot);
    atomic_store(&e->ss_status, op == FE_SS_SAVE ? FE_SST_SAVING : op == FE_SS_LOAD ? FE_SST_LOADING : FE_SST_IDLE);
    atomic_store(&e->ss_req, op);
    return 1;
}
int fe_session_status(const fe_engine *e) { return atomic_load(&((fe_engine *)e)->ss_status); }
int fe_session_names(const fe_engine *e, char *buf, int len) {
    int p = 0;
    for (int n = 1; n <= FE_NSLOTS && p < len - 2; n++)
        p += snprintf(buf + p, (size_t)(len - p), "%s%s", e->ss_names[n], n < FE_NSLOTS ? ";" : "");
    return p;
}
const char *fe_session_take_state(fe_engine *e) {
    if (!atomic_load(&e->ss_state_ready)) return NULL;
    atomic_store(&e->ss_state_ready, 0);
    return e->ss_state;
}

/* ---- WAV (from Loopex: atomic write through .tmp + fsync + rename) ---- */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int wav_write(const char *path, const int16_t *pcm, int len, uint32_t sr) {
    char tmp[400]; if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    FILE *g = fopen(tmp, "wb"); if (!g) return 0;
    uint8_t h[44]; uint32_t data = (uint32_t)len * 4u;
    memcpy(h, "RIFF", 4); put32(h + 4, 36u + data); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 2);
    put32(h + 24, sr); put32(h + 28, sr * 4u); put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
    int ok = (fwrite(h, 1, 44, g) == 44);
    if (ok && len > 0) ok = (fwrite(pcm, sizeof(int16_t), (size_t)len * 2, g) == (size_t)len * 2);
    if (ok) ok = (fflush(g) == 0 && fsync(fileno(g)) == 0);
    if (fclose(g) != 0) ok = 0;
    if (ok) ok = (rename(tmp, path) == 0);
    if (!ok) remove(tmp);
    return ok;
}
/* 16-bit PCM, mono or stereo, into interleaved stereo. Returns frames read, 0 = not usable. */
static int wav_read(const char *path, int16_t *pcm, int maxlen) {
    FILE *g = fopen(path, "rb"); if (!g) return 0;
    uint8_t h[12]; if (fread(h, 1, 12, g) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(g); return 0; }
    int ch = 0, bits = 0, haveFmt = 0; uint32_t dataLen = 0;
    for (;;) { uint8_t c[8]; if (fread(c, 1, 8, g) != 8) break; uint32_t sz = get32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t f[16];
            if (sz < 16 || fread(f, 1, 16, g) != 16) break;
            if (get16(f) != 1) break;
            ch = get16(f + 2); bits = get16(f + 14); haveFmt = 1;
            if (sz > 16) fseek(g, (long)(sz - 16 + (sz & 1)), SEEK_CUR); }
        else if (!memcmp(c, "data", 4)) { dataLen = sz; break; }
        else fseek(g, (long)(sz + (sz & 1)), SEEK_CUR); }
    if (!haveFmt || bits != 16 || (ch != 1 && ch != 2) || dataLen == 0) { fclose(g); return 0; }
    int frames = (int)(dataLen / (uint32_t)(2 * ch)); if (frames > maxlen) frames = maxlen;
    int got = 0;
    if (ch == 2) got = (int)(fread(pcm, 4, (size_t)frames, g));
    else {
        int16_t buf[4096];
        while (got < frames) { int n = frames - got; if (n > 4096) n = 4096;
            int r = (int)fread(buf, 2, (size_t)n, g); if (r <= 0) break;
            for (int j = 0; j < r; j++) { pcm[2 * (got + j)] = buf[j]; pcm[2 * (got + j) + 1] = buf[j]; }
            got += r; if (r < n) break; }
    }
    fclose(g); return got;
}

/* the fiche on one line: tabs and newlines in fields become spaces */
static void clean(char *d, const char *s, size_t n) {
    size_t i = 0; for (; s[i] && i < n - 1; i++) d[i] = (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') ? ' ' : s[i]; d[i] = 0;
}
static char *field(char **p) {                       /* next tab-separated field, in place */
    char *s = *p; if (!s) return (char *)"";
    char *t = strchr(s, '\t');
    if (t) { *t = 0; *p = t + 1; } else { char *nl = strpbrk(s, "\r\n"); if (nl) *nl = 0; *p = NULL; }
    return s;
}

void fe_session_scan(fe_engine *e) {
    char path[400];
    for (int n = 1; n <= FE_NSLOTS; n++) {
        e->ss_names[n][0] = 0;
        snprintf(path, sizeof path, "%s/session%02d/name.txt", e->cfg.session_dir, n);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(e->ss_names[n], (int)sizeof e->ss_names[n], f)) {
            size_t L = strlen(e->ss_names[n]);
            while (L && (e->ss_names[n][L - 1] == '\n' || e->ss_names[n][L - 1] == '\r' || e->ss_names[n][L - 1] == ';')) e->ss_names[n][--L] = 0;
        }
        fclose(f);
    }
}

static void mkdirs(const char *dir) {                /* the base and its parents, best effort */
    char b[300]; snprintf(b, sizeof b, "%s", dir);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(b, 0777); *p = '/'; }
    mkdir(b, 0777);
}

static void do_save(fe_engine *e, int slot, const char *dir) {
    char path[400];
    mkdirs(e->cfg.session_dir); mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/state.txt", dir);
    FILE *f = fopen(path, "w"); if (f) { fputs(e->ss_state, f); fclose(f); }
    snprintf(path, sizeof path, "%s/meta.txt", dir);
    FILE *m = fopen(path, "w");
    for (int i = 0; i < FE_NV; i++) {
        fe_tape *t = e->voices[i].tape;                  /* a pointer read: it may move, its memory stays */
        int len = t->frames; if (len > t->cap) len = t->cap;
        fe_card c = t->card;
        char wp[400]; snprintf(wp, sizeof wp, "%s/s%02d_son%02d.wav", dir, slot, i + 1);
        if (len < 64 || !wav_write(wp, t->pcm, len, (uint32_t)(e->sr + 0.5f))) { remove(wp); len = 0; }
        if (m) {
            char ti[112], au[64], li[24], ur[160], pl[32];
            clean(ti, c.title, sizeof ti); clean(au, c.author, sizeof au); clean(li, c.license, sizeof li);
            clean(ur, c.url, sizeof ur); clean(pl, c.place, sizeof pl);
            fprintf(m, "%d\t%d\t%s\t%s\t%s\t%s\t%s\t%.5f\t%.5f\n", len, c.kind, ti, au, li, ur, pl, c.lat, c.lon);
        }
    }
    if (m) fclose(m);
    time_t tt = time(NULL); struct tm tmv; localtime_r(&tt, &tmv); char nm[40];
    snprintf(nm, sizeof nm, "%02d_%04d%02d%02d_%02d%02d", slot, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    snprintf(path, sizeof path, "%s/name.txt", dir);
    FILE *nf = fopen(path, "w"); if (nf) { fputs(nm, nf); fclose(nf); }
    fe_session_scan(e);
    atomic_store(&e->ss_status, FE_SST_OK);
}

static void do_load(fe_engine *e, int slot, const char *dir) {
    char path[400], line[600];
    snprintf(path, sizeof path, "%s/meta.txt", dir);
    FILE *m = fopen(path, "r");
    if (!m) { atomic_store(&e->ss_status, FE_SST_EMPTY); return; }
    for (int i = 0; i < FE_NV && atomic_load(&e->running); i++) {
        if (!fgets(line, sizeof line, m)) break;
        char *p = line;
        int len = atoi(field(&p));
        if (len < 64) continue;
        fe_tape *t = e->spare;                           /* ours while pending == -1 */
        char wp[400]; snprintf(wp, sizeof wp, "%s/s%02d_son%02d.wav", dir, slot, i + 1);
        int n = wav_read(wp, t->pcm, t->cap);
        if (n < 64) continue;
        t->frames = n;
        memset(&t->card, 0, sizeof t->card);
        t->card.kind = atoi(field(&p));
        snprintf(t->card.title, sizeof t->card.title, "%s", field(&p));
        snprintf(t->card.author, sizeof t->card.author, "%s", field(&p));
        snprintf(t->card.license, sizeof t->card.license, "%s", field(&p));
        snprintf(t->card.url, sizeof t->card.url, "%s", field(&p));
        snprintf(t->card.place, sizeof t->card.place, "%s", field(&p));
        t->card.lat = strtof(field(&p), NULL);
        t->card.lon = strtof(field(&p), NULL);
        fe_tape_analyse(t, e->sr);
        atomic_store(&e->voices[i].want, 0);            /* no harvest replaces what we bring back */
        atomic_store_explicit(&e->pend_load, 1, memory_order_relaxed);
        atomic_store_explicit(&e->pending, i, memory_order_release);
        for (int w = 0; w < 400 && atomic_load_explicit(&e->pending, memory_order_acquire) != -1 && atomic_load(&e->running); w++) ms_sleep(5);
        if (atomic_load(&e->pending) != -1) break;      /* the audio thread is not running: stop here */
    }
    fclose(m);
    snprintf(path, sizeof path, "%s/state.txt", dir);
    FILE *f = fopen(path, "r");
    if (f) { size_t n = fread(e->ss_state, 1, (size_t)e->ss_state_cap - 1, f); e->ss_state[n] = 0; fclose(f);
             atomic_store(&e->ss_state_ready, 1); }
    fe_session_scan(e);
    atomic_store(&e->ss_status, FE_SST_OK);
}

static void do_delete(fe_engine *e, int slot, const char *dir) {
    char path[400];
    snprintf(path, sizeof path, "%s/state.txt", dir); remove(path);
    snprintf(path, sizeof path, "%s/meta.txt", dir);  remove(path);
    snprintf(path, sizeof path, "%s/name.txt", dir);  remove(path);
    for (int i = 0; i < FE_NV; i++) { snprintf(path, sizeof path, "%s/s%02d_son%02d.wav", dir, slot, i + 1); remove(path); }
    rmdir(dir);
    fe_session_scan(e);
    atomic_store(&e->ss_status, FE_SST_OK);
}

/* Worker: called between harvests, with pending == -1. */
void fe_session_poll(fe_engine *e) {
    int req = atomic_exchange(&e->ss_req, 0);
    if (!req || !e->cfg.session_dir) return;
    int slot = atomic_load(&e->ss_slot);
    char dir[320]; snprintf(dir, sizeof dir, "%s/session%02d", e->cfg.session_dir, slot);
    if (req == FE_SS_SAVE) do_save(e, slot, dir);
    else if (req == FE_SS_LOAD) do_load(e, slot, dir);
    else if (req == FE_SS_DELETE) do_delete(e, slot, dir);
    else { fe_session_scan(e); atomic_store(&e->ss_status, FE_SST_IDLE); }
}
