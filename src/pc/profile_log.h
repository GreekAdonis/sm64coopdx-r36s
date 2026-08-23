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

    // Which state change forced each batch split, counted only when the vertex
    // buffer actually had geometry to emit. These sum to drawCalls (minus the
    // end-of-frame and HUD-pass flushes) and say where a frame's draw calls
    // come from, which "draws" alone cannot.
    u32 flushDepth;      // depth test, depth mask or decal mode
    u32 flushViewport;   // viewport or scissor rect
    u32 flushShader;     // shader program switch
    u32 flushAlpha;      // alpha blending toggled
    u32 flushTexture;    // a tile was re-imported
    u32 flushSampler;    // filter/clamp/mirror parameters
    u32 flushBufferFull; // hit MAX_BUFFERED triangles
    u32 flushCombiner;   // a new colour combiner had to be built
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
