/*
 * Fouille engine — live harvest test, run ON the Move (static binary).
 * Starts the engine online, lets the worker listen to the first voices'
 * places and prints each card. No audio output; writes nothing.
 *
 *   ./net_test [seconds] [voices]
 */
#define _POSIX_C_SOURCE 200809L
#include "fe.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void msleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 90;
    int want = argc > 2 ? atoi(argv[2]) : 6;
    fe_config c = {0};
    c.sample_rate = 44100; c.max_block = 128; c.voice_seconds = 30; c.seed = (uint32_t)time(NULL);
    c.key_paths[0] = "/data/UserData/UserLibrary/Fouille/freesound.key";
    c.key_paths[1] = "/data/UserData/schwung/modules/overtake/fouille/freesound.key";
    c.key_paths[2] = "/data/UserData/schwung/modules/sound_generators/fouille/freesound.key";
    fe_engine *e = fe_create(&c);
    if (!e) { printf("fe_create failed\n"); return 1; }
    static const char *K[] = { "-", "Aporee", "Freesound", "synthese" };
    int shown[FE_NV] = {0}, got = 0;
    float l[128], r[128];
    char st[64];
    for (int t = 0; t < secs * 100 && got < want; t++) {
        fe_render(e, l, r, 128);                      /* the hand-over happens in render */
        for (int v = 0; v < want && v < FE_NV; v++) {
            if (shown[v] || fe_voice_state(e, v) < FE_VS_READY) continue;
            fe_card k; fe_voice_card(e, v, &k);
            printf("[%5.1f s] voix %2d  %-12s %-9s %5.1f s  %s / %s / %s  (%.4f, %.4f)  %s\n",
                   t / 100.0, v + 1, k.place, K[k.kind & 3], k.seconds, k.title, k.author, k.license,
                   k.lat, k.lon, k.url);
            fflush(stdout);
            shown[v] = 1; got++;
        }
        if (t % 500 == 0) { fe_net_status(e, st, sizeof st); printf("           ... %s\n", st); fflush(stdout); }
        msleep(10);
    }
    fe_net_status(e, st, sizeof st);
    printf("%d/%d voix remplies, etat: %s\n", got, want, st);
    fe_destroy(e);
    return got == want ? 0 : 1;
}
