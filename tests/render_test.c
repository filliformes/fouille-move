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
    system("mkdir -p tests/out");
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

    /* CPU: 16 voices playing, all colours, render time per 128-frame block */
    fe_engine *q = make(SR);
    for (int v = 0; v < FE_NV; v++) { fe_set(q, v, 5, (v % 3) / 2.0f); fe_set(q, v, 14, 0.5f); fe_set(q, v, 15, 0.8f); fe_gate(q, v, 1, 0.8f); }
    run(q, SR, 1.0f, 128, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    run(q, SR, 5.0f, 128, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / (5.0 * SR / 128);
    printf("  info 16 voices: %.1f us per 128-frame block (budget 2902 us)\n", us);
    fe_destroy(q);

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
