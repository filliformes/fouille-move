/*
 * Fouille engine — offline tests. No network: synthetic fragments.
 * Checks RELATIONS (silence before a gate, sound after, a release decays to
 * silence, a swap under a playing voice dips instead of clicking, the engine
 * runs at 48 kHz too) and writes tests/out/scene.wav to listen to.
 *
 *   cc -O2 -std=c11 -Iengine tests/render_test.c engine/fe_*.c -lm -lpthread -o render_test
 */
#define _POSIX_C_SOURCE 200809L
#include "fe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static void msleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

typedef struct { double sum; float peak; int n, nan; } stats;
static stats run(fe_engine *e, float sr, float seconds, int block, FILE *wav) {
    stats s = {0};
    float l[512], r[512];
    int total = (int)(seconds * sr);
    for (int done = 0; done < total; done += block) {
        int n = total - done < block ? total - done : block;
        fe_render(e, l, r, n);
        for (int i = 0; i < n; i++) {
            if (!isfinite(l[i]) || !isfinite(r[i])) s.nan++;
            float a = fmaxf(fabsf(l[i]), fabsf(r[i]));
            if (a > s.peak) s.peak = a;
            s.sum += (double)l[i] * l[i] + (double)r[i] * r[i];
            if (wav) { int16_t o[2] = { (int16_t)(l[i] * 32767), (int16_t)(r[i] * 32767) }; fwrite(o, 2, 2, wav); }
        }
        s.n += n;
    }
    return s;
}
static float rms(stats s) { return s.n ? (float)sqrt(s.sum / (2.0 * s.n)) : 0; }

static fe_engine *make(float sr) {
    fe_config c = {0};
    c.sample_rate = sr; c.max_block = 128; c.voice_seconds = 30; c.offline = 1; c.seed = 42;
    fe_engine *e = fe_create(&c);
    if (!e) { printf("fe_create failed\n"); exit(1); }
    float l[128], r[128];
    for (int t = 0; t < 400; t++) {                   /* let the worker fill the voices */
        int ready = 0;
        for (int v = 0; v < FE_NV; v++) ready += fe_voice_state(e, v) >= FE_VS_READY;
        if (ready == FE_NV) break;
        fe_render(e, l, r, 128);
        msleep(5);
    }
    return e;
}

static void wav_header(FILE *f, int sr, int frames) {
    int data = frames * 4;
    fwrite("RIFF", 1, 4, f); int v = 36 + data; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); short s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);
    fwrite(&sr, 4, 1, f); v = sr * 4; fwrite(&v, 4, 1, f); s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}


/* render into a mono buffer (L+R)/2, for comparisons between engines */
static void render_to(fe_engine *e, float *buf, int frames) {
    float l[128], r[128];
    for (int done = 0; done < frames; done += 128) {
        int n = frames - done < 128 ? frames - done : 128;
        fe_render(e, l, r, n);
        for (int i = 0; i < n; i++) buf[done + i] = (l[i] + r[i]) * 0.5f;
    }
}
static float buf_rms(const float *b, int n) { double s = 0; for (int i = 0; i < n; i++) s += (double)b[i] * b[i]; return n ? (float)sqrt(s / n) : 0; }
static float diff_rms(const float *a, const float *b, int n) { double s = 0; for (int i = 0; i < n; i++) { double d = a[i] - b[i]; s += d * d; } return n ? (float)sqrt(s / n) : 0; }
static float bright(const float *b, int n) {           /* first-difference rms over rms: a brightness proxy */
    double d = 0, x = 0; for (int i = 1; i < n; i++) { d += (double)(b[i] - b[i - 1]) * (b[i] - b[i - 1]); x += (double)b[i] * b[i]; }
    return x > 0 ? (float)sqrt(d / x) : 0;
}
static void wait_card(fe_engine *e, int v, const char *needle, int want, float sr) {
    for (int t = 0; t < 600; t++) {                      /* up to ~6 s of real time for the worker */
        fe_card c; fe_voice_card(e, v, &c);
        if ((strstr(c.place, needle) != NULL) == want) return;
        run(e, sr, 0.01f, 128, NULL); msleep(5);
    }
}

static void gesture_tests(float SR) {
    printf("gestures\n");
    const int N = (int)(1.0f * SR);
    float *a = calloc((size_t)N, sizeof(float)), *b = calloc((size_t)N, sizeof(float)), *c = calloc((size_t)N, sizeof(float));

    /* left pad: a sound opens exactly at Depart, even with Dispersion up */
    fe_engine *e = make(SR);
    fe_set(e, 10, FE_P_START, 0.5f); fe_set(e, 10, FE_P_SCATTER, 1.0f);
    fe_gate(e, 10, 1, 0.8f); run(e, SR, 0.01f, 128, NULL);
    float p0 = fe_voice_pos(e, 10);
    CHECK(fabsf(p0 - 0.5f) < 0.01f, "a pad opens the sound at Depart (pos %.3f, Depart 0.5)", p0);
    run(e, SR, 2.0f, 128, NULL);
    fe_gate(e, 10, 0, 0); run(e, SR, 0.03f, 128, NULL);          /* closing: still in its tail */
    fe_gate(e, 10, 1, 0.8f); run(e, SR, 0.05f, 128, NULL);
    float p1 = fe_voice_pos(e, 10);
    CHECK(fabsf(p1 - 0.5f) < 0.01f, "reopened in its tail, it starts again at Depart (pos %.3f)", p1);
    stats x = run(e, SR, 0.5f, 128, NULL);
    CHECK(rms(x) > 0.002f && x.nan == 0, "and it sounds (rms %.4f)", rms(x));
    fe_gate(e, 10, 0, 0); run(e, SR, 4.0f, 128, NULL);

    /* settings round-trip and reset */
    fe_gesture_param(e, FE_X_GEL, 2, 0.9f); run(e, SR, 0.01f, 128, NULL);
    CHECK(fabsf(fe_gesture_param_get(e, FE_X_GEL, 2) - 0.9f) < 1e-6f, "a gesture setting is applied");
    fe_gesture_reset(e, FE_X_GEL); run(e, SR, 0.01f, 128, NULL);
    CHECK(fe_gesture_param_get(e, FE_X_GEL, 2) == fe_gesture_param_default(FE_X_GEL, 2), "reset brings the defaults back");

    /* Bongo: a hit is louder than the voice was, then the voice sits low until release */
    fe_set(e, 3, FE_P_HOLD, 0); fe_set(e, 3, FE_P_LENGTH, 1.0f);
    fe_gate(e, 3, 1, 0.6f); run(e, SR, 1.0f, 128, NULL);
    stats steady = run(e, SR, 0.5f, 128, NULL);
    fe_gesture(e, -1, FE_X_BONGO, 1.0f);
    stats hit = run(e, SR, 0.08f, 128, NULL);
    run(e, SR, 2.0f, 128, NULL);
    stats low = run(e, SR, 0.5f, 128, NULL);
    CHECK(hit.peak > steady.peak * 1.15f, "Bongo strikes (hit peak %.3f > steady %.3f)", hit.peak, steady.peak);
    CHECK(rms(low) < rms(steady) * 0.7f, "held, the voice sits low in its LPG (%.4f < %.4f)", rms(low), rms(steady));
    fe_gesture(e, -1, FE_X_BONGO, 0); run(e, SR, 0.5f, 128, NULL);
    stats back = run(e, SR, 0.5f, 128, NULL);
    CHECK(rms(back) > rms(steady) * 0.6f, "released, it comes back (%.4f)", rms(back));
    fe_gate(e, 3, 0, 0); run(e, SR, 4.0f, 128, NULL);

    /* Gel: the head stops, the sound keeps sounding */
    fe_set(e, 5, FE_P_SPEED, 0.5f);
    fe_gate(e, 5, 1, 0.8f); run(e, SR, 0.5f, 128, NULL);
    fe_gesture(e, 5, FE_X_GEL, 0.6f); run(e, SR, 0.6f, 128, NULL);  /* default fade in ~35 ms */
    float g0 = fe_voice_pos(e, 5);
    x = run(e, SR, 1.0f, 128, NULL);
    float g1 = fe_voice_pos(e, 5);
    fe_card c5; fe_voice_card(e, 5, &c5);
    CHECK(fabsf(g1 - g0) * c5.seconds < 0.02f, "Gel holds the head (moved %.3f s in 1 s)", fabsf(g1 - g0) * c5.seconds);
    CHECK(rms(x) > 0.002f && x.nan == 0, "Gel keeps sounding (rms %.4f)", rms(x));
    fe_gesture(e, 5, FE_X_GEL, 0); run(e, SR, 1.5f, 128, NULL);
    float g2 = fe_voice_pos(e, 5); run(e, SR, 0.5f, 128, NULL);
    CHECK(fabsf(fe_voice_pos(e, 5) - g2) * c5.seconds > 0.3f, "released, the head runs again");
    CHECK(fe_gesture_get(e, 5, FE_X_GEL) == 0 && fe_gesture_get(e, 6, FE_X_GEL) == 0, "a targeted gesture stays on its voice");
    fe_gate(e, 5, 0, 0); run(e, SR, 4.0f, 128, NULL);

    /* Accroche: the colour narrows around one frequency, and stays bounded */
    fe_gate(e, 7, 1, 1.0f); run(e, SR, 0.5f, 128, NULL);
    render_to(e, a, N);
    fe_gesture(e, -1, FE_X_ACCROCHE, 1.0f); run(e, SR, 0.3f, 128, NULL);
    render_to(e, b, N);
    x = run(e, SR, 3.0f, 128, NULL);
    CHECK(fabsf(bright(b, N) - bright(a, N)) > 0.15f * bright(a, N), "Accroche changes the colour (%.3f -> %.3f)", bright(a, N), bright(b, N));
    CHECK(buf_rms(b, N) > 0.001f && x.nan == 0 && x.peak <= 1, "Accroche sounds, bounded (rms %.4f, peak %.3f)", buf_rms(b, N), x.peak);
    fe_gesture(e, -1, FE_X_ACCROCHE, 0); fe_gate(e, 7, 0, 0); run(e, SR, 4.0f, 128, NULL);

    /* Ressac: the output returns after the voices stop, then fades; never runs away */
    fe_gate(e, 8, 1, 1.0f); fe_gate(e, 9, 1, 1.0f);
    fe_gesture(e, -1, FE_X_RESSAC, 1.0f);
    x = run(e, SR, 12.0f, 128, NULL);
    CHECK(x.nan == 0 && x.peak <= 1.0f, "Ressac at full stays bounded over 12 s (peak %.3f)", x.peak);
    fe_gate(e, 8, 0, 0); fe_gate(e, 9, 0, 0);
    run(e, SR, 0.4f, 128, NULL);
    stats tail = run(e, SR, 0.6f, 128, NULL);
    fe_gesture(e, -1, FE_X_RESSAC, 0);
    run(e, SR, 12.0f, 128, NULL);
    stats gone = run(e, SR, 0.5f, 128, NULL);
    CHECK(rms(tail) > 0.002f, "the wave comes back after the voices stop (rms %.4f)", rms(tail));
    CHECK(gone.peak < 2e-3f, "released, it dies away (peak %.5f)", gone.peak);
    fe_destroy(e);

    /* Contagion: a voice borrows from a neighbour on the map, not from a far one */
    for (int k = 0; k < 3; k++) {                          /* 0: no contagion, 1: near (Mile End), 2: far (Kamouraska) */
        fe_engine *q = make(SR);
        fe_set(q, 1, FE_P_PLACE, (k == 2 ? 19.0f : 1.0f) / 19.0f);
        for (int t = 0; t < 400 && !(fe_voice_state(q, 1) >= FE_VS_READY && !fe_voice_busy(q, 1)); t++) { run(q, SR, 0.01f, 128, NULL); msleep(5); }
        fe_set(q, 1, FE_P_VOLUME, 0.0f);                  /* its matter, not its sound */
        fe_gesture_param(q, FE_X_CONTAGION, 2, 0.0f);
        fe_gate(q, 0, 1, 0.8f); fe_gate(q, 1, 1, 0.8f);
        run(q, SR, 0.5f, 128, NULL);
        if (k) fe_gesture(q, 0, FE_X_CONTAGION, 1.0f);
        run(q, SR, 0.2f, 128, NULL);
        render_to(q, k == 0 ? a : k == 1 ? b : c, N);
        fe_destroy(q);
    }
    float near = diff_rms(a, b, N), far = diff_rms(a, c, N);
    CHECK(near > 0.2f * buf_rms(a, N), "Contagion: the Plateau takes from Mile End (diff %.4f vs %.4f)", near, buf_rms(a, N));
    CHECK(far < near * 0.2f, "and nearly nothing from Kamouraska (diff %.5f)", far);

    /* Derive geosonique: the sound goes to listen nearby, then comes home */
    e = make(SR);
    fe_card h0; fe_voice_card(e, 2, &h0);
    fe_gate(e, 2, 1, 0.8f); run(e, SR, 0.2f, 128, NULL);
    fe_gesture(e, -1, FE_X_DERIVE, 0.5f);
    wait_card(e, 2, "km", 1, SR);
    fe_card h1; fe_voice_card(e, 2, &h1);
    CHECK(strstr(h1.place, "km") != NULL, "Derive: voice 3 listens elsewhere ('%s' -> '%s')", h0.place, h1.place);
    fe_card f3; fe_voice_card(e, 4, &f3);
    CHECK(strstr(f3.place, "km") == NULL, "a closed voice does not drift ('%s')", f3.place);
    fe_gesture(e, -1, FE_X_DERIVE, 0);
    wait_card(e, 2, "km", 0, SR);
    fe_card h2; fe_voice_card(e, 2, &h2);
    CHECK(strcmp(h2.place, h0.place) == 0, "released, it comes home ('%s')", h2.place);
    fe_freeze(e, 2, 1); fe_gesture(e, -1, FE_X_DERIVE, 0.5f);
    for (int t = 0; t < 200; t++) { run(e, SR, 0.01f, 128, NULL); msleep(5); }
    fe_card h3; fe_voice_card(e, 2, &h3);
    CHECK(strcmp(h3.place, h0.place) == 0, "a frozen voice stays home ('%s')", h3.place);
    fe_destroy(e);

    free(a); free(b); free(c);
}

int main(void) {
    const float SR = 44100;
    printf("fouille engine tests\n");

    fe_engine *e = make(SR);
    int ready = 0;
    for (int v = 0; v < FE_NV; v++) ready += fe_voice_state(e, v) == FE_VS_READY;
    CHECK(ready == FE_NV, "16 voices ready offline (%d)", ready);
    fe_card c; fe_voice_card(e, 0, &c);
    CHECK(c.kind == 3 && c.seconds > 5, "voice 0 has a synthetic fragment with a card (%.1f s, '%s')", c.seconds, c.place);

    stats s = run(e, SR, 0.5f, 128, NULL);
    CHECK(s.peak == 0, "silence before any gate (peak %g)", s.peak);

    fe_gate(e, 0, 1, 1.0f);
    s = run(e, SR, 2.0f, 128, NULL);
    CHECK(rms(s) > 0.003f && s.nan == 0, "a gate makes sound (rms %.4f, nan %d)", rms(s), s.nan);
    CHECK(s.peak <= 1.0f, "output stays in range (peak %.3f)", s.peak);
    CHECK(fe_voice_state(e, 0) == FE_VS_PLAYING, "voice 0 is playing");

    /* strike: a hard hit is louder than a soft one */
    fe_engine *e2 = make(SR);
    fe_gate(e2, 3, 1, 0.15f); stats soft = run(e2, SR, 1.0f, 128, NULL);
    fe_gate(e2, 3, 0, 0); run(e2, SR, 10.0f, 128, NULL);
    fe_gate(e2, 3, 1, 1.0f); stats hard = run(e2, SR, 1.0f, 128, NULL);
    CHECK(rms(hard) > rms(soft) * 1.2f, "velocity strikes the LPG (soft %.4f < hard %.4f)", rms(soft), rms(hard));
    fe_destroy(e2);

    /* release decays to silence and the voice returns to READY */
    fe_gate(e, 0, 0, 0);
    run(e, SR, 6.0f, 128, NULL);                       /* default fall ~ 145 ms, generous margin */
    s = run(e, SR, 0.2f, 128, NULL);
    CHECK(s.peak < 1e-3f, "release decays to silence (peak %g)", s.peak);
    CHECK(fe_voice_state(e, 0) == FE_VS_READY, "voice 0 back to ready");

    /* swap under a playing voice: dips (duck), never jumps above the steady level */
    fe_gate(e, 1, 1, 0.8f);
    stats steady = run(e, SR, 1.5f, 128, NULL);
    fe_card before; fe_voice_card(e, 1, &before);
    fe_refetch(e, 1);
    float minw = 1e9f, maxw = 0;
    for (int w = 0; w < 400; w++) {                    /* 400 x 5 ms windows */
        stats x = run(e, SR, 0.005f, 128, NULL);
        if (rms(x) < minw) minw = rms(x);
        if (x.peak > maxw) maxw = x.peak;
        msleep(1);
    }
    fe_card after; fe_voice_card(e, 1, &after);
    CHECK(fabsf(after.seconds - before.seconds) > 1e-3f || after.centroid_hz != before.centroid_hz,
          "refetch delivered a new fragment (%.2f s -> %.2f s)", before.seconds, after.seconds);
    CHECK(minw < rms(steady) * 0.3f, "the swap ducks (min window rms %.4f vs steady %.4f)", minw, rms(steady));
    CHECK(maxw <= fmaxf(steady.peak * 1.6f, 0.05f), "no burst at the swap (peak %.3f vs steady %.3f)", maxw, steady.peak);
    CHECK(fe_voice_state(e, 1) == FE_VS_PLAYING, "voice 1 still playing after the swap");
    fe_gate(e, 1, 0, 0);

    /* every hold mode and direction runs clean */
    for (int h = 0; h < 3; h++) for (int d = 0; d < 2; d++) {
        fe_set(e, 2, 5 /* HOLD */, h / 2.0f); fe_set(e, 2, 11 /* DIR */, (float)d);
        fe_set(e, 2, 10 /* LENGTH */, 0.2f);
        fe_gate(e, 2, 1, 0.9f);
        stats x = run(e, SR, 1.5f, 128, NULL);
        CHECK(x.nan == 0 && rms(x) > 0.002f && x.peak <= 1, "hold %d dir %d (rms %.4f)", h, d, rms(x));
        fe_gate(e, 2, 0, 0); run(e, SR, 3.0f, 128, NULL);
    }

    /* the scene: 6 voices, all the colours, 14 s */
    if (system("mkdir -p tests/out") != 0) printf("  (mkdir tests/out failed)\n");
    FILE *wav = fopen("tests/out/scene.wav", "wb");
    const float seconds = 14;
    wav_header(wav, (int)SR, (int)(seconds * SR));
    fe_set(e, 4, 5, 0.5f); fe_set(e, 4, 16 /* DRIFT */, 0.8f);
    fe_set(e, 5, 5, 1.0f); fe_set(e, 5, 13 /* GRAIN */, 0.3f); fe_set(e, 5, 21 /* POOL */, 0.6f);
    fe_set(e, 6, 14 /* LOCK */, 0.8f); fe_set(e, 6, 12 /* SPEED */, 0.3f);
    fe_set(e, 7, 15 /* WEAR */, 1.0f); fe_set(e, 7, 10, 0.1f);
    fe_set(e, 8, 18 /* DRIVE */, 0.6f); fe_set(e, 8, 17 /* TONE */, 0.8f);
    int seq[] = { 4, 5, 6, 7, 8, 9 };
    long written = 0;
    for (int k = 0; k < 6; k++) {
        fe_gate(e, seq[k], 1, 0.5f + 0.1f * k);
        stats x = run(e, SR, 1.5f, 128, wav); written += x.n;
    }
    for (int k = 0; k < 6; k++) { fe_gate(e, seq[k], 0, 0); stats x = run(e, SR, 0.5f, 128, wav); written += x.n; }
    stats tail = run(e, SR, seconds - (float)written / SR, 128, wav);
    fclose(wav);
    CHECK(tail.nan == 0, "scene rendered to tests/out/scene.wav");

    /* the pool keeps sounding after the voices stop, then fades */
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    fe_destroy(e);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    CHECK(ms < 500, "destroy joins the worker quickly (%.0f ms)", ms);

    /* portability: 48 kHz, odd block size */
    fe_engine *p = make(48000);
    fe_gate(p, 0, 1, 1);
    s = run(p, 48000, 1.0f, 96, NULL);
    CHECK(rms(s) > 0.003f && s.nan == 0, "48 kHz, 96-frame blocks (rms %.4f)", rms(s));
    fe_destroy(p);

    gesture_tests(SR);

    /* CPU: 16 voices playing, all colours, render time per 128-frame block */
    fe_engine *q = make(SR);
    for (int v = 0; v < FE_NV; v++) { fe_set(q, v, 5, (v % 3) / 2.0f); fe_set(q, v, 14, 0.5f); fe_set(q, v, 15, 0.8f); fe_gate(q, v, 1, 0.8f); }
    run(q, SR, 1.0f, 128, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    run(q, SR, 5.0f, 128, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / (5.0 * SR / 128);
    printf("  info 16 voices: %.1f us per 128-frame block (budget 2902 us)\n", us);
    for (int g = 0; g < FE_X_COUNT; g++) if (fe_gesture_ready(g) && g != FE_X_DERIVE) fe_gesture(q, -1, g, 0.8f);
    run(q, SR, 1.0f, 128, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    stats all = run(q, SR, 5.0f, 128, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / (5.0 * SR / 128);
    printf("  info 16 voices + 5 gestures on all: %.1f us per block\n", us);
    CHECK(all.nan == 0 && all.peak <= 1.0f, "everything at once stays finite and bounded (peak %.3f)", all.peak);
    fe_destroy(q);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
