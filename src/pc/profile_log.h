#pragma once

#include <PR/ultratypes.h>
#include <stdbool.h>

// Per-frame profiling instrumentation, compiled in only with PROFILE=1.
//
// A profile build writes one CSV row per game frame (times in microseconds,
// counters absolute for that frame) so a laggy play session can be analysed
// afterwards instead of guessed at. See docs/PROFILING-RK3326.md.

#ifdef PROFILE_BUILD

struct ProfileCounters {
    u32 drawCalls;       // gfx_flush() -> rapi->draw_triangles()
    u32 tris;            // triangles in those draw calls
    u32 verts;           // vertices pushed through gfx_sp_vertex()
    u32 texLoads;        // texture cache misses (i.e. real GL uploads)
    u32 texBytes;        // bytes handed to glTexImage2D
    u32 texCacheFlushes; // whole-cache invalidations (pool ran full)
    u32 texBinds;        // real glBindTexture calls
    u32 texBindSkips;    // binds skipped by the bind cache
    u32 shaderLoads;     // shader program switches
    u32 subFrames;       // rendered frames produced for this game frame
};

extern struct ProfileCounters gProfileCounters;

#define PROFILE_ADD(_field, _n) (gProfileCounters._field += (u32)(_n))

void profile_log_init(void);
void profile_log_frame(void);
void profile_log_shutdown(void);

#else

#define PROFILE_ADD(_field, _n) ((void)0)
#define profile_log_init()      ((void)0)
#define profile_log_frame()     ((void)0)
#define profile_log_shutdown()  ((void)0)

#endif
