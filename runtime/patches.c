// Ark: optional game patches, applied to guest memory after loading (Legends: Arceus 1.1.1 only; each checks the
// original value first). Recompiled code reads data from guest memory, so data patches work as on the console;
// code is compiled, so code changes are made where the runtime sees them (nvnWindowSetPresentInterval).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

// ARK_60FPS=1: 60 fps at normal game speed. The game steps its simulation by a fixed frame time in nanoseconds
// (main+0x397E8D8, 33,333,334 = 1/30 s, in a table of frame times for present intervals 1..5; a second copy at
// main+0x39848E0, below) and asks for present interval 2 at main+0x2AA27A8 (skipped there, see
// objects.c). ARK_60FPS=2: 60 fps with the 1/30 s step kept (the game runs twice as fast).
int g_ark_60fps;
static int sites_mask = 15 | 64;

// Run-time alternatives in the translated code (tools/lifter/variants.py): 1/30 s fixed steps in code, 1/60 s at 60 fps.
// ARK_60FPS_SITES=<mask> picks the groups (default all; 0 none), to narrow down a group that misbehaves.
static int force_lod0, grass_far;
int ark_variant(int group) {
    if (group == 16) return force_lod0;
    if (group == 17) return grass_far;
    if (group == 64 || group == 65) {  // text printer: skip every other presented frame (65 repeats 64's choice)
        static __thread int last;
        extern uint64_t renderer_present_count(void);
        if (group == 64) last = g_ark_60fps == 1 && (sites_mask & 64) && (renderer_present_count() & 1);
        return last;
    }
    return g_ark_60fps == 1 && (sites_mask & group) != 0;
}

void game_patches_apply(void) {
    // ARK_LOD=3 (or ARK_LOD0=1): every model at its full-detail level (tools/lifter/variants.py group 16)
    force_lod0 = (getenv("ARK_LOD") && atoi(getenv("ARK_LOD")) >= 3) || (getenv("ARK_LOD0") && atoi(getenv("ARK_LOD0")) != 0);
    if (getenv("ARK_LOD0") && atoi(getenv("ARK_LOD0")) == 0) force_lod0 = 0;
    if (force_lod0) LOG("[patch] models at full detail (no level of detail)\n");
    grass_far = force_lod0;
    const char *e = getenv("ARK_60FPS");
    g_ark_60fps = e ? atoi(e) : 0;
    if (!g_ark_60fps) return;
    if (getenv("ARK_60FPS_SITES")) sites_mask = (int)strtol(getenv("ARK_60FPS_SITES"), NULL, 0);
    uint64_t *step = (uint64_t *)(uintptr_t)(0x7000000000ull + 0x397E8D8);
    if (*step != 33333334ull) {
        LOG("[patch] 60 fps: unexpected frame step %llu (not version 1.1.1?): not applied\n", (unsigned long long)*step);
        g_ark_60fps = 0;
        return;
    }
    // The second copy of the table (main+0x39848E0) is read in one place, main+0x2B43450: game-frame steps =
    // measured frame time / table[interval]. The measured time already follows the first table, so this one stays
    // at 1/30 s (0.5 steps a frame at 60 fps); halving it too ran those animations at double speed.
    // ARK_60FPS_TABLE2=1 halves it anyway (for comparison).
    uint64_t *step2 = (uint64_t *)(uintptr_t)(0x7000000000ull + 0x39848E8);
    if (g_ark_60fps == 1) {
        *step = 16666667ull;
        if (getenv("ARK_60FPS_TABLE2") && atoi(getenv("ARK_60FPS_TABLE2")) && *step2 == 33333334ull) *step2 = 16666667ull;
    }
    LOG("[patch] 60 fps (%s game speed%s)\n", g_ark_60fps == 1 ? "normal" : "double",
        g_ark_60fps == 1 && sites_mask != (15 | 64) ? ", some fixed-step code sites left at 1/30 s" : "");
}

static const char *dpc0(void) { return getenv("ARK_PROBE_DUMP_PC"); }

// Debug: instructions named in ARK_GEN_PROBES when the code was generated call this first. From presented frame
// ARK_PROBE_AT (default 1), the first ARK_PROBE_N (default 6) visits of each are logged with the registers.
void ark_probe(Cpu *c, uint64_t pc) {
    extern uint64_t renderer_present_count(void);
    static long at = -1, maxn;
    static struct { uint64_t pc, lr; long n; } seen[512];
    static int nseen, bylr = -1;
    if (bylr < 0) bylr = getenv("ARK_PROBE_BYLR") && atoi(getenv("ARK_PROBE_BYLR"));  // count visits per caller
    uint64_t lr = bylr ? c->x[30] : 0;
    if (at < 0) {
        at = getenv("ARK_PROBE_AT") ? atol(getenv("ARK_PROBE_AT")) : 1;
        maxn = getenv("ARK_PROBE_N") ? atol(getenv("ARK_PROBE_N")) : 6;
    }
    if ((long)renderer_present_count() < at) return;
    int i = 0;
    while (i < nseen && (seen[i].pc != pc || seen[i].lr != lr)) i++;
    if (i == nseen) { if (nseen == 512) return; seen[nseen].pc = pc, seen[nseen++].lr = lr; }
    if (seen[i].n++ >= maxn) return;
    double d[4];
    float f[4];
    for (int k = 0; k < 4; k++) { uint64_t lo = (uint64_t)c->v[k]; memcpy(&d[k], &lo, 8); memcpy(&f[k], &lo, 4); }
    {   // ARK_PROBE_WATCH=<reg>:<hex off>: watch x<reg>+off of the first ARK_PROBE_WATCH_N (8) visits, then arm
        static int nw;
        const char *pw = getenv("ARK_PROBE_WATCH");
        int maxw = getenv("ARK_PROBE_WATCH_N") ? atoi(getenv("ARK_PROBE_WATCH_N")) : 8;
        if (pw && nw < maxw && (!dpc0() || strtoull(dpc0(), NULL, 16) == pc)) {
            extern void watch_add(uint64_t, uint64_t), watch_arm(void);
            char *e;
            int r = (int)strtol(pw, &e, 10);
            uint64_t a = c->x[r & 31] + (*e == ':' ? strtoull(e + 1, NULL, 16) : 0);
            watch_add(a, a + 4);
            if (++nw == maxw) watch_arm();
        }
    }
    LOG("[probe] %llx frame %llu x0 %llx x1 %llx x2 %llx x3 %llx x8 %llx x9 %llx x19 %llx x20 %llx lr %llx | v0 %llx (%g) v1 %llx v2 %llx v3 %llx v4 %llx\n",
        (unsigned long long)pc, (unsigned long long)renderer_present_count(), (unsigned long long)c->x[0],
        (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[8], (unsigned long long)c->x[9],
        (unsigned long long)c->x[19], (unsigned long long)c->x[20], (unsigned long long)c->x[30],
        (unsigned long long)(uint64_t)c->v[0], d[0], (unsigned long long)(uint64_t)c->v[1],
        (unsigned long long)(uint64_t)c->v[2], (unsigned long long)(uint64_t)c->v[3],
        (unsigned long long)(uint64_t)c->v[4]);
    // ARK_PROBE_DUMP=<reg>:<hex off>:<hex len>,...: also the memory at x<reg>+off, as floats and words
    const char *dump = getenv("ARK_PROBE_DUMP"), *dpc = getenv("ARK_PROBE_DUMP_PC");  // (only at that pc, if set)
    if (dpc && strtoull(dpc, NULL, 16) != pc) dump = NULL;
    for (const char *p = dump; p && *p;) {
        char *e;
        int r = (int)strtol(p, &e, 10);
        if (*e != ':') break;
        long off = strtol(e + 1, &e, 16), len = 16;
        if (*e == ':') len = strtol(e + 1, &e, 16);
        uint64_t base = c->x[r & 31] + off;
        LOG("[probe]   x%d+%#lx:", r, off);
        for (long k = 0; k + 4 <= len; k += 4) {
            uint32_t w;
            float f;
            memcpy(&w, (const void *)(uintptr_t)(base + k), 4);
            memcpy(&f, &w, 4);
            LOG(" %x(%g)", w, f);
        }
        LOG("\n");
        p = *e == ',' ? e + 1 : e;
    }
}

// ---- terrain detail range (tools/lifter/patches.py, main+0x24713C4): the distance within which terrain patches are
// split into finer ones (40 m) times this factor. ARK_TERRAIN_RANGE=<f> (default 4 with ARK_LOD=3, else 1); F7 cycles it.
static float terrain_factor = -1;
static void terrain_factor_init(void) {
    if (terrain_factor >= 0) return;
    const char *e = getenv("ARK_TERRAIN_RANGE");
    terrain_factor = e ? (float)atof(e) : (getenv("ARK_LOD") && atoi(getenv("ARK_LOD")) >= 3) ? 4.0f : 1.0f;
    if (terrain_factor < 0.25f) terrain_factor = 0.25f;
    if (terrain_factor > 16) terrain_factor = 16;
}
void ark_terrain_range(Cpu *c) {
    terrain_factor_init();
    uint32_t w = (uint32_t)c->v[5];
    float f;
    memcpy(&f, &w, 4);
    f *= terrain_factor;
    memcpy(&w, &f, 4);
    c->v[5] = w;
}
float ark_terrain_factor(void) { terrain_factor_init(); return terrain_factor; }

// ---- run-time switches (runtime/hotkeys.c: F2..F5 in the game window; the title bar shows them)
static int table2_half;
static bool frame_tables_ok(void) {
    uint64_t v = *(uint64_t *)(uintptr_t)(0x7000000000ull + 0x397E8D8);
    return v == 33333334ull || v == 16666667ull;
}
int ark_switch_get(int which) {
    switch (which) {
    case 0: return g_ark_60fps == 1;
    case 1: return force_lod0;
    case 2: return grass_far;
    case 3: return (sites_mask & 64) != 0;
    case 4: return table2_half || (getenv("ARK_60FPS_TABLE2") && atoi(getenv("ARK_60FPS_TABLE2")));
    }
    return 0;
}
void ark_switch_toggle(int which) {
    extern void nvn_apply_60fps_interval(int on);
    uint64_t *step = (uint64_t *)(uintptr_t)(0x7000000000ull + 0x397E8D8), *step2 = step + (0x39848E8 - 0x397E8D8) / 8;
    switch (which) {
    case 0:
        if (!frame_tables_ok()) { LOG("[switch] 60 fps: not version 1.1.1's frame table\n"); return; }
        g_ark_60fps = g_ark_60fps == 1 ? 0 : 1;
        *step = g_ark_60fps ? 16666667ull : 33333334ull;
        if (!g_ark_60fps) *step2 = 33333334ull, table2_half = 0;
        nvn_apply_60fps_interval(g_ark_60fps);
        break;
    case 1: force_lod0 = !force_lod0; break;
    case 2: grass_far = !grass_far; break;
    case 3: sites_mask ^= 64; break;
    case 5: {
        terrain_factor_init();
        static const float steps[] = {1, 2, 4, 6};
        int i = 0;
        while (i < 4 && steps[i] <= terrain_factor + 0.01f) i++;
        terrain_factor = steps[i % 4];
        LOG("[switch] terrain detail range x%g (%g m)\n", terrain_factor, 40 * terrain_factor);
        return;
    }
    case 4:
        table2_half = !ark_switch_get(4);
        *step2 = table2_half && g_ark_60fps ? 16666667ull : 33333334ull;
        break;
    }
    static const char *names[] = {"60 fps", "full-detail models", "grass to its limit", "text at 30 fps pace", "second frame table halved"};
    if (which >= 0 && which < 5) LOG("[switch] %s: %s\n", names[which], ark_switch_get(which) ? "on" : "off");
}

// Ark: game speed for testing. The game advances one fixed step per presented frame, paced by Ark at the present
// interval (nvn/objects.c present_pace) and by the 60 Hz vsync timer (hle/vi.c); both run ark_speed() times faster.
// 0: no pacing (as fast as the machine runs it; the vsync timer at 16x). ARK_SPEED=1|2|4|0 at start; F8 in the window
// cycles x1 -> x2 -> x4 -> unlimited. Audio follows the game, so it may stutter or change pitch above x1.
static int speed = -1;
int ark_speed(void) {
    if (speed < 0) {
        const char *e = getenv("ARK_SPEED");
        speed = e ? atoi(e) : 1;
        if (speed < 0) speed = 1;
        if (speed > 16) speed = 16;
        if (speed != 1) LOG("[speed] game speed %s (ARK_SPEED)\n", speed ? "raised" : "unlimited");
    }
    return speed;
}
void ark_speed_cycle(void) {
    int s = ark_speed();
    speed = s == 1 ? 2 : s == 2 ? 4 : s == 4 ? 0 : 1;
    if (speed) LOG("[speed] game speed x%d\n", speed);
    else LOG("[speed] game speed: unlimited\n");
}
