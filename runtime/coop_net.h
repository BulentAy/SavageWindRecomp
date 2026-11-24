#pragma once
#include <stdbool.h>
#include <stdint.h>

// Lifecycle on the host thread, with the game-side caller quiescent at start.
// No endpoint means no worker. A successful start does not imply authentication.
bool coop_net_start(void);
void coop_net_stop(void);
bool coop_net_connected(void);

typedef struct {
    enum { COOP_NET_JOIN, COOP_NET_POSE, COOP_NET_LEFT } type;
    uint32_t id;
    uint64_t received_ms; // local monotonic time, not a wire timestamp
    float m[12];         // POSE only, row-major 3x4
} CoopNetEvent;

// One game-thread caller. These never wait; push drops on overflow. Drain pop
// to the newest pose, applying JOIN and LEFT in order. JOIN means first valid pose.
bool coop_net_push(const float m[12]);
bool coop_net_pop(CoopNetEvent *event);
// MTX records handed to the socket / valid remote poses queued, since start (diagnostics).
void coop_net_counts(unsigned *sent, unsigned *received);
