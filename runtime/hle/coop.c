// Co-op ghost. SAVAGEWIND_COOP_TEST=local: local proof of the pose and actor ABI, no transport. With networking
// (runtime/coop_net.c) the first remote peer's pose drives one ghost actor. SAVAGEWIND_COOP_ACTOR=<name> picks the
// actor (local default: a stock NPC; network default: CoopPlayer, which tools/coop_ghost_assets.py builds into a
// SAVAGEWIND_ROMFS_OVERLAY folder).
#include "coop.h"
#include "coop_net.h"
#include "control_mem.h"
#include "framepace.h"
#include "gamelib.h"
#include "game_sites.h"
#include "hle.h"

#include <math.h>

static char test_actor[32] = "Npc_Kakariko003";  // (or SAVAGEWIND_COOP_ACTOR)

typedef struct {
    uint64_t unit;
    uint8_t failed;
    uint8_t pad[7];
} Handle;

typedef struct {
    uint64_t vtable, proc;
    uint16_t count, used;
    uint8_t data[0xc0];
} Params;

_Static_assert(sizeof(Handle) == 0x10, "BaseProcHandle ABI");
_Static_assert(sizeof(Params) == 0xd8 && offsetof(Params, data) == 0x14, "InstParamPack ABI");

// Keep every argument the asynchronous create might reference until guest teardown.
static struct {
    Handle handle;
    Params params;
    char name[sizeof test_actor], key[3];
} *scratch;

enum Phase { WAIT_LINK, PENDING, WOKEN, FINISHED };
static struct {
    enum Phase phase;
    bool calling;
    uint32_t thread;
    uint64_t link, creator, actor, deadline, since, since_frame;
    unsigned stable, samples;
    float origin[3], target[3];
} local;

enum { SAMPLES = 8, DELAY_MS = 120, STALE_MS = 2000, RANGE_M = 120, MAX_SPAWNS = 8, LOG_EVERY = 6 };
enum Ghost { GHOST_NONE, GHOST_PENDING, GHOST_ALIVE };
static struct {
    uint32_t peer;
    unsigned n, spawns, updates;
    double last_ms;  // newest pose, game-thread receive time
    struct { double ms; float m[12]; } ring[SAMPLES];  // oldest first
    enum Ghost state;
    bool gone, wrote;
    uint64_t link, creator, actor, deadline;
    float put[12], err_max, link_at_spawn[3];
} ghost;

int g_coop_local;  // 1: local test, 2: network ghost (patches.py _vfr_ratio calls rt_coop_update while set)

RT_ON_RESET(coop_reset) {
    g_coop_local = 0;
    scratch = NULL;
    memset(&local, 0, sizeof local);
    memset(&ghost, 0, sizeof ghost);
}

void coop_start(bool net) {
    const char *mode = getenv("SAVAGEWIND_COOP_TEST");
    bool test = mode && !strcmp(mode, "local");
    if (!test && !net) return;
    if (!test) memcpy(test_actor, "CoopPlayer", sizeof "CoopPlayer");
    const char *actor = getenv("SAVAGEWIND_COOP_ACTOR");
    if (actor && *actor) {
        size_t n = strspn(actor, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");
        if (actor[n] || n >= sizeof test_actor) {
            LOG("[coop] FAIL SAVAGEWIND_COOP_ACTOR: up to %zu letters, digits or _\n", sizeof test_actor - 1);
            return;
        }
        memcpy(test_actor, actor, n + 1);
    }
    if (strcmp(gamelib_tables()->version, "1.9.0") || !g_site.coop_actor_creator || !g_site.coop_heap_mgr ||
        !dispatch_lookup(g_site.coop_get_heap) || !dispatch_lookup(g_site.coop_request_actor) ||
        !dispatch_lookup(g_site.coop_release_wake) || !dispatch_lookup(g_site.coop_delete_actor)) {
        LOG("[coop] FAIL bridge sites unavailable for this game library\n");
        return;
    }
    g_coop_local = test ? 1 : 2;
    if (!test) LOG("[coop] NET ghost actor %s\n", test_actor);
}

static bool ptr(uint64_t a) { return a >= GUEST_LO && a < GUEST_HI && !(a & 7); }

static uint64_t read_ptr(uint64_t addr) {
    uint64_t a;
    return control_mem_peek(addr, &a, sizeof a) && ptr(a) ? a : 0;
}

static bool actor_calc(uint64_t a) {
    uint8_t state;
    return ptr(a) && control_mem_peek(a + 0x64, &state, sizeof state) && state == 1;
}

static bool listed(uint64_t creator, uint64_t actor) {
    uint64_t sentinel = creator + 0x38;
    uint32_t meta[2]; // node count and offset inside an actor
    if (!creator || !control_mem_peek(sentinel + 0x10, meta, sizeof meta) || meta[0] > 20000 ||
        meta[1] > 0x2000 || (meta[1] & 7)) return false;
    uint64_t node = read_ptr(sentinel + 8);
    for (unsigned i = 0; i < meta[0] && node && node != sentinel; i++, node = read_ptr(node + 8))
        if (node == actor + meta[1]) return true;
    return false;
}

static float distance(const float a[3], const float b[3]) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x*x + y*y + z*z);
}

static bool pose(uint64_t a, float m[12]) {
    if (!ptr(a) || !control_mem_peek(a + 0x398, m, 12 * sizeof *m)) return false;
    for (int i = 0; i < 12; i++)
        if (!isfinite(m[i])) return false;
    return true;
}

static void fail(const char *why) {
    LOG("[coop] FAIL %s\n", why);
    local.phase = FINISHED;
}

static bool call(Cpu *c, const char *name, uint64_t fn, const uint64_t *args, size_t n, uint64_t *result) {
    if (local.calling || c != kernel_current_cpu() || !c->sp || (c->sp & 15) || !dispatch_lookup(fn)) {
        fail("unsafe guest call context");
        return false;
    }
    Cpu saved;
    memcpy(&saved, c, sizeof saved);
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr(), frame = framepace_game_frame();
    local.calling = true;
    memcpy(c->x, args, n * sizeof *args);
    guest_call(c, fn);
    uint64_t ret = c->x[0];
    bool stack_ok = c->sp == saved.sp;
    memcpy(c, &saved, sizeof saved);
    rt_set_fpcr(fpcr);
    rt_set_fpsr(fpsr);
    local.calling = false;
    if (!stack_ok || memcmp(c, &saved, sizeof saved)) {
        fail("guest call did not preserve the CPU/stack");
        return false;
    }
    if (framepace_game_frame() != frame) {
        fail("VFR updated during guest call");
        return false;
    }
    *result = ret;
    LOG("[coop] CALL %s cpu=preserved\n", name);
    return true;
}

// Asks for test_actor at target. A fresh scratch per request: the game may still reference an abandoned one.
static bool request(Cpu *c, uint64_t creator, uint64_t mgr, const float target[3]) {
    uint64_t heap, accepted;
    if (!call(c, "getHeap", g_site.coop_get_heap, &mgr, 1, &heap)) return false;
    if (!ptr(heap)) { fail("invalid creation heap"); return false; }
    scratch = hle_alloc(sizeof *scratch);
    memcpy(scratch->name, test_actor, sizeof test_actor);
    memcpy(scratch->key, "@P", sizeof scratch->key);
    uint64_t key = (uintptr_t)scratch->key;
    scratch->params.count = 1;
    scratch->params.used = sizeof key + 1 + 3 * sizeof *target;
    memcpy(scratch->params.data, &key, sizeof key);
    scratch->params.data[sizeof key] = 4; // Vec3
    memcpy(scratch->params.data + sizeof key + 1, target, 3 * sizeof *target);
    const uint64_t args[] = {creator, (uintptr_t)scratch->name, heap, (uintptr_t)&scratch->handle,
                             (uintptr_t)&scratch->params, 0, 1};
    if (!call(c, "requestCreateActor", g_site.coop_request_actor, args, 7, &accepted)) return false;
    if (!(uint8_t)accepted) { fail("actor request rejected"); return false; }
    return true;
}

// The pending request: -1 failed (logged), 0 still pending, 1 woken (*actor).
static int wake(Cpu *c, uint64_t frame, uint64_t *actor) {
    uint64_t unit = read_ptr((uintptr_t)&scratch->handle.unit);
    uint32_t state = 0;
    if (scratch->handle.failed || (unit && control_mem_peek(unit, &state, sizeof state) && (state == 3 || state == 5))) {
        fail("asynchronous actor creation failed");
        return -1;
    }
    if (state != 2) return 0;
    uint64_t ready = read_ptr(unit + 0x10), handle = (uintptr_t)&scratch->handle;
    LOG("[coop] HANDLE ready update=%llu\n", (unsigned long long)frame);
    if (!ready) { fail("ready handle has no actor"); return -1; }
    if (!call(c, "releaseAndWakeProc", g_site.coop_release_wake, &handle, 1, actor)) return -1;
    if (*actor != ready || scratch->handle.unit) { fail("wake did not release the expected actor"); return -1; }
    return 1;
}

static void spawn_local(Cpu *c, uint64_t creator, uint64_t mgr, const float xyz[3], uint64_t frame) {
    memcpy(local.origin, xyz, sizeof local.origin);
    memcpy(local.target, xyz, sizeof local.target);
    local.target[0] += 3.0f;
    if (!request(c, creator, mgr, local.target)) return;
    local.creator = creator;
    local.phase = PENDING;
    local.deadline = frame + 600;
    LOG("[coop] SPAWN update=%llu %s link=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f)\n",
        (unsigned long long)frame, test_actor, xyz[0], xyz[1], xyz[2], local.target[0], local.target[1], local.target[2]);
}

static bool is_actor(uint64_t a, uint64_t link) {
    char name[sizeof test_actor];
    size_t name_n = strlen(test_actor) + 1;
    return a != link && actor_calc(a) && control_mem_peek(a + 0x1c, name, name_n) && !memcmp(name, test_actor, name_n);
}

static double now_ms(void) { return plat_now_ns() / 1.0e6; }

static void forget_ghost(const char *why) {
    if (ghost.state != GHOST_NONE) LOG("[coop] LOST %s state=%d\n", why, ghost.state);
    ghost.state = GHOST_NONE;  // (a pending handle's scratch is abandoned, never reused)
    ghost.actor = ghost.creator = 0;
    ghost.wrote = false;
}

static void drain(void) {
    CoopNetEvent e;
    double now = now_ms();
    while (coop_net_pop(&e)) {
        if (e.type == COOP_NET_LEFT) {
            if (e.id == ghost.peer) ghost.gone = true;
            LOG("[coop] RX LEFT id=%u\n", e.id);
            continue;
        }
        if (e.type == COOP_NET_JOIN || e.id != ghost.peer) {
            if (e.id != ghost.peer) ghost.n = 0;  // (no interpolation across peers)
            if (e.type == COOP_NET_JOIN) LOG("[coop] RX JOIN id=%u\n", e.id);
            ghost.peer = e.id;
            if (e.type == COOP_NET_JOIN) continue;
        }
        if (ghost.gone) ghost.n = 0;  // (a returning peer starts a fresh history)
        if (ghost.n == SAMPLES) memmove(ghost.ring, ghost.ring + 1, --ghost.n * sizeof *ghost.ring);
        ghost.ring[ghost.n].ms = now;
        memcpy(ghost.ring[ghost.n++].m, e.m, sizeof e.m);
        ghost.last_ms = now;
        ghost.gone = false;
    }
    if (ghost.n && now - ghost.last_ms > STALE_MS) ghost.gone = true;
}

// Rendered DELAY_MS behind the newest pose: translation interpolated, newest rotation, no extrapolation.
static void target_pose(float out[12]) {
    double t = now_ms() - DELAY_MS;
    memcpy(out, ghost.ring[ghost.n - 1].m, 12 * sizeof *out);
    unsigned i = ghost.n - 1;
    while (i && ghost.ring[i - 1].ms > t) i--;
    if (!i || ghost.ring[i].ms <= t) {
        for (int k = 3; k < 12; k += 4) out[k] = ghost.ring[i].m[k];
        return;
    }
    double a = ghost.ring[i - 1].ms, b = ghost.ring[i].ms, f = b > a ? (t - a) / (b - a) : 1;
    for (int k = 3; k < 12; k += 4)
        out[k] = (float)(ghost.ring[i - 1].m[k] + f * (ghost.ring[i].m[k] - ghost.ring[i - 1].m[k]));
}

// One remote ghost, on the game thread: spawn near Link, place it every update, remove it when the peer goes.
static void net_update(Cpu *c) {
    drain();
    uint64_t frame = framepace_game_frame();
    if (local.phase == FINISHED) return;
    if (framepace_title_ui() || framepace_loading_ui()) { forget_ghost("world exit"); return; }
    if (framepace_paused()) return;
    uint64_t pi = read_ptr(g_site.player_info), link = pi ? read_ptr(pi + 0x60) : 0;
    float m[12];
    if (!actor_calc(link) || !pose(link, m)) return;
    if (link != ghost.link) { forget_ghost("Link changed"); ghost.link = link; }
    float xyz[3] = {m[3], m[7], m[11]};
    coop_net_push(m);
    uint64_t creator = read_ptr(g_site.coop_actor_creator);
    if (ghost.state != GHOST_NONE && creator != ghost.creator) { forget_ghost("ActorCreator changed"); return; }
    bool log = ++ghost.updates % LOG_EVERY == 0;

    if (ghost.state == GHOST_ALIVE) {
        if (!is_actor(ghost.actor, link) || (ghost.updates % 30 == 0 && !listed(creator, ghost.actor))) {
            forget_ghost("ghost no longer listed/Calc");
            return;
        }
        float back[12];
        if (ghost.wrote && pose(ghost.actor, back)) {  // what the game made of last update's write
            float d[3] = {back[3] - ghost.put[3], back[7] - ghost.put[7], back[11] - ghost.put[11]};
            float e = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (e > ghost.err_max) ghost.err_max = e;
        }
        if (ghost.gone) {
            uint64_t args[] = {ghost.actor, 0}, ignored;
            if (!call(c, "deleteLater", g_site.coop_delete_actor, args, 2, &ignored)) return;
            LOG("[coop] REMOVE update=%llu peer=%u silent_ms=%.0f\n", (unsigned long long)frame, ghost.peer,
                now_ms() - ghost.last_ms);
            forget_ghost("removed");
            ghost.n = 0;
            return;
        }
    }
    if (ghost.state == GHOST_PENDING) {
        int r = wake(c, frame, &ghost.actor);
        if (r < 0) return;
        if (!r) { if (frame > ghost.deadline) fail("ghost confirmation timed out"); return; }
        ghost.state = GHOST_ALIVE;
        LOG("[coop] ALIVE update=%llu %s link=(%.3f,%.3f,%.3f) link_delta=%.3f\n", (unsigned long long)frame,
            test_actor, xyz[0], xyz[1], xyz[2], distance(xyz, ghost.link_at_spawn));
    }
    if (!ghost.n || ghost.gone) {
        if (log) {
            unsigned tx, rx;
            coop_net_counts(&tx, &rx);
            LOG("[coop] TRACK t=%.1f update=%llu link=(%.3f,%.3f,%.3f) tx=%u rx=%u ghost=none\n", now_ms(),
                (unsigned long long)frame, xyz[0], xyz[1], xyz[2], tx, rx);
        }
        return;
    }
    float want[12];
    target_pose(want);
    float at[3] = {want[3], want[7], want[11]};
    if (ghost.state == GHOST_NONE) {
        uint64_t mgr = read_ptr(g_site.coop_heap_mgr);
        if (distance(at, xyz) >= RANGE_M || framepace_in_event() || !mgr || !listed(creator, link)) return;
        if (ghost.spawns == MAX_SPAWNS) { fail("ghost spawn limit reached"); return; }
        if (!request(c, creator, mgr, at)) return;
        ghost.spawns++;
        ghost.state = GHOST_PENDING;
        ghost.creator = creator;
        ghost.deadline = frame + 600;
        memcpy(ghost.link_at_spawn, xyz, sizeof xyz);
        LOG("[coop] SPAWN update=%llu %s peer=%u target=(%.3f,%.3f,%.3f) link=(%.3f,%.3f,%.3f)\n",
            (unsigned long long)frame, test_actor, ghost.peer, at[0], at[1], at[2], xyz[0], xyz[1], xyz[2]);
        return;
    }
    if (ghost.state != GHOST_ALIVE) return;
    ghost.wrote = control_mem_poke(ghost.actor + 0x398, want, sizeof want);
    memcpy(ghost.put, want, sizeof want);
    if (log) {
        unsigned tx, rx;
        coop_net_counts(&tx, &rx);
        const float *r = ghost.ring[ghost.n - 1].m;
        LOG("[coop] TRACK t=%.1f update=%llu link=(%.3f,%.3f,%.3f) tx=%u rx=%u remote=(%.3f,%.3f,%.3f) "
            "ghost=(%.3f,%.3f,%.3f) readback_max=%.4f written=%d\n", now_ms(), (unsigned long long)frame,
            xyz[0], xyz[1], xyz[2], tx, rx, r[3], r[7], r[11], at[0], at[1], at[2], ghost.err_max, ghost.wrote);
        ghost.err_max = 0;
    }
}

void rt_coop_update(Cpu *c) {
    uint32_t thread = kernel_current_thread_handle();
    if (!thread) return;
    if (g_coop_local == 2) {
        // The frame's VFR update moves between guest threads (measured: 3 of them): once per update, never nested.
        static int busy;
        static uint64_t last;
        uint64_t frame = framepace_game_frame();
        if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;
        if (frame != last && !local.calling) { last = frame; net_update(c); }
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
        return;
    }
    uint32_t owner = __atomic_load_n(&local.thread, __ATOMIC_RELAXED);
    if (!owner && __atomic_compare_exchange_n(&local.thread, &owner, thread, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        owner = thread;
    if (owner != thread || local.calling) return;
    uint64_t frame = framepace_game_frame();
    if (framepace_title_ui() || framepace_loading_ui()) {
        local.stable = 0;
        if (local.phase == PENDING || local.phase == WOKEN) fail("world exited before actor confirmation");
        return;
    }
    if (framepace_paused()) { local.stable = 0; return; }
    if ((local.phase == PENDING || local.phase == WOKEN) && frame > local.deadline) fail("actor confirmation timed out");
    uint64_t pi = read_ptr(g_site.player_info), link = pi ? read_ptr(pi + 0x60) : 0;
    float m[12], cache[3];
    if (!actor_calc(link) || !pose(link, m) || !control_mem_peek(pi + 0x88, cache, sizeof cache)) {
        local.stable = 0;
        return;
    }
    float xyz[3] = {m[3], m[7], m[11]};
    float error = distance(xyz, cache);
    if (!isfinite(error)) { local.stable = 0; return; }
    if (local.link != link) {
        if (local.phase == PENDING || local.phase == WOKEN) fail("Link changed before actor confirmation");
        local.link = link;
        local.stable = 0;
    }
    uint64_t now = plat_now_ns();
    if (!local.since) local.since = now, local.since_frame = frame;
    if (++local.samples == 120 && now > local.since) {
        LOG("[coop] CADENCE thread=%#x update=%llu %.2f updates/s\n", thread, (unsigned long long)frame,
            1.0e9 * (frame - local.since_frame) / (now - local.since));
        local.since = now;
        local.since_frame = frame;
        local.samples = 0;
    }
    LOG("[coop] POSE update=%llu xyz=(%.3f,%.3f,%.3f) rot=(%.4f,%.4f,%.4f;%.4f,%.4f,%.4f;%.4f,%.4f,%.4f) cache_error=%.4f\n",
        (unsigned long long)frame, xyz[0], xyz[1], xyz[2], m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], error);

    if (local.phase == FINISHED) return;
    uint64_t creator = read_ptr(g_site.coop_actor_creator);
    if (local.phase == WAIT_LINK) {
        if (local.stable < 30 && ++local.stable < 30) return;
        uint64_t mgr = read_ptr(g_site.coop_heap_mgr);
        if (error < 1.0f && !framepace_in_event() && mgr && listed(creator, link)) {
            LOG("[coop] READY thread=%#x update=%llu Link is listed and Calc\n", thread, (unsigned long long)frame);
            spawn_local(c, creator, mgr, xyz, frame);
        }
        return;
    }
    if (creator != local.creator) { fail("ActorCreator changed before actor confirmation"); return; }
    if (local.phase == PENDING) {
        if (wake(c, frame, &local.actor) == 1) local.phase = WOKEN;
        return;
    }
    if (!is_actor(local.actor, link) || !listed(creator, local.actor) || !pose(local.actor, m)) return;
    float actual[] = {m[3], m[7], m[11]};
    LOG("[coop] ALIVE update=%llu %s listed=1 state=Calc actor=(%.3f,%.3f,%.3f) distance=%.3f target_error=%.3f "
        "link=(%.3f,%.3f,%.3f) link_delta=%.3f\n", (unsigned long long)frame, test_actor, actual[0], actual[1], actual[2],
        distance(actual, xyz), distance(actual, local.target), xyz[0], xyz[1], xyz[2], distance(xyz, local.origin));
    local.phase = FINISHED;
}
