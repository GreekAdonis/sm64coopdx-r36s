#include <math.h>
#include <string.h>

#include "engine/math_util.h"
#include "geo_dl_bounds.h"

// Walk limits. A static level display list is a few hundred commands; these are
// set well above anything real so that hitting one means the walk has gone wrong
// -- a malformed list, or one whose encoding this does not model -- and the
// right answer is to give up and let the caller draw it.
#define DL_BOUNDS_MAX_COMMANDS 8192
#define DL_BOUNDS_MAX_DEPTH    12

// Open-addressed, power-of-two, never resized. Run 23's busiest frame appended
// 899 display-list nodes across 218 distinct pointers, so this sits far below
// half load. Past the fill limit new lists are simply reported unbounded, which
// costs performance and nothing else.
#define DL_BOUNDS_SLOTS 2048
#define DL_BOUNDS_MAX   1024

struct DlBoundsEntry {
    const void *displayList;
    u64  checksum;     // first commands of the list, to catch a reused address
    Vec3f center;
    f32  radius;
    bool bounded;      // false: walked it and could not trust the result
};

static struct DlBoundsEntry sEntries[DL_BOUNDS_SLOTS];
static u32 sEntryCount = 0;

void geo_dl_bounds_reset(void) {
    memset(sEntries, 0, sizeof(sEntries));
    sEntryCount = 0;
}

// Cheap identity check on the contents, so that a display list freed by a pack
// unload and its address handed back for something else does not inherit the old
// box. Four commands is enough to separate unrelated lists in practice; the
// pointer is still the primary key, this only guards reuse.
//
// Stops at G_ENDDL rather than always reading four commands: a list can be
// shorter than that, and walking off the end of one to hash bytes that are not
// ours is exactly the kind of read this function exists to make safe.
static u64 dl_checksum(const Gfx *dl) {
    u64 h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 4; i++) {
        h ^= (u64)dl[i].words.w0;
        h *= 0x100000001b3ULL;
        h ^= (u64)dl[i].words.w1;
        h *= 0x100000001b3ULL;
        if ((u8)(dl[i].words.w0 >> 24) == (u8)G_ENDDL) { break; }
    }
    return h;
}

struct DlBoundsWalk {
    Vec3f min;
    Vec3f max;
    u32  commands;
    u32  vertices;
    bool ok;
};

#define W0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << (width)) - 1))

static void dl_bounds_walk(const Gfx *cmd, struct DlBoundsWalk *w, int depth) {
    if (cmd == NULL || depth > DL_BOUNDS_MAX_DEPTH) { w->ok = false; return; }

    for (;;) {
        if (++w->commands > DL_BOUNDS_MAX_COMMANDS) { w->ok = false; return; }

        const u32 opcode = cmd->words.w0 >> 24;

        switch (opcode) {
            case G_VTX: {
#ifdef F3DEX_GBI_2
                const size_t n = W0(12, 8);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                const size_t n = W0(10, 6);
#else
                const size_t n = W0(0, 16) / sizeof(Vtx);
#endif
                const Vtx *v = (const Vtx *)(uintptr_t)cmd->words.w1;
                if (v == NULL) { w->ok = false; return; }
                for (size_t i = 0; i < n; i++) {
                    const f32 *ob = v[i].v.ob;
                    for (int a = 0; a < 3; a++) {
                        if (ob[a] < w->min[a]) { w->min[a] = ob[a]; }
                        if (ob[a] > w->max[a]) { w->max[a] = ob[a]; }
                    }
                }
                w->vertices += (u32)n;
                break;
            }

            case G_DL:
                // Bit 16 of w0 distinguishes a call from a tail jump, the same
                // way gfx_run_dl() reads it.
                if (W0(16, 1) == 0) {
                    dl_bounds_walk((const Gfx *)(uintptr_t)cmd->words.w1, w, depth + 1);
                    if (!w->ok) { return; }
                } else {
                    cmd = (const Gfx *)(uintptr_t)cmd->words.w1;
                    if (cmd == NULL) { w->ok = false; return; }
                    continue;   // jumped, do not advance
                }
                break;

            case (u8)G_ENDDL:
                return;

            // Anything that can put geometry on screen without the vertices
            // passing through G_VTX above would make the box a lie. Rather than
            // enumerate every such command across four GBI variants, refuse the
            // two that exist and accept everything else as state-only.
#ifdef F3DEX_GBI_2
            case (u8)G_MODIFYVTX:
            case (u8)G_BRANCH_Z:
                w->ok = false;
                return;
#endif

            default:
                // Every remaining command is a fixed-size state change in all
                // the GBI variants this builds for, so stepping over it is safe.
                break;
        }

        cmd++;
    }
}

static struct DlBoundsEntry *dl_bounds_slot(const void *displayList) {
    // Display lists are at least 8-byte aligned, so the low bits carry nothing.
    u64 h = (u64)(uintptr_t)displayList >> 3;
    h *= 0x9E3779B97F4A7C15ULL;
    size_t i = (size_t)(h >> 48) & (DL_BOUNDS_SLOTS - 1);

    while (sEntries[i].displayList != NULL) {
        if (sEntries[i].displayList == displayList) { return &sEntries[i]; }
        i = (i + 1) & (DL_BOUNDS_SLOTS - 1);
    }
    return &sEntries[i];   // free slot
}

bool geo_dl_bounds_get(const void *displayList, Vec3f outCenter, f32 *outRadius) {
    if (displayList == NULL) { return false; }

    struct DlBoundsEntry *e = dl_bounds_slot(displayList);
    const u64 sum = dl_checksum((const Gfx *)displayList);

    if (e->displayList == displayList) {
        if (e->checksum == sum) {
            if (!e->bounded) { return false; }
            vec3f_copy(outCenter, e->center);
            *outRadius = e->radius;
            return true;
        }
        // Same address, different contents. Recompute in place.
    } else {
        if (sEntryCount >= DL_BOUNDS_MAX) { return false; }
        sEntryCount++;
    }

    struct DlBoundsWalk w;
    vec3f_set(w.min,  1.0e18f,  1.0e18f,  1.0e18f);
    vec3f_set(w.max, -1.0e18f, -1.0e18f, -1.0e18f);
    w.commands = 0;
    w.vertices = 0;
    w.ok = true;

    dl_bounds_walk((const Gfx *)displayList, &w, 0);

    e->displayList = displayList;
    e->checksum = sum;
    // A list with no vertices of its own draws nothing, but it may still be the
    // state-only prologue to something that does, so it is not safe to treat an
    // empty box as "cull me".
    e->bounded = w.ok && w.vertices > 0;

    if (!e->bounded) { return false; }

    for (int a = 0; a < 3; a++) {
        e->center[a] = (w.min[a] + w.max[a]) * 0.5f;
    }
    const f32 ex = (w.max[0] - w.min[0]) * 0.5f;
    const f32 ey = (w.max[1] - w.min[1]) * 0.5f;
    const f32 ez = (w.max[2] - w.min[2]) * 0.5f;
    e->radius = sqrtf(ex * ex + ey * ey + ez * ez);

    vec3f_copy(outCenter, e->center);
    *outRadius = e->radius;
    return true;
}
