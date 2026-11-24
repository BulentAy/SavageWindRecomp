// Local proof of the pose and actor ABI. No transport. SAVAGEWIND_COOP_ACTOR=<name> spawns another actor than the
// stock NPC, e.g. the ghost Link tools/coop_ghost_assets.py builds into a SAVAGEWIND_ROMFS_OVERLAY folder.
#include "coop.h"
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

int g_coop_local;

RT_ON_RESET(coop_reset) {
    g_coop_local = 0;
    scratch = NULL;
    memset(&local, 0, sizeof local);
}

void coop_start(void) {
    const char *mode = getenv("SAVAGEWIND_COOP_TEST");
    if (!mode || strcmp(mode, "local")) return;
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
        !dispatch_lookup(g_site.coop_release_wake)) {
        LOG("[coop] FAIL bridge sites unavailable for this game library\n");
        return;
    }
    g_coop_local = 1;
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

static void request(Cpu *c, uint64_t creator, uint64_t mgr, const float xyz[3], uint64_t frame) {
    uint64_t heap, accepted;
    if (!call(c, "getHeap", g_site.coop_get_heap, &mgr, 1, &heap)) return;
    if (!ptr(heap)) { fail("invalid creation heap"); return; }

    memcpy(local.origin, xyz, sizeof local.origin);
    memcpy(local.target, xyz, sizeof local.target);
    local.target[0] += 3.0f;
    scratch = hle_alloc(sizeof *scratch);
    memcpy(scratch->name, test_actor, sizeof test_actor);
    memcpy(scratch->key, "@P", sizeof scratch->key);
    uint64_t key = (uintptr_t)scratch->key;
    scratch->params.count = 1;
    scratch->params.used = sizeof key + 1 + sizeof local.target;
    memcpy(scratch->params.data, &key, sizeof key);
    scratch->params.data[sizeof key] = 4; // Vec3
    memcpy(scratch->params.data + sizeof key + 1, local.target, sizeof local.target);
    const uint64_t args[] = {creator, (uintptr_t)scratch->name, heap, (uintptr_t)&scratch->handle,
                             (uintptr_t)&scratch->params, 0, 1};
    if (!call(c, "requestCreateActor", g_site.coop_request_actor, args, 7, &accepted)) return;
    if (!(uint8_t)accepted) { fail("actor request rejected"); return; }
    local.creator = creator;
    local.phase = PENDING;
    local.deadline = frame + 600;
    LOG("[coop] SPAWN update=%llu %s link=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f)\n",
        (unsigned long long)frame, test_actor, xyz[0], xyz[1], xyz[2], local.target[0], local.target[1], local.target[2]);
}

void rt_coop_update(Cpu *c) {
    uint32_t thread = kernel_current_thread_handle();
    if (!thread) return;
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
            request(c, creator, mgr, xyz, frame);
        }
        return;
    }
    if (creator != local.creator) { fail("ActorCreator changed before actor confirmation"); return; }
    if (local.phase == PENDING) {
        uint64_t unit = read_ptr((uintptr_t)&scratch->handle.unit);
        uint32_t state = 0;
        if (scratch->handle.failed || (unit && control_mem_peek(unit, &state, sizeof state) && (state == 3 || state == 5))) {
            fail("asynchronous actor creation failed");
            return;
        }
        if (state != 2) return;
        uint64_t ready = read_ptr(unit + 0x10), handle = (uintptr_t)&scratch->handle;
        LOG("[coop] HANDLE ready update=%llu\n", (unsigned long long)frame);
        if (!ready) { fail("ready handle has no actor"); return; }
        if (!call(c, "releaseAndWakeProc", g_site.coop_release_wake, &handle, 1, &local.actor)) return;
        if (local.actor != ready || scratch->handle.unit) { fail("wake did not release the expected actor"); return; }
        local.phase = WOKEN;
        return;
    }
    char name[sizeof test_actor];
    size_t name_n = strlen(test_actor) + 1;
    if (local.actor == link || !actor_calc(local.actor) || !listed(creator, local.actor) ||
        !control_mem_peek(local.actor + 0x1c, name, name_n) || memcmp(name, test_actor, name_n) ||
        !pose(local.actor, m)) return;
    float actual[] = {m[3], m[7], m[11]};
    LOG("[coop] ALIVE update=%llu %s listed=1 state=Calc actor=(%.3f,%.3f,%.3f) distance=%.3f target_error=%.3f "
        "link=(%.3f,%.3f,%.3f) link_delta=%.3f\n", (unsigned long long)frame, test_actor, actual[0], actual[1], actual[2],
        distance(actual, xyz), distance(actual, local.target), xyz[0], xyz[1], xyz[2], distance(xyz, local.origin));
    local.phase = FINISHED;
}
