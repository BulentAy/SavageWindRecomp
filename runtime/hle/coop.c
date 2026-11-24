// Co-op ghost. SAVAGEWIND_COOP_TEST=local: local proof of the pose and actor ABI, no transport. With networking
// (runtime/coop_net.c) the first remote peer's pose drives one ghost actor. SAVAGEWIND_COOP_ACTOR=<name> picks the
// actor (local default: a stock NPC; network default: CoopPlayer, which tools/coop_ghost_assets.py builds into a
// SAVAGEWIND_ROMFS_OVERLAY folder, with its hair and clothes as separate actors CoopPlayer<piece>).
#include "coop.h"
#include "coop_net.h"
#include "control_mem.h"
#include "framepace.h"
#include "gamelib.h"
#include "game_sites.h"
#include "hle.h"

#include <math.h>
#include <stdatomic.h>

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

enum { SAMPLES = 8, DELAY_MS = 120, MIN_GAP_MS = 25, STALE_MS = 2000, RANGE_M = 120, MAX_SPAWNS = 8, LOG_EVERY = 6 };
enum Ghost { GHOST_NONE, GHOST_PENDING, GHOST_ALIVE };
// The ghost's worn pieces (tools/coop_ghost_assets.py PIECES): optional actors placed and animated with it.
enum { PIECES = 3 };
static const char *const piece_names[PIECES] = {"Hair", "Upper", "Lower"};
static const char *const sequences[] = {
    "Wait", "CoopWalk", "CoopRun", "CoopSprint", "CoopJump", "CoopFall", "CoopClimb", "CoopClimbWait",
    "CoopSwim", "CoopSwimWait", "CoopGlide",
};
_Static_assert(sizeof sequences / sizeof *sequences == COOP_ANIM_GLIDE - COOP_ANIM_IDLE + 1, "locomotion table");
static struct { uint64_t vtable, data; char name[64]; } *as_name;
// The AS hook can run on a different guest thread. Publish the actor and its state together.
static _Atomic uint64_t observed_as;
static bool anim_bridge;
static struct {
    float x, z, speed;
    double ms;
    CoopAnim last;
    char last_clip[64];
} gait;
static struct {
    uint32_t peer;
    unsigned n, spawns, updates;
    double last_ms;  // newest pose, game-thread receive time
    struct { double ms; CoopAnim anim; CoopClip clip; float m[12]; } ring[SAMPLES];  // oldest first
    enum Ghost state;
    char playing[64];
    bool gone, wrote;
    uint64_t link, creator, actor, deadline;
    struct { uint64_t actor; char playing[64]; bool off; } piece[PIECES];
    unsigned pending;  // 1 + the piece whose creation is pending, 0 none
    uint64_t piece_deadline;
    float put[12], err_max, link_at_spawn[3];
    // written heading; its largest per-update change (degrees) and turn rate (degrees/s) since the last TRACK
    float yaw, yaw_step_max, yaw_rate_max;
    double yaw_ms;
    unsigned yaw_steps, yaw_still;  // per-update changes counted / below 0.01 degree, since the last TRACK
} ghost;

int g_coop_local;  // 1: local test, 2: network ghost (patches.py _vfr_ratio calls rt_coop_update while set)

RT_ON_RESET(coop_reset) {
    g_coop_local = 0;
    scratch = NULL;
    as_name = NULL;
    anim_bridge = false;
    atomic_store(&observed_as, 0);
    memset(&gait, 0, sizeof gait);
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
    anim_bridge = !test && g_site.coop_string_vtable_slot && dispatch_lookup(g_site.coop_play_as);
    g_coop_local = test ? 1 : 2;
    if (!test) LOG("[coop] NET ghost actor %s\n", test_actor);
}

static bool ptr(uint64_t a) { return a >= GUEST_LO && a < GUEST_HI && !(a & 7); }

static uint64_t read_ptr(uint64_t addr) {
    uint64_t a;
    return control_mem_peek(addr, &a, sizeof a) && ptr(a) ? a : 0;
}

static uint64_t player(void) {
    uint64_t info = read_ptr(g_site.player_info);
    return info ? read_ptr(info + 0x60) : 0;
}

static CoopAnim classify_as(const char *name) {
    if (!strcmp(name, "ParashawlGlide") || !strcmp(name, "SurfingOnPara")) return COOP_ANIM_GLIDE;
    if (!strcmp(name, "ClimbWait") || !strcmp(name, "LadderWait")) return COOP_ANIM_CLIMB_WAIT;
    if (!strncmp(name, "ClimbMove", 9) || !strncmp(name, "ClimbJump", 9) || !strncmp(name, "WallDash", 8) ||
        !strcmp(name, "ClimbSlip") || !strncmp(name, "LadderMove", 10)) return COOP_ANIM_CLIMB;
    if (!strcmp(name, "SwimWait")) return COOP_ANIM_SWIM_WAIT;
    if (!strncmp(name, "SwimMove", 8) || !strcmp(name, "SwimAtnMove") || !strcmp(name, "SwimDash")) return COOP_ANIM_SWIM;
    if (!strcmp(name, "Fall") || !strcmp(name, "SurfingOnFall")) return COOP_ANIM_FALL;
    if (!strcmp(name, "Jump") || !strcmp(name, "BackJump")) return COOP_ANIM_JUMP;
    return COOP_ANIM_NONE;  // Ground movement uses a blended sequence; derive its gait from Link's speed.
}

void rt_coop_observe_as(Cpu *c) {
    if ((uint32_t)c->x[2] != 0 && (uint32_t)c->x[2] != UINT32_MAX) return;
    uint64_t link = player();
    if (!link || !c->x[0] || read_ptr(link + 0x560) != c->x[0]) return;
    uint64_t text = 0;
    char name[48];
    if (!control_mem_peek(c->x[1] + 8, &text, sizeof text) || !text ||
        !control_mem_peek(text, name, sizeof name) || !memchr(name, 0, sizeof name)) return;
    uint64_t state = (link << 8) | (unsigned)classify_as(name);
    atomic_store_explicit(&observed_as, state, memory_order_relaxed);
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
    if (fn == g_site.coop_play_as) c->v[0] = c->v[1] = 0; // AS blend offsets in s0/s1
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

// An optional actor's failure is logged instead of ending the co-op test.
static void fail_or_log(bool optional, const char *why) {
    if (optional) LOG("[coop] PIECE FAIL %s\n", why);
    else fail(why);
}

// Asks for actor name at target. A fresh scratch per request: the game may still reference an abandoned one.
static bool request(Cpu *c, uint64_t creator, uint64_t mgr, const float target[3], const char *name, bool optional) {
    uint64_t heap, accepted;
    if (!call(c, "getHeap", g_site.coop_get_heap, &mgr, 1, &heap)) return false;
    if (!ptr(heap)) { fail("invalid creation heap"); return false; }
    scratch = hle_alloc(sizeof *scratch);
    snprintf(scratch->name, sizeof scratch->name, "%s", name);
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
    if (!(uint8_t)accepted) { fail_or_log(optional, "actor request rejected"); return false; }
    return true;
}

// The pending request: -1 failed (logged), 0 still pending, 1 woken (*actor).
static int wake(Cpu *c, uint64_t frame, uint64_t *actor, bool optional) {
    uint64_t unit = read_ptr((uintptr_t)&scratch->handle.unit);
    uint32_t state = 0;
    if (scratch->handle.failed || (unit && control_mem_peek(unit, &state, sizeof state) && (state == 3 || state == 5))) {
        fail_or_log(optional, "asynchronous actor creation failed");
        return -1;
    }
    if (state != 2) return 0;
    uint64_t ready = read_ptr(unit + 0x10), handle = (uintptr_t)&scratch->handle;
    LOG("[coop] HANDLE ready update=%llu\n", (unsigned long long)frame);
    if (!ready) { fail_or_log(optional, "ready handle has no actor"); return -1; }
    if (!call(c, "releaseAndWakeProc", g_site.coop_release_wake, &handle, 1, actor)) return -1;
    if (*actor != ready || scratch->handle.unit) { fail("wake did not release the expected actor"); return -1; }
    return 1;
}

static void spawn_local(Cpu *c, uint64_t creator, uint64_t mgr, const float xyz[3], uint64_t frame) {
    memcpy(local.origin, xyz, sizeof local.origin);
    memcpy(local.target, xyz, sizeof local.target);
    local.target[0] += 3.0f;
    if (!request(c, creator, mgr, local.target, test_actor, false)) return;
    local.creator = creator;
    local.phase = PENDING;
    local.deadline = frame + 600;
    LOG("[coop] SPAWN update=%llu %s link=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f)\n",
        (unsigned long long)frame, test_actor, xyz[0], xyz[1], xyz[2], local.target[0], local.target[1], local.target[2]);
}

static bool is_named(uint64_t a, uint64_t link, const char *want) {
    char name[sizeof test_actor];
    size_t name_n = strlen(want) + 1;
    return name_n <= sizeof name && a != link && actor_calc(a) && control_mem_peek(a + 0x1c, name, name_n) &&
           !memcmp(name, want, name_n);
}

static bool is_actor(uint64_t a, uint64_t link) { return is_named(a, link, test_actor); }

static const char *piece_name(int i) {
    static char names[PIECES][sizeof test_actor];
    snprintf(names[i], sizeof names[i], "%s%s", test_actor, piece_names[i]);
    return names[i];
}

static double now_ms(void) { return plat_now_ns() / 1.0e6; }

static CoopAnim local_anim(uint64_t link, const float m[12]) {
    if (!anim_bridge) return COOP_ANIM_NONE;
    double now = now_ms(), dt = now - gait.ms;
    if (!gait.ms || dt >= 300) {
        gait.x = m[3]; gait.z = m[11]; gait.ms = now; gait.speed = 0;
    } else if (dt >= 80) {
        gait.speed = hypotf(m[3] - gait.x, m[11] - gait.z) * (float)(1000 / dt);
        gait.x = m[3]; gait.z = m[11]; gait.ms = now;
    }
    uint64_t observed = atomic_load_explicit(&observed_as, memory_order_relaxed);
    CoopAnim anim = observed >> 8 == link ? (CoopAnim)(observed & 0xff) : COOP_ANIM_NONE;
    if (!anim) {
        // A discontinuity (e.g. teleport) isn't a sprint.
        if (gait.speed < 0.30f || gait.speed > 20.0f) anim = COOP_ANIM_IDLE;
        else if (gait.speed < 2.8f) anim = COOP_ANIM_WALK;
        else if (gait.speed < 6.5f) anim = COOP_ANIM_RUN;
        else anim = COOP_ANIM_SPRINT;
    }
    if (anim != gait.last) {
        LOG("[coop] ANIM TX state=%d %s speed=%.2f\n", anim, sequences[anim - COOP_ANIM_IDLE], gait.speed);
        gait.last = anim;
    }
    return anim;
}

// BOTW 1.9.0: bank 0, track 0 owns the body graph; its cells point to the live frame controllers.
typedef struct { uint32_t n, pad; uint64_t data; } AsTable;
typedef struct { uint8_t child, frame, blend, flags; float weight, blend_factor; } AsCell;
typedef struct { uint32_t flags; float current, previous, rate, first, end; } AsFrame;
_Static_assert(sizeof(AsTable) == 16 && sizeof(AsCell) == 12 && sizeof(AsFrame) == 24, "AS ABI");

static uint64_t as_track(uint64_t as) {
    uint64_t banks = as ? read_ptr(as + 0xc0) : 0;
    return banks ? read_ptr(banks + 0x28) : 0;
}

static bool as_sequence(uint64_t track, char *name, size_t size) {
    uint64_t owner = track ? read_ptr(track) : 0, text;
    // AS names can point into the packed string table, not an aligned object.
    return owner && control_mem_peek(owner + 0x10, &text, sizeof text) &&
        control_mem_peek(text, name, size) && memchr(name, 0, size);
}

static bool as_tables(uint64_t track, AsTable table[3]) {
    uint64_t owner = track ? read_ptr(track) : 0;
    uint64_t ctx = owner ? read_ptr(owner + 0xd0) : 0;
    return ctx && control_mem_peek(ctx, table, 3 * sizeof *table) &&
        table[0].n <= 255 && table[1].n <= 255 && table[2].n <= 512 &&
        ptr(table[0].data) && ptr(table[1].data) && ptr(table[2].data);
}

static uint64_t as_frame(const AsTable table[3], const AsCell *cell) {
    return (cell->flags & 9) == 9 && cell->frame < table[1].n ? table[1].data + 32 * cell->frame : 0;
}

static CoopClip local_clip(uint64_t link) {
    CoopClip clip = {0};
    if (!anim_bridge) return clip;
    uint64_t track = as_track(read_ptr(link + 0x560));
    char seq[32];
    if (!as_sequence(track, seq, sizeof seq) ||
        (strcmp(seq, "Wait") && strcmp(seq, "Move") && strcmp(seq, "Turn") &&
         strcmp(seq, "Jump") && strcmp(seq, "Fall") && strcmp(seq, "Land"))) return clip;
    AsTable table[3];
    uint8_t map[512];
    AsCell cells[255];
    if (!as_tables(track, table) || !control_mem_peek(table[2].data, map, table[2].n) ||
        !control_mem_peek(table[0].data, cells, table[0].n * sizeof *cells)) return clip;
    // shortcut: the strongest evaluated skeletal leaf, stream joint poses for exact multi-clip blends and IK.
    float weight = 0;
    for (unsigned i = 0; i < table[2].n; i++) {
        if (map[i] >= table[0].n) continue;
        const AsCell *cell = &cells[map[i]];
        if (!(cell->weight > weight && cell->weight <= 1.001f)) continue;
        uint64_t addr = as_frame(table, cell);
        AsFrame frame;
        if (!addr || !control_mem_peek(addr, &frame, sizeof frame) || !(frame.current >= 0 && frame.current <= 10000))
            continue;
        snprintf(clip.name, sizeof clip.name, "CoopClip_%s_%u", seq, i);
        clip.frame = frame.current;
        weight = cell->weight;
    }
    return clip;
}

static void seek_clip(uint64_t as, const CoopClip *clip) {
    uint64_t track = as_track(as);
    char active[64];
    AsTable table[3];
    uint8_t index;
    AsCell cell;
    AsFrame frame;
    if (!as_sequence(track, active, sizeof active) || strcmp(active, clip->name) || !as_tables(track, table) ||
        !table[2].n || !control_mem_peek(table[2].data, &index, sizeof index) || index >= table[0].n ||
        !control_mem_peek(table[0].data + index * sizeof cell, &cell, sizeof cell)) return;
    uint64_t addr = as_frame(table, &cell);
    if (!addr || !control_mem_peek(addr, &frame, sizeof frame) ||
        !(frame.first >= 0 && frame.first <= frame.end && frame.end <= 10000)) return;
    float at = fminf(frame.end, fmaxf(frame.first, clip->frame));
    control_mem_poke(addr + offsetof(AsFrame, current), (float[]){at, at}, 2 * sizeof at);
}

// The ghost and all worn pieces use the same sampled clip and phase.
static void animate(Cpu *c, CoopAnim anim, const CoopClip *clip) {
    if (!anim_bridge || strcmp(test_actor, "CoopPlayer")) return;
    const char *fallback = sequences[(anim ? anim : COOP_ANIM_IDLE) - COOP_ANIM_IDLE];
    bool exact = !strncmp(clip->name, "CoopClip_", 9);
    const char *name = exact ? clip->name : fallback;
    if (!as_name) {
        uint64_t vt = read_ptr(g_site.coop_string_vtable_slot);
        if (!vt) return;
        as_name = hle_alloc(sizeof *as_name);
        as_name->vtable = vt + 0x10;
        as_name->data = (uintptr_t)as_name->name;
    }
    for (int p = -1; p < PIECES; p++) {
        uint64_t actor = p < 0 ? ghost.actor : ghost.piece[p].actor;
        char *playing = p < 0 ? ghost.playing : ghost.piece[p].playing;
        uint64_t as = actor ? read_ptr(actor + 0x560) : 0;
        if (!as) continue;
        if (strcmp(playing, name)) {
            uint64_t result, args[] = {as, (uintptr_t)as_name, UINT32_MAX, UINT32_MAX, 1};
            strcpy(as_name->name, name);
            if (!call(c, "playAS", g_site.coop_play_as, args, 5, &result)) return;
            if (exact) {
                char active[64] = {0};
                if (!as_sequence(as_track(as), active, sizeof active) || strcmp(active, name)) {
                    if (p < 0) LOG("[coop] CLIP MISS %s\n", name);
                    strcpy(as_name->name, fallback);
                    if (!call(c, "playAS", g_site.coop_play_as, args, 5, &result)) return;
                } else if (p < 0) {
                    LOG("[coop] CLIP RX t=%.1f peer=%u %s frame=%.2f\n", now_ms(), ghost.peer, name, clip->frame);
                }
            } else if (p < 0) {
                LOG("[coop] ANIM RX peer=%u state=%d %s\n", ghost.peer, anim ? anim : COOP_ANIM_IDLE, name);
            }
            strcpy(playing, name);
        }
        if (exact) seek_clip(as, clip);
    }
}

static float yaw_deg(const float m[12]) { return atan2f(m[2], m[10]) * 57.2957795f; }  // heading of local +Z

static void forget_ghost(const char *why) {
    if (ghost.state != GHOST_NONE) LOG("[coop] LOST %s state=%d\n", why, ghost.state);
    ghost.state = GHOST_NONE;  // (a pending handle's scratch is abandoned, never reused)
    ghost.actor = ghost.creator = 0;
    ghost.wrote = false;
    ghost.playing[0] = 0;
    memset(ghost.piece, 0, sizeof ghost.piece);
    ghost.pending = 0;
}

static bool delete_actor(Cpu *c, uint64_t actor) {
    uint64_t args[] = {actor, 0}, ignored;
    return call(c, "deleteLater", g_site.coop_delete_actor, args, 2, &ignored);
}

static bool delete_pieces(Cpu *c) {
    for (int p = 0; p < PIECES; p++) {
        if (ghost.piece[p].actor && !delete_actor(c, ghost.piece[p].actor)) return false;
        ghost.piece[p].actor = 0;
    }
    return true;
}

// One piece at a time once the ghost is alive (the creation scratch is shared). A piece the overlay lacks, or that
// fails, stays off until the ghost is spawned again. With one pending, only finishes it (at unused).
static void spawn_pieces(Cpu *c, uint64_t creator, uint64_t frame, const float at[3]) {
    if (ghost.pending) {
        int p = ghost.pending - 1;
        uint64_t actor;
        int r = wake(c, frame, &actor, true);
        if (!r && frame <= ghost.piece_deadline) return;
        ghost.pending = 0;
        if (r <= 0) {
            ghost.piece[p].off = true;
            if (!r) LOG("[coop] PIECE FAIL %s confirmation timed out\n", piece_name(p));
            return;
        }
        ghost.piece[p].actor = actor;
        if (strncmp(ghost.playing, "CoopClip_", 9)) {  // legacy clips have no shared frame to seek to
            ghost.playing[0] = 0;
            for (int q = 0; q < PIECES; q++) ghost.piece[q].playing[0] = 0;
        }
        LOG("[coop] PIECE ALIVE update=%llu %s\n", (unsigned long long)frame, piece_name(p));
        return;
    }
    uint64_t mgr = read_ptr(g_site.coop_heap_mgr);
    for (int p = 0; p < PIECES && mgr; p++) {
        if (ghost.piece[p].actor || ghost.piece[p].off) continue;
        if (!request(c, creator, mgr, at, piece_name(p), true)) { ghost.piece[p].off = true; return; }
        ghost.pending = p + 1;
        ghost.piece_deadline = frame + 600;
        return;
    }
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
        // Batched arrivals need distinct receive stamps so interpolation can traverse their samples.
        double ms = ghost.n ? fmax(now, ghost.ring[ghost.n - 1].ms + MIN_GAP_MS) : now;
        ghost.ring[ghost.n].ms = fmin(ms, now + DELAY_MS / 2);
        ghost.ring[ghost.n].anim = e.anim;
        ghost.ring[ghost.n].clip = e.clip;
        memcpy(ghost.ring[ghost.n++].m, e.m, sizeof e.m);
        ghost.last_ms = now;
        ghost.gone = false;
    }
    if (ghost.n && now - ghost.last_ms > STALE_MS) ghost.gone = true;
}

// Rendered DELAY_MS behind the newest pose: translation and rotation interpolated, no extrapolation.
// Rotation: the turn from p to q (q = R p) taken at constant speed, f of its angle about its axis; a ~180° turn
// between samples has no defined axis and keeps the newer.
static bool blend_rotation(float out[12], const float *p, const float *q, double f) {
    double r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i][j] = q[i * 4] * p[j * 4] + q[i * 4 + 1] * p[j * 4 + 1] + q[i * 4 + 2] * p[j * 4 + 2];
    double u[3] = {r[2][1] - r[1][2], r[0][2] - r[2][0], r[1][0] - r[0][1]};
    double s = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) / 2, c = (r[0][0] + r[1][1] + r[2][2] - 1) / 2;
    if (s < 1e-4 && c < 0) return false;
    double a = f * atan2(s, c), sa = sin(a), ca = cos(a);
    for (int k = 0; k < 3; k++) u[k] = s < 1e-4 ? 0 : u[k] / (2 * s);
    const double x[3][3] = {{0, -u[2], u[1]}, {u[2], 0, -u[0]}, {-u[1], u[0], 0}};  // [u]x
    for (int i = 0; i < 3; i++)  // Rodrigues: (cos a I + sin a [u]x + (1 - cos a) u u^T) p
        for (int j = 0; j < 3; j++) {
            double v = 0;
            for (int k = 0; k < 3; k++) v += ((i == k) * ca + sa * x[i][k] + (1 - ca) * u[i] * u[k]) * p[k * 4 + j];
            out[i * 4 + j] = (float)v;
        }
    return true;
}

static CoopAnim target_pose(float out[12], CoopClip *clip) {
    double t = now_ms() - DELAY_MS;
    memcpy(out, ghost.ring[ghost.n - 1].m, 12 * sizeof *out);
    unsigned i = ghost.n - 1;
    while (i && ghost.ring[i - 1].ms > t) i--;
    if (!i || ghost.ring[i].ms <= t) {
        memcpy(out, ghost.ring[i].m, 12 * sizeof *out);
        *clip = ghost.ring[i].clip;
        return ghost.ring[i].anim;
    }
    double a = ghost.ring[i - 1].ms, b = ghost.ring[i].ms, f = b > a ? (t - a) / (b - a) : 1;
    if (!blend_rotation(out, ghost.ring[i - 1].m, ghost.ring[i].m, f)) memcpy(out, ghost.ring[i].m, 12 * sizeof *out);
    for (int k = 3; k < 12; k += 4)
        out[k] = (float)(ghost.ring[i - 1].m[k] + f * (ghost.ring[i].m[k] - ghost.ring[i - 1].m[k]));
    *clip = ghost.ring[i - 1].clip;
    const CoopClip *next = &ghost.ring[i].clip;
    if (clip->name[0] && !strcmp(clip->name, next->name) && next->frame >= clip->frame)
        clip->frame += (float)f * (next->frame - clip->frame);
    return ghost.ring[i - 1].anim;
}

// One remote ghost, on the game thread: spawn near Link, place it every update, remove it when the peer goes.
static void net_update(Cpu *c) {
    drain();
    uint64_t frame = framepace_game_frame();
    if (local.phase == FINISHED) return;
    if (framepace_title_ui() || framepace_loading_ui()) {
        forget_ghost("world exit");
        atomic_store_explicit(&observed_as, 0, memory_order_relaxed);
        gait.ms = 0;
        return;
    }
    if (framepace_paused()) return;
    uint64_t link = player();
    float m[12];
    if (!actor_calc(link) || !pose(link, m)) return;
    if (link != ghost.link) { forget_ghost("Link changed"); ghost.link = link; gait.ms = 0; }
    float xyz[3] = {m[3], m[7], m[11]};
    CoopAnim anim = local_anim(link, m);
    CoopClip clip = local_clip(link);
    if (strcmp(gait.last_clip, clip.name)) {
        if (clip.name[0]) LOG("[coop] CLIP TX t=%.1f %s frame=%.2f\n", now_ms(), clip.name, clip.frame);
        strcpy(gait.last_clip, clip.name);
    }
    coop_net_push(m, anim, &clip);
    uint64_t creator = read_ptr(g_site.coop_actor_creator);
    if (ghost.state != GHOST_NONE && creator != ghost.creator) { forget_ghost("ActorCreator changed"); return; }
    bool log = ++ghost.updates % LOG_EVERY == 0;

    if (ghost.state == GHOST_ALIVE) {
        if (!is_actor(ghost.actor, link) || (ghost.updates % 30 == 0 && !listed(creator, ghost.actor))) {
            if (ghost.pending) { spawn_pieces(c, creator, frame, NULL); return; }  // (no piece left behind)
            if (!delete_pieces(c)) return;
            forget_ghost("ghost no longer listed/Calc");
            return;
        }
        for (int p = 0; p < PIECES; p++) {
            if (ghost.piece[p].actor && !is_named(ghost.piece[p].actor, link, piece_name(p))) {
                LOG("[coop] PIECE LOST %s\n", piece_name(p));
                ghost.piece[p].actor = 0;
                ghost.piece[p].off = true;
            }
        }
        float back[12];
        if (ghost.wrote && pose(ghost.actor, back)) {  // what the game made of last update's write
            float d[3] = {back[3] - ghost.put[3], back[7] - ghost.put[7], back[11] - ghost.put[11]};
            float e = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
            if (e > ghost.err_max) ghost.err_max = e;
        }
        if (ghost.gone) {
            if (ghost.pending) { spawn_pieces(c, creator, frame, NULL); return; }
            if (!delete_pieces(c) || !delete_actor(c, ghost.actor)) return;
            LOG("[coop] REMOVE update=%llu peer=%u silent_ms=%.0f\n", (unsigned long long)frame, ghost.peer,
                now_ms() - ghost.last_ms);
            forget_ghost("removed");
            ghost.n = 0;
            return;
        }
    }
    if (ghost.state == GHOST_PENDING) {
        int r = wake(c, frame, &ghost.actor, false);
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
            LOG("[coop] TRACK t=%.1f update=%llu link=(%.3f,%.3f,%.3f) link_yaw=%.2f tx=%u rx=%u ghost=none\n", now_ms(),
                (unsigned long long)frame, xyz[0], xyz[1], xyz[2], yaw_deg(m), tx, rx);
        }
        return;
    }
    float want[12];
    anim = target_pose(want, &clip);
    float at[3] = {want[3], want[7], want[11]};
    if (ghost.state == GHOST_NONE) {
        uint64_t mgr = read_ptr(g_site.coop_heap_mgr);
        if (distance(at, xyz) >= RANGE_M || framepace_in_event() || !mgr || !listed(creator, link)) return;
        if (ghost.spawns == MAX_SPAWNS) { fail("ghost spawn limit reached"); return; }
        if (!request(c, creator, mgr, at, test_actor, false)) return;
        ghost.spawns++;
        ghost.state = GHOST_PENDING;
        ghost.creator = creator;
        ghost.deadline = frame + 600;
        memcpy(ghost.link_at_spawn, xyz, sizeof xyz);
        LOG("[coop] SPAWN update=%llu %s peer=%u target=(%.3f,%.3f,%.3f) link=(%.3f,%.3f,%.3f)\n",
            (unsigned long long)frame, test_actor, ghost.peer, at[0], at[1], at[2], xyz[0], xyz[1], xyz[2]);
        return;
    }
    if (ghost.state != GHOST_ALIVE || !is_actor(ghost.actor, link)) return;
    animate(c, anim, &clip);
    float yaw = yaw_deg(want), step = fabsf(remainderf(yaw - ghost.yaw, 360.0f));
    double ms = now_ms();
    if (ghost.wrote) {
        ghost.yaw_steps++;
        ghost.yaw_still += step < 0.01f;
        if (step > ghost.yaw_step_max) ghost.yaw_step_max = step;
        if (ms > ghost.yaw_ms && step * 1000 / (ms - ghost.yaw_ms) > ghost.yaw_rate_max)
            ghost.yaw_rate_max = (float)(step * 1000 / (ms - ghost.yaw_ms));
    }
    ghost.yaw = yaw;
    ghost.yaw_ms = ms;
    ghost.wrote = control_mem_poke(ghost.actor + 0x398, want, sizeof want);
    for (int p = 0; p < PIECES; p++)
        if (ghost.piece[p].actor) control_mem_poke(ghost.piece[p].actor + 0x398, want, sizeof want);
    memcpy(ghost.put, want, sizeof want);
    if (anim_bridge && !strcmp(test_actor, "CoopPlayer")) spawn_pieces(c, creator, frame, at);
    if (log) {
        unsigned tx, rx;
        coop_net_counts(&tx, &rx);
        const float *r = ghost.ring[ghost.n - 1].m;
        LOG("[coop] TRACK t=%.1f update=%llu link=(%.3f,%.3f,%.3f) link_yaw=%.2f tx=%u rx=%u remote=(%.3f,%.3f,%.3f) "
            "ghost=(%.3f,%.3f,%.3f) readback_max=%.4f written=%d remote_yaw=%.2f ghost_yaw=%.2f yaw_step_max=%.3f "
            "yaw_rate_max=%.0f yaw_still=%u/%u\n", now_ms(), (unsigned long long)frame, xyz[0], xyz[1], xyz[2],
            yaw_deg(m), tx, rx, r[3], r[7], r[11], at[0], at[1], at[2], ghost.err_max, ghost.wrote, yaw_deg(r), yaw,
            ghost.yaw_step_max, ghost.yaw_rate_max, ghost.yaw_still, ghost.yaw_steps);
        ghost.err_max = ghost.yaw_step_max = ghost.yaw_rate_max = 0;
        ghost.yaw_steps = ghost.yaw_still = 0;
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
        if (wake(c, frame, &local.actor, false) == 1) local.phase = WOKEN;
        return;
    }
    if (!is_actor(local.actor, link) || !listed(creator, local.actor) || !pose(local.actor, m)) return;
    float actual[] = {m[3], m[7], m[11]};
    LOG("[coop] ALIVE update=%llu %s listed=1 state=Calc actor=(%.3f,%.3f,%.3f) distance=%.3f target_error=%.3f "
        "link=(%.3f,%.3f,%.3f) link_delta=%.3f\n", (unsigned long long)frame, test_actor, actual[0], actual[1], actual[2],
        distance(actual, xyz), distance(actual, local.target), xyz[0], xyz[1], xyz[2], distance(xyz, local.origin));
    local.phase = FINISHED;
}
