/*
 * fouille_probe — Step 0 network check for Fouille, run ON the Move.
 *
 * It exercises the exact path the module will use: a C process that runs
 * curl through popen(), reads the bytes from the pipe, and decodes MP3 with
 * minimp3. Nothing here touches the audio thread; it is a standalone binary.
 *
 *   fouille_probe curl                      which curl, version, protocols
 *   fouille_probe fetch <url> [max_kb]      download (range), decode, report
 *   fouille_probe stream <url> <seconds>    decode a live stream as it arrives
 *   fouille_probe file <path.mp3>           decode a local file
 *
 * Writes nothing to disk. Exit code 0 = the step worked.
 * License: MIT (Fouille). minimp3: CC0.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "../engine/minimp3.h"

static const char *CURL_CANDIDATES[] = {
    "/data/UserData/schwung/bin/curl",
    "/usr/bin/curl",
    "/bin/curl",
    "/usr/local/bin/curl",
    "/data/UserData/schwung/modules/overtake/fouille/bin/curl",
    NULL
};

static const char *find_curl(void) {
    const char *env = getenv("FOUILLE_CURL");
    if (env && access(env, X_OK) == 0) return env;
    for (int i = 0; CURL_CANDIDATES[i]; i++)
        if (access(CURL_CANDIDATES[i], X_OK) == 0) return CURL_CANDIDATES[i];
    return NULL;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The URL goes between single quotes for the shell; refuse anything that
 * could break out of them. Real URLs never contain a quote. */
static int url_ok(const char *u) {
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) return 0;
    for (const char *p = u; *p; p++)
        if (*p == '\'' || *p == '\n' || *p == '\r') return 0;
    return 1;
}

/* Running statistics over decoded audio. */
typedef struct {
    long frames_ok, frames_skipped;
    long samples;          /* per channel */
    int hz, channels, kbps;
    double sum2, peak;
} stats_t;

static void stats_add(stats_t *s, const float *pcm, int n, const mp3dec_frame_info_t *fi) {
    s->frames_ok++;
    s->hz = fi->hz; s->channels = fi->channels; s->kbps = fi->bitrate_kbps;
    for (int i = 0; i < n * fi->channels; i++) {
        double v = pcm[i];
        s->sum2 += v * v;
        if (fabs(v) > s->peak) s->peak = fabs(v);
    }
    s->samples += n;
}

static void stats_print(const stats_t *s, double wall) {
    double secs = s->hz ? (double)s->samples / s->hz : 0.0;
    long nvals = s->samples * (s->channels ? s->channels : 1);
    double rms = nvals ? sqrt(s->sum2 / nvals) : 0.0;
    printf("  decoded     : %.2f s of audio (%ld frames, %ld skipped)\n",
           secs, s->frames_ok, s->frames_skipped);
    printf("  format      : %d Hz, %d ch, %d kbps (last frame)\n",
           s->hz, s->channels, s->kbps);
    printf("  level       : rms %.1f dBFS, peak %.1f dBFS\n",
           rms > 0 ? 20 * log10(rms) : -999.0, s->peak > 0 ? 20 * log10(s->peak) : -999.0);
    if (wall > 0) printf("  wall time   : %.2f s\n", wall);
}

/* Decode everything in buf; returns bytes consumed. If !final, keeps a tail
 * so a frame is never cut in half at a read boundary. */
static size_t decode_buf(mp3dec_t *dec, const uint8_t *buf, size_t len, int final, stats_t *s) {
    static float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t off = 0;
    const size_t keep = final ? 0 : 4096;   /* > 2 max-size frames */
    while (len - off > keep) {
        mp3dec_frame_info_t fi;
        int n = mp3dec_decode_frame(dec, buf + off, (int)(len - off), pcm, &fi);
        if (fi.frame_bytes == 0) {
            if (final) return len;          /* no more frames */
            break;                          /* need more data */
        }
        if (n > 0) stats_add(s, pcm, n, &fi);
        else s->frames_skipped++;
        off += fi.frame_bytes;
    }
    return off;
}

/* Read a pipe/file to the end (or max bytes), decoding as bytes arrive. */
static int decode_stream(FILE *f, size_t max_bytes, stats_t *s, size_t *bytes_out,
                         double *first_byte_s, double *longest_gap_s) {
    static uint8_t buf[1 << 16];
    mp3dec_t dec; mp3dec_init(&dec);
    size_t have = 0, total = 0;
    double t0 = now_s(), last = t0;
    *first_byte_s = -1; *longest_gap_s = 0;
    for (;;) {
        size_t want = sizeof(buf) - have;
        if (max_bytes && total + want > max_bytes) want = max_bytes - total;
        if (want == 0) break;
        size_t n = fread(buf + have, 1, want > 2048 ? 2048 : want, f);
        if (n == 0) break;
        double t = now_s();
        if (*first_byte_s < 0) *first_byte_s = t - t0;
        else if (t - last > *longest_gap_s) *longest_gap_s = t - last;
        last = t;
        have += n; total += n;
        size_t used = decode_buf(&dec, buf, have, 0, s);
        memmove(buf, buf + used, have - used);
        have -= used;
    }
    decode_buf(&dec, buf, have, 1, s);
    *bytes_out = total;
    return total > 0;
}

static int cmd_curl(void) {
    const char *c = find_curl();
    if (!c) {
        printf("curl: NOT FOUND (looked in:");
        for (int i = 0; CURL_CANDIDATES[i]; i++) printf(" %s", CURL_CANDIDATES[i]);
        printf(")\n");
        return 1;
    }
    printf("curl: %s\n", c);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "'%s' -V 2>&1", c);
    FILE *p = popen(cmd, "r");
    if (!p) { printf("  popen failed\n"); return 1; }
    char line[512]; int https = 0;
    while (fgets(line, sizeof line, p)) {
        printf("  %s", line);
        if (strstr(line, "Protocols:") && strstr(line, "https")) https = 1;
    }
    int rc = pclose(p);
    printf("  https support: %s\n", https ? "yes" : "NO");
    return (rc == 0 && https) ? 0 : 1;
}

static int run_decode(const char *what, FILE *f, size_t max_bytes, int is_pipe) {
    stats_t s; memset(&s, 0, sizeof s);
    size_t bytes = 0; double fb, gap;
    double t0 = now_s();
    decode_stream(f, max_bytes, &s, &bytes, &fb, &gap);
    double wall = now_s() - t0;
    int rc = is_pipe ? pclose(f) : fclose(f);
    printf("%s\n", what);
    printf("  bytes       : %zu\n", bytes);
    if (fb >= 0) printf("  first byte  : %.2f s, longest gap %.2f s\n", fb, gap);
    stats_print(&s, wall);
    if (is_pipe && rc != 0) printf("  curl exit   : %d (28 = timeout, expected for streams)\n",
                                   WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
    return s.frames_ok > 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s curl | fetch <url> [max_kb] | stream <url> <s> | file <p>\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "curl")) return cmd_curl();

    if (!strcmp(argv[1], "file") && argc >= 3) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 1; }
        return run_decode("file decode:", f, 0, 0);
    }

    const char *curl = find_curl();
    if (!curl) { printf("curl not found, run: fouille_probe curl\n"); return 1; }

    if (!strcmp(argv[1], "fetch") && argc >= 3) {
        if (!url_ok(argv[2])) { printf("refusing URL\n"); return 2; }
        long kb = argc >= 4 ? atol(argv[3]) : 512;
        char cmd[2048];
        snprintf(cmd, sizeof cmd,
                 "'%s' -sSL --connect-timeout 8 --max-time 30 -r 0-%ld '%s'",
                 curl, kb * 1024 - 1, argv[2]);
        FILE *p = popen(cmd, "r");
        if (!p) { printf("popen failed\n"); return 1; }
        return run_decode("fetch + decode:", p, (size_t)kb * 1024, 1);
    }

    if (!strcmp(argv[1], "stream") && argc >= 4) {
        if (!url_ok(argv[2])) { printf("refusing URL\n"); return 2; }
        int secs = atoi(argv[3]);
        if (secs < 1) secs = 1;
        if (secs > 120) secs = 120;
        char cmd[2048];
        /* -N: no buffering, so bytes reach us as the radio sends them. */
        snprintf(cmd, sizeof cmd,
                 "'%s' -sSLN --connect-timeout 8 --max-time %d '%s'",
                 curl, secs, argv[2]);
        FILE *p = popen(cmd, "r");
        if (!p) { printf("popen failed\n"); return 1; }
        int rc = run_decode("stream + decode:", p, 0, 1);
        printf("  (a live stream should decode about as many seconds as it ran)\n");
        return rc;
    }

    fprintf(stderr, "bad arguments\n");
    return 2;
}
