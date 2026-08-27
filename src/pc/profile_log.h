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

    // Triangles thrown away in gfx_sp_tri1() after their vertices had already
    // been transformed and lit. `tris` alone cannot show this: run 10's gore room
    // transformed 268,000 vertices to draw 2,132 triangles, and the only way to
    // see where the other 99% went was to infer it from the ratio. Split by
    // reason, because the fixes differ -- clip rejects want a tighter object cull
    // in front of the renderer, cull rejects are the model's own backfaces and
    // are working as intended.
    u32 trisClipRejected;  // whole triangle outside the view volume
    u32 trisCullRejected;  // backface/frontface culled
    u32 texLoads;        // texture cache misses (i.e. real GL uploads)
    u32 texBytes;        // bytes handed to glTexImage2D
    u32 texCacheFlushes; // whole-cache invalidations (pool ran full)
    u32 texBinds;        // real glBindTexture calls
    u32 texBindSkips;    // binds skipped by the bind cache
    u32 texImportSkips;  // imports elided as provably no-ops (no batch split)
    u32 shaderLoads;     // shader program switches
    u32 subFrames;       // rendered frames produced for this game frame

    // Lua traffic. us_hook divided by hookCalls is the average cost of one call
    // into a mod, which says how much of it is the binding's own dispatch: many
    // cheap calls mean dispatch dominates and fixing it helps every mod at once.
    //
    // It does not say a large figure is unfixable. us_hook is wall time inside
    // smlua_pcall(), so it also contains every engine function the callback
    // called back into -- run 15 had add_surface() at 32% of CPU that way, and
    // it was fixed engine-side. The sampled profile is what separates mod
    // bytecode from engine code that mods merely invoke.
    //
    // Field gets/sets count the cobject __index/__newindex path, the hottest
    // thing in the binding and the one that scales with how hard mods poke at
    // object fields rather than with how much work they do.
    u32 hookCalls;       // every smlua_call_hook() -- exactly what CTX_HOOK times
    u32 hookBehavior;    // of those, per-object behaviour callbacks
    u32 luaFieldGets;    // cobject __index
    u32 luaFieldSets;    // cobject __newindex
    // Field lookups served by the direct-mapped memo in smlua_get_object_field()
    // rather than by hashing into the per-LOT index. Against luaFieldGets +
    // luaFieldSets this is the hit rate; a low one means the memo is thrashing
    // and is costing a probe for nothing.
    u32 luaFieldMemoHits;

    // Packet codec traffic. us_netcodec on its own cannot distinguish "we
    // compress a lot of packets" from "each compression is expensive", and the
    // two have completely different fixes. Dividing us_netcodec by these gives
    // the per-call cost directly, which is what says whether the zlib stream
    // setup or the packet volume is the thing to attack.
    u32 codecCompressCalls;
    u32 codecDecompressCalls;

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

    // The same draws and texture splits, divided by whether the pass they belong
    // to can be reordered at all.
    //
    // Run 16 put draw submission at 50.9us per call (R^2 0.991 against draws and
    // verts), so with nine Bowsers on screen 671 draws cost 34.2ms of a 53.9ms
    // display-list pass -- double the vertex work -- and 87.7% of the splits
    // behind them were fltexture. Batching by texture would collapse a lot of
    // that, but only where draw order is free to change.
    //
    // geo_process_master_list_sub() emits one gDPSetRenderMode per master list,
    // and the opaque layers (LAYER_FORCE..LAYER_OPAQUE_INTER) use render modes
    // carrying Z_UPD while the transparent ones (LAYER_ALPHA..
    // LAYER_TRANSPARENT_INTER) do not. Depth-writing draws resolve by z-buffer
    // and may be emitted in any order; blended draws must stay back-to-front. So
    // Z_UPD at flush time is the layer class, without the renderer needing to be
    // told the layer index or the display list gaining a marker command.
    //
    // What this is for: `fltexopaque` is the recoverable part. If the texture
    // churn turns out to sit in `fltexblend`, reordering cannot touch it and the
    // idea is dead without anyone having to try it.
    u32 drawsOpaque;     // draw calls in a depth-writing (reorderable) pass
    u32 drawsBlend;      // draw calls in a blended (order-dependent) pass
    u32 flushTexOpaque;  // of flushTexture, the reorderable ones
    u32 flushTexBlend;   // of flushTexture, the ones that must keep their order

    // What the geo pass actually handed the renderer, which is what the
    // renderer's cost is proportional to -- the objects column counts what is
    // alive, not what is drawn, and nothing else here bridges the two.
    //
    // objsDrawn against objects says how much the existing frustum cull is
    // already removing, and therefore how much is left for a tighter one.
    //
    // dlDistinct against dlNodes says how many appended nodes share a display
    // list. Instances of one model resolve to the same shared Gfx pointer, so a
    // small dlDistinct against a large dlNodes means a same-display-list
    // batching pass has something to collapse; if they track each other, the
    // geometry is per-instance and no such pass would help.
    u32 objsDrawn;       // objects that passed obj_is_in_view() and were rendered
    // Objects the cull removed, and how many of those the screen-space size test
    // caught on its own. objsDrawn + objsCulled is everything the geo pass tested,
    // so the pair turns "the cull passes too much" from an inference into a
    // measurement -- and objsCulledSize says whether cull_min_pixels is earning
    // its keep or just costing a divide per object.
    u32 objsCulled;
    u32 objsCulledSize;
    u32 dlNodes;         // display lists appended to the master lists
    u32 dlDistinct;      // distinct display list pointers among them

    // Renders dropped to keep the simulation at the wall-clock 30Hz the netcode's
    // area timer runs on. Non-zero means the client was behind and traded picture
    // for staying in sync; sustained non-zero means it is not winning that trade
    // and needs to stop being authoritative rather than just drawing less.
    u32 renderSkips;
};

extern struct ProfileCounters gProfileCounters;

// Writes per-hook-type call counts and microseconds to `path`, alongside the
// CSV and the sampler dump. Defined in smlua_hooks.c, which owns both the
// accumulators and the hook type names; declared here so profile_log.c does not
// have to pull in the Lua headers.
void profile_dump_hook_types(const char* path, double nowSeconds);

// Closes the current hook-attribution window if `nowSeconds` has crossed its
// end. Opens the file on first call. Same windowing as the sampled profile, so
// the two can be sliced to the same stretch of play.
void profile_hook_types_tick(const char* path, double nowSeconds, double windowSeconds);

#define PROFILE_ADD(_field, _n) (gProfileCounters._field += (u32)(_n))

// Counts one appended display list: bumps dlNodes, and dlDistinct as well if
// this pointer has not already been seen this frame. Out of line because it
// needs a per-frame set, which lives in profile_log.c next to the reset.
void profile_note_display_list(const void *displayList);
#define PROFILE_NOTE_DL(_dl) profile_note_display_list(_dl)

void profile_log_init(void);
void profile_log_frame(void);
void profile_log_shutdown(void);

#else

#define PROFILE_ADD(_field, _n) ((void)0)
#define PROFILE_NOTE_DL(_dl)    ((void)0)
#define profile_log_init()      ((void)0)
#define profile_log_frame()     ((void)0)
#define profile_log_shutdown()  ((void)0)

#endif
