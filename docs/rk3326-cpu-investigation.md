# RK3326 CPU investigation — where the multiplayer 5.6ms lives

Analysis only. Nothing here was built or run. Base commit: `c2196e8c8`
(`worktree-interp-skip-tri-dirty`). Evidence is `docs/profiling-results-7/`
(minigame room, up to 9 players, 15,343 frames) unless stated.

The target from the perf brief: the 9-player frame does 38.9ms of work against a
33.3ms budget, spans 2.34 vblanks, rounds to 3, and reads as 20fps. Recovering
~5.6ms doesn't buy 23fps — it buys 30.

**Conclusion up front: the 5.6ms is reachable, and almost none of it is in Lua.**
The recoverable time is in the network send path and the renderer's batch
splitting. The Lua hook time is real but is mod bytecode, and the part of it the
engine controls is ~2.3ms, not the ~10ms a naive regression suggests.

---

## 1. Three corrections to the brief's model

These change what is worth working on, so they come first.

### 1.1 Hook time is not per-object behaviour callbacks

The brief states "nearly all hook time is per-object behaviour callbacks fired
from object update". The counters say otherwise:

| counter | per frame (players ≥ 5) |
|---|---|
| `hookcalls` | 468.0 |
| `hookbhv` | 11.1 |

**11 of 468.** The other 457 are event hooks — `HOOK_UPDATE`,
`HOOK_MARIO_UPDATE`, `HOOK_ON_OBJECT_RENDER` and friends — one call per
registered mod callback per fire. A "behaviour LOD for distant objects" (brief's
target #2) would therefore touch ~2% of hook calls. It is not a lever.

### 1.2 Hook volume is flat in player count

| players | frames | `hookcalls`/frame | `us_hook` | `objects` |
|---|---|---|---|---|
| 5–8 | 492 | 464.6 | 14,526 | 50.2 |
| 9–10 | 2,940 | 468.8 | 16,356 | 83.1 |

Player count nearly doubles, object count nearly doubles, hook calls move 0.9%
and hook time 12.6%. Hook cost is a function of *which mods are loaded*, not of
how many players are in the room.

This also means the brief's central solo-vs-9-player table is confounded. Run
7's solo frames are level 16 with `hookcalls = 0` and `fieldgets = 0` — no mods
loaded at all. That table compares "no mods, 1 player" against "mods, 9 players"
and attributes the whole delta to player count.

### 1.3 The field-lookup optimisation the brief proposes already exists

Target #1 is "cache the string→offset lookup". Commit `a08fcd55c` ("Index Lua
cobject field tables instead of binary searching them") is already an ancestor
of `main`, and `smlua_get_object_field_from_ot()` is an FNV-1a hash index with a
stored-hash guard. The profiled binary has it. It is 0.58% of the sampler.

The naive regression is what makes this look bigger than it is:

```
us_hook ~ hookcalls + fieldgets + objects   R2=0.696  fieldgets=3.082 us
us_hook ~ fieldgets                         R2=0.656  fieldgets=3.440 us
us_hook ~ hookcalls                         R2=0.158
```

3.1µs/get × 3,416 gets = 10.6ms, i.e. two-thirds of hook time. That coefficient
is not a marginal cost — `fieldgets` is the best available proxy for "how much
mod bytecode ran this frame", so it absorbs all correlated mod work.

Costing it from the sampler instead, which is workload-independent:
`smlua__get_field` (266) + `smlua_get_object_field_from_ot` (438) +
`smlua_push_field` (102) + `smlua_push_object` (143), plus the metamethod
round-trip (`luaV_finishget` 223, `luaT_callTM` 228, `luaT_gettmbyobj` 94,
`lua_tolstring` 113, `lua_touserdata` 81, `lua_type` 89) ≈ 1,650 samples ≈ 8.3s
over ~11.7M field gets = **~0.7µs per get, ~2.3ms/frame**.

What remains is the Lua→C metamethod transition, not the lookup. That has no
cheap fix: `__index` must be a C function because the value is read out of live
C memory, so every `obj.oPosX` pays a `luaV_finishget` → `luaT_callTM` →
`luaD_precall` → C → `luaD_poscall` round trip. Treat ~2.3ms as near the floor
and spend effort elsewhere.

One thing that is *not* a cost, checked and ruled out: `gMarioStates` is a plain
Lua table of pre-built userdata (`EXPOSE_GLOBAL_ARRAY`, `smlua_cobject.c:917`),
so the `lua_getglobal("gMarioStates")` + `lua_gettable` that all 24 MarioState
hook thunks perform is two hash lookups, not a metamethod call and not an
allocation. There are also no Lua GC symbols anywhere in the top 90 — allocation
churn is not a factor.

---

## 2. The 9-player frame, measured

Contexts are inclusive, so nested rows overlap their parent.

```
total                     49.2 ms
  network                  5.5      netcodec 2.5, netsocket 1.5, other 1.3
  game loop               18.6      of which lua hooks 16.4
  lua update               0.7
  audio                    0.6
  render                  23.8      gfxdl 13.4, swap 9.9, interp 0.5
```

Work excluding the vsync idle in `swap`: **39.3ms**.

Engine-controllable slices: `gfxdl` 13.4ms, `network` 5.5ms, the ~2.3ms of
field-get machinery inside hooks, and ~2.2ms of non-hook game loop. The other
~14ms of hook time is mod bytecode and no client-side change reaches it.

---

## 3. Findings, ranked by expected recovery

### A. Compress each broadcast packet once, not once per recipient — ~1.2ms

`coopnet.c:324` sets `.requireServerBroadcast = false`. So for CoopNet,
`network_send()` (`network.c:396-416`) falls through to the fan-out loop and
calls `network_send_to(i, p)` for every connected player, and
`network_send_to()` calls `packet_compress()` at `network.c:352` — **inside the
per-recipient loop**. In a 9-player room the same payload is zlib-compressed
eight times.

The payload really is identical across those eight iterations:

- `packet_set_destination()` writes `PACKET_DESTINATION_BROADCAST` for every
  recipient, because `network_send()` sets `p->requestBroadcast = TRUE` first
  (`network.c:386`, then `network.c:267-269`). Not the per-player global index.
- `packet_set_flags()` derives from packet fields only, not from the recipient.
- `packet_set_ordered_data()` early-returns once `orderedSeqId != 0`, so it
  writes on the first iteration only.
- `packet_hash()` is a pure function of `buffer[0..dataLength)`, so it produces
  the same four bytes every time.

Nothing between iterations touches the buffer. Compress once before the loop and
reuse `sCompBuffer` for all recipients; `gNetworkSystem->send()` is synchronous,
so a single static buffer is safe. Keep the per-recipient path for anything not
provably identical — scope the fast path to `p->requestBroadcast` set.

Sizing: `us_netcodec` is 2.5ms/frame at 9 players and covers both `compress2()`
and `uncompress()`. Compression at `Z_BEST_SPEED` costs several times its
matching decompression, and the send side is multiplied by recipients while the
receive side is not, so compression is the larger half. Removing 7/8 of it
should recover on the order of 1.2ms. The scaling is visible in the data:
netcodec goes 1,304µs → 2,516µs between 8 and 9 players.

`packet_hash()` is also an O(dataLength) loop recomputed per recipient. Same
hoist, much smaller prize.

### B. Use CoopNet's broadcast primitive — ~1.0ms, needs verification

`lib/coopnet/include/libcoopnet.h:71` exports:

```c
CoopNetRc coopnet_send(const uint8_t* aData, uint64_t aDataLength);
```

`ns_coopnet_network_send()` (`coopnet.c:233`) only ever calls `coopnet_send_to()`
per peer. The lobby-wide primitive is never used anywhere in the tree.

With (A) already collapsing the compression, this collapses the remaining N
syscalls to one, against `us_netsocket` = 1.5ms/frame.

Verify before building: that `coopnet_send()` in the pinned build
(`coop-deluxe/coopnet @9d9b3dd`, per `lib/coopnet/README.md`) fans out to lobby
members and not to every connected peer, and that it is main-thread safe in the
same way `coopnet_send_to()` is. Note this is orthogonal to the send-thread dead
end — it removes syscalls rather than moving them to another thread, so it does
not touch the unsynchronised `mPeers` `std::map` problem.

Per-recipient bookkeeping — the rate limiter, `lastSent`,
`network_remember_reliable()` — still has to run per player even when the send
is collapsed.

### C. Bucket the opaque master list by display-list pointer — ~1.5–2.5ms

The brief writes this off as "only matters for the voxel-mod workload" and "not
worth doing for the multiplayer case". The multiplayer numbers disagree.

Batch splits per frame, 9-player room:

| cause | per frame | share |
|---|---|---|
| `flshader` | 45.0 | 58.6% |
| `fltexture` | 25.7 | 33.5% |
| `flfull` | 3.3 | 4.3% |
| `fldepth` | 2.7 | 3.6% |
| everything else | 0.02 | 0.0% |

And the cost of a batch:

```
us_gfxdl ~ draws+tris+verts                  R2=0.913  draws=77.6us  tris=1.09  verts=0.46
us_gfxdl ~ draws+tris+verts+shaders+binds    R2=0.918  draws=41.4us  shaders=37.3  binds=17.1
```

78.8 draws/frame at 41–78µs each is 3.3–6.1ms — roughly half of `gfxdl`. 92% of
those draws exist because the shader program or the bound texture changed.

The material fact: `dldistinct`/`dlnodes` = 121.2 / 390.0 = **0.31**. Each
distinct display list is emitted ~3.2 times per frame, because same-model
instances share a `Gfx*` (`obj_set_model` → `dynos_model_get_geo` →
`sharedChild`). They are just not adjacent in the list.

`geo_process_master_list_sub()` (`rendering_graph_node.c:563`) walks
`node->listHeads[i]` in insertion order and emits a `gSPMatrix` + `gSPDisplayList`
pair per node. Reordering that singly-linked list by `displayList` pointer before
emitting — a stable bucket, not a comparison sort — groups instances of the same
model together, and a shared `Gfx*` implies the same combiner and the same
textures. Both dominant split causes collapse together.

Safety: only for z-buffered, non-blended layers, where draw order is not
observable — `LAYER_OPAQUE` and its z-buffered siblings. Transparent and decal
layers must keep insertion order. The interpolation table (`sMtxTbl`) is built
inside the same loop and records `gDisplayListHead` positions as it goes, so it
follows any new emit order without change.

The texture bind cache is not the problem here, incidentally: 57.1 real binds
against 0.5 skips per frame is a ~1% hit rate, but `gfx_opengl_select_texture()`
(`gfx_opengl.c:941`) is correct. The list genuinely alternates textures 57 times
a frame. Reordering is what fixes it.

### D. Make the tri-state cache guard O(1) — answers open thread #1

The brief flags `9d59484d6` as unproven (+6.1% normalised) and suggests adding a
hit/miss counter, reverting if the hit rate is poor. The hit rate can be derived
without instrumenting, and it is *high* — the guard is the problem, not the miss
rate.

`gfx_pc.c:1306-1316` compares eleven things on every triangle:

```c
if (!sTriState.valid
    || sTriState.other_mode_l     != rdp.other_mode_l
    || sTriState.other_mode_h     != rdp.other_mode_h
    || sTriState.geometry_mode    != rsp.geometry_mode
    || sTriState.cc_rgb1          != rdp.combine_mode.rgb1
    ...
    || sTriState.world_geometry   != tri_world_geometry) {
```

Every register in that key is one the renderer also flushes a batch for. So
misses are bounded by batch splits: ~77 splits against 2,571 triangles per
frame ⇒ **miss rate ~3%, hit rate ~97%**.

That is exactly why it measured slower. The cached work (a colour-combiner
lookup and an indirect `shader_get_info`) is skipped on 97% of triangles, but
all 2,571 triangles pay eleven dependent loads and compares — ~28,000 compares
per frame to avoid ~77 lookups.

Don't revert it. Replace the key with a single `u32` version counter bumped by
the setters that write those registers (`gfx_dp_set_combine_mode`,
`gfx_dp_set_other_mode_l`/`_h`, `gfx_sp_geometry_mode`, and wherever
`rdp.loaded_texture[1].addr` is written), plus `tri_world_geometry`. The guard
becomes one load and one compare, and the 97% hit rate finally pays.

### E. Cut the GL calls per shader switch — ~0.4–0.8ms

45 shader switches/frame, and `gfx_opengl_unload_shader` + `gfx_opengl_load_shader`
(`gfx_opengl.c:330-348`) do this every time:

- `N` × `glDisableVertexAttribArray` (old program)
- `glUseProgram`
- `N` × `glEnableVertexAttribArray` + `N` × `glVertexAttribPointer`
- `gfx_opengl_set_shader_uniforms()` — unconditionally re-uploads, including
  `glUniform1iv(..., SHADER_FLAG_MAX, gShaderFlags)` and
  `glUniform1fv(..., SHADER_FLAG_MAX, gShaderFlagValues)` for every
  world-geometry shader
- `gfx_opengl_set_texture_uniforms()` × 2

That is ~20+ driver calls per switch, ~900+/frame, into a `libGLESv2.so.2` that
is already 24% of the main thread. Three fixes, all local:

1. Track a bitmask of currently-enabled attribute arrays and emit only the
   delta. Programs in this renderer share a vertex layout family, so most
   switches need no enable/disable at all.
2. Skip `glVertexAttribPointer` when the new program's `num_floats` and
   `attrib_sizes` match what that index was last given — the pointer is per
   attribute index and survives the `glBufferData` re-upload.
3. Version the shader-flag uniforms. `gShaderFlags`/`gShaderFlagValues` change
   only when a mod changes them; store a per-program uploaded version against a
   global counter. Same for the `frame_count` noise uniform (once per program
   per frame) and the texture size/filter uniforms. The file already uses
   exactly this idiom for `prg->uploaded_filtering`.

This is pure overhead removal and does not depend on (C); doing (C) first
reduces how much it is worth.

### F. `sync_object_should_own()` — small, free

`sync_object.c:322-326`:

```c
for (s32 i = 0; i < MAX_PLAYERS; i++) {
    if (i != 0 && !is_player_in_local_area(&gMarioStates[i])) { continue; }
    if (player_distance(&gMarioStates[0], so->o) > player_distance(&gMarioStates[i], so->o)) { return false; }
}
```

`player_distance(&gMarioStates[0], so->o)` is loop-invariant and recomputed on
every iteration, up to `MAX_PLAYERS` times per sync object per frame. And
`player_distance()` (`sync_object.c:282`) ends in `sqrt` — the *double* one, so
three `f32`s get promoted to `f64` on a core with no FP64 vector unit
(`libm.so.6` is 0.49% of the sampler).

Three changes: hoist the invariant call; skip the degenerate `i == 0` iteration;
and compare squared distances, since ownership only needs the ordering and the
comparison is monotonic in the square. Coordinate magnitudes (~2×10⁴ ⇒ sums
~1.2×10⁹) are far inside `f32` range. The one caller that needs a real distance
is `packet_object.c:518`, where `updateRate = dist / 1000.0f`; that one wants
`sqrtf`, not `sqrt`.

Also in that loop, `packet_object.c:528` calls `clock_elapsed()` once per sync
object — `linux-vdso.so.1` is 0.51% of the sampler. Read it once per frame.

---

## 4. Adding up

| | expected |
|---|---|
| A. compress broadcasts once | ~1.2 ms |
| B. `coopnet_send()` broadcast | ~1.0 ms |
| C. bucket opaque list by DL pointer | ~1.5–2.5 ms |
| D. O(1) tri-state guard | ~0.2–0.4 ms |
| E. fewer GL calls per shader switch | ~0.4–0.8 ms |
| F. `sync_object_should_own` | ~0.1–0.3 ms |
| **total** | **~4.4–6.2 ms** |

Against a 5.6ms requirement, with the cliff meaning partial credit is worth
nothing until the whole gap closes. (A) + (C) alone are ~2.7–3.7ms and are the
two highest-confidence items; (B) and (E) are the ones most likely to over- or
under-deliver.

Note what is *not* on this list. There is no Lua item, because the field-lookup
index already landed and the remaining ~2.3ms is metamethod dispatch with no
cheap fix. The 16.4ms of hook time is mod bytecode. If this list lands and the
frame still misses, the next move is not a client-side optimisation — it is
Layer 2, the authority backstop, so a client that cannot keep up stops
broadcasting state from a simulation running at the wrong rate.

## 5. Still open

- Whether `coopnet_send()` in `@9d9b3dd` broadcasts to the lobby or to all
  peers. Blocks (B), not (A).
- How much of the 45 shader switches survives bucketing. `dldistinct`/`dlnodes`
  = 0.31 bounds it but does not predict it, because the split between opaque and
  transparent layers is not instrumented. A `dlnodes`/`dldistinct` pair counted
  per layer would settle it before any renderer change is written.
- The brief's other two open threads are unchanged: the camera parent-matrix
  identity check, and Layer 2's ownership-vacuum design problem.
