#pragma once

#include <PR/ultratypes.h>
#include <stdbool.h>

// Context timing is compiled in for development builds and for PROFILE=1
// builds (see src/pc/profile_log.c), which are otherwise identical to a
// release build so the numbers describe the shipping binary.
#if defined(DEVELOPMENT) || defined(PROFILE_BUILD)
#define CTX_TIMING_ENABLED 1
#endif

#define CTX_BEGIN(_ctx) debug_context_begin(_ctx)
#define CTX_END(_ctx) debug_context_end(_ctx)
#define CTX_WITHIN(_ctx) debug_context_within(_ctx)
#define CTX_TIME(_ctx, time) debug_context_set_time(_ctx, time)
#define CTX_EXTENT(_ctx, _f) { CTX_BEGIN(_ctx); _f(); CTX_END(_ctx); }

// Instrumentation that only exists when the timers do, so a release build is
// left exactly as it was (no depth counting, no clock reads, no calls).
#ifdef CTX_TIMING_ENABLED
#define CTX_BEGIN_TIMED(_ctx) debug_context_begin(_ctx)
#define CTX_END_TIMED(_ctx)   debug_context_end(_ctx)
#else
#define CTX_BEGIN_TIMED(_ctx) ((void)0)
#define CTX_END_TIMED(_ctx)   ((void)0)
#endif

enum DebugContext {
    CTX_NONE,
    CTX_TOTAL,
    CTX_NETWORK,
    CTX_INTERP,
    CTX_GAME_LOOP,
    CTX_SMLUA,
    CTX_AUDIO,
    CTX_RENDER,
    CTX_LEVEL_SCRIPT,
    CTX_HOOK,
    CTX_LIGHTING,
    CTX_OBJECTS,     // update_objects(): behaviour scripts + object collision
    CTX_GEO,         // geo_process_root(): scene graph walk that builds the DL
    CTX_GFX_DL,      // gfx_run()/gfx_end_frame_render(): DL interpretation + GL calls
    CTX_TEXUPLOAD,   // rapi->upload_texture(): CPU repack + glTexImage2D
    CTX_SWAP,        // gfx_display_frame(): SwapWindow, where a GPU-bound frame stalls
    // Network work that has no dependency on game state and could therefore run
    // on another core. Whatever CTX_NETWORK holds beyond these two is coupled to
    // the object pool and the player list and has to stay on the main thread, so
    // the pair sets a ceiling on what threading the network could actually buy.
    CTX_NET_CODEC,   // zlib compress/decompress of packet payloads
    CTX_NET_SOCKET,  // the send syscall itself
    CTX_DELAY,       // frame-cap sleep/busy-wait; idle time, not work
    CTX_MAX,
    // MUST BE KEPT IN SYNC WITH sDebugContextNames
};

void debug_context_begin(enum DebugContext ctx);
void debug_context_end(enum DebugContext ctx);
void debug_context_reset(void);
bool debug_context_within(enum DebugContext ctx);
void debug_context_set_time(enum DebugContext ctx, f64 time);
f64 debug_context_get_time(enum DebugContext ctx);