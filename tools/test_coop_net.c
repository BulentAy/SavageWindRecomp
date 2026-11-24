// Synthetic host driver; tools/test_coop_net.py supervises two copies and the relay.
#include "coop_net.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void pause_ms(unsigned ms) {
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

static void pose(float m[12], int peer, int seq) {
    const float v[12] = {0, 0, 1, peer * 10000 + seq, 0, 1, 0, -seq * 0.5f, -1, 0, 0, peer + 0.25f};
    memcpy(m, v, sizeof v);
}

static int wire(void) {
    assert(coop_net_start());
    for (int i = 0; !coop_net_connected() && i < 700; i++) pause_ms(10);
    assert(coop_net_connected());
    puts("authenticated");
    assert(getchar() == '\n'); // don't drain until the server has sent its burst
    for (int i = 0; coop_net_connected() && i < 300; i++) pause_ms(10);
    assert(!coop_net_connected()); // the oversized record ends the session
    CoopNetEvent event;
    assert(coop_net_pop(&event) && event.type == COOP_NET_JOIN && event.id == 9);
    int poses = 0;
    bool left = false;
    while (coop_net_pop(&event)) {
        assert(!left && event.id == 9);
        if (event.type == COOP_NET_LEFT) left = true;
        else {
            const float expected[] = {1, 0, 0, 123.5f, 0, 1, 0, -24, 0, 0, 1, 8.25f};
            assert(event.type == COOP_NET_POSE && !memcmp(event.m, expected, sizeof expected));
            assert((int)event.anim == (poses == 0 ? 104 : 0));
            poses++;
        }
    }
    assert(left && poses > 0 && poses < 48);
    coop_net_stop();
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(argv[1], "wire")) return wire();
    if (!strcmp(argv[1], "off")) {
        assert(!coop_net_start() && !coop_net_connected());
        CoopNetEvent event;
        assert(!coop_net_pop(&event));
        return 0;
    }
    int me = atoi(argv[1]), other = 3 - me;
    assert(me == 1 || me == 2);
    assert(coop_net_start());
    for (int round = 1; round <= 2; round++) {
        for (int i = 0; !coop_net_connected() && i < 1000; i++) pause_ms(10);
        assert(coop_net_connected());
        int seq = 0, last = -1, received = 0, goal = round == 1 ? 100 : 3;
        uint32_t id = 0;
        for (int tick = 0; tick < 2400; tick++) {
            CoopNetEvent event;
            while (coop_net_pop(&event)) {
                if (event.type == COOP_NET_JOIN) {
                    assert(!id && event.id);
                    id = event.id;
                } else if (event.type == COOP_NET_LEFT) {
                    assert(event.id == id && received >= goal);
                    id = 0;
                    if (round == 1) goto disconnected;
                } else {
                    assert(event.type == COOP_NET_POSE && id && event.id == id);
                    int sample = (int)event.m[3] - other * 10000;
                    float expected[12];
                    pose(expected, other, sample);
                    assert(sample > last && !memcmp(expected, event.m, sizeof expected));
                    assert((int)event.anim == 101 + sample % 11);
                    char clip[64];
                    snprintf(clip, sizeof clip, "CoopClip_Move_%d", sample % 3);
                    assert(!strcmp(event.clip.name, clip) && event.clip.frame == (float)sample + 0.5f);
                    last = sample;
                    if (++received == goal) {
                        unsigned tx, rx;
                        coop_net_counts(&tx, &rx);
                        assert(tx > 0 && rx >= (unsigned)received);
                        printf("round=%d exact=%d\n", round, received);
                    }
                }
            }
            if (tick % 12 == 0 && coop_net_connected()) {
                float matrix[12];
                pose(matrix, me, ++seq);
                CoopClip clip = {.frame = (float)seq + 0.5f};
                snprintf(clip.name, sizeof clip.name, "CoopClip_Move_%d", seq % 3);
                assert(coop_net_push(matrix, 101 + seq % 11, &clip));
            }
            if (round == 2 && received >= goal && seq >= 12) {
                coop_net_stop();
                assert(!coop_net_connected() && !coop_net_push((float[12]){0}, 0, NULL));
                assert(coop_net_start());
                assert(!coop_net_pop(&event));
                coop_net_stop();
                return 0;
            }
            pause_ms(10);
        }
        assert(!"timed out waiting for relay traffic or disconnect");
    disconnected:
        assert(!coop_net_connected());
        puts("disconnected");
    }
    return 1;
}
