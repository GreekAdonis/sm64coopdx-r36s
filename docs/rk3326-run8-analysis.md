# Run 8 — what Milestone 1 bought, and what is actually capping the framerate

Capture: `docs/profiling-results-8/`, 36,718 iterations over 1,263s, up to 16
players, `audio_threaded=1` (new this run — audio is off the main thread and
`us_audio` now reads 0).

## Build lineage — confirmed cumulative

Checked against the binary and the log rather than assumed:

| evidence | what it proves is in the build |
|---|---|
| imports `deflateInit2_`, `deflateReset`, `deflate` **and** `compress2` | `376efd0ee` persistent zlib stream, with its `compress2` fallback |
| `codeccomp` / `codecdecomp` columns | same commit's counters |
| `sm64coopdx-profile.csv.hooks` exists | `785eea636` hook attribution (4th of 6) |
| `impskips` column | `20aa0e5ec` texture-import skips |
| `renderskips` column | `10d6c5657` render dropping |
| `dldistinct` column | `2296df774` geo counters |
| no `network_send_queue_*` | the send-thread revert held |

Git history is linear, so the 4th commit being present puts commits 1–4 in. The
5th (GL call reduction) is all `static` functions that inline away, so it cannot
be confirmed by symbol — but `libGLESv2` self time fell from 24.03% to 16.26%,
which is the effect it was meant to have.

## Milestone 1 verdict: the codec fix worked

| | run 7 (p≥5) | run 8 (p≥5) |
|---|---|---|
| `us_netcodec` per frame | 2,552 µs | **668 µs** |
| codec calls per frame | (uncounted) | 21.0 |
| **µs per codec call** | ~121 (implied) | **31.8** |

A 3.8× drop per call, against a benchmark that predicted 7–15× on desktop — the
gap is the A35. `libz` fell 2.01% → 1.51% of the sampler. Net saving ≈ 1.9 ms
per multiplayer frame. `libGLESv2` 24.03% → 16.26% and `gfx_sp_tri1` 15.11% →
11.32%.

**This is no longer a bottleneck.** `us_netcodec` is 668 µs against a frame that
is missing its budget by 16 ms.

## The actual problem: the client renders one frame in three

`renderskips` fires on **66.6% of multiplayer frames**, and the run-length
distribution is `{1: 10, 2: 9339}` — it hits the two-skip cap essentially every
time. The pattern is skip, skip, render.

    29.1 simulation ticks/s        <- the mitigation is doing its job
    14.3 displayed frames/s        <- this is the slowdown being felt

Sliced by whether the frame rendered (players ≥ 5, medians in µs):

| | total | net | game | objects | hook | gfxdl | swap | draws |
|---|---|---|---|---|---|---|---|---|
| skipped | 19,712 | 1,729 | 17,161 | 7,607 | 13,000 | 0 | 0 | 0 |
| rendered | 62,336 | 1,386 | 17,007 | 7,554 | 12,874 | 25,011 | 12,968 | 168 |

The important column is `game`: **17.0 ms on rendered frames and 17.2 ms on
skipped ones.** The simulation cost is identical either way. Dropping the render
buys nothing on the logic side — it only removes the 25 ms of `gfxdl`.

(An earlier pass at this table showed `hook` as 0 on rendered frames. That was an
artifact: over half of all rendered frames in the session are solo level-16
frames with no mods loaded, which dragged the median to zero. Restricting to
players ≥ 5 removes it.)

### The budget

    simulation, every iteration      19.7 ms
    rendering, when it happens     + 29.7 ms
    -------------------------------------------
    a frame that both simulates      49.4 ms   against 33.3

**We need ~16 ms**, not the 5.6 ms run 7 implied. The rooms in this session are
simply heavier: 125 objects against 78, and more mods.

Note the floor: simulation alone is 19.7 ms, so even a free renderer caps this at
~50 fps. The simulation is not the thing stopping 30 fps — the renderer is.

## Where the renderer time goes

Apportioned from sampler self time, per **rendered** frame:

| | ms | |
|---|---|---|
| `libGLESv2` (the driver) | 8.7 | draw calls and state changes |
| `gfx_sp_tri1` | 6.1 | per-triangle vertex packing |
| `gfx_sp_vertex` | 2.6 | |
| `gfx_run_dl` | 1.5 | |
| `geo_process_node_and_siblings` | 1.3 | |
| `mtxf_mul` | 0.9 | |

Per rendered frame the renderer is handed **173.7 draws, 4,058 triangles, 13,274
vertices, 92.1 shader switches and 132.5 texture binds** — roughly 400 driver
state operations. That is why the driver is the biggest single item.

And the batching opportunity is now larger than it was in run 7:

    dlNodes 472.1   dlDistinct 91.7   ratio 0.19
    -> each display list is emitted 5.1x per frame  (run 7: 3.2x)

    flshader 81.6 + fltexture 75.1 = 90% of all draw calls

Marginal costs on rendered frames (R² = 0.958):

    us_gfxdl ~ 40.6/draw + 1.14/tri + 0.73/vert

## Two regimes, not one

Rendered frames by level, sorted by `gfxdl` — these are different problems:

| lvl/area | n | total | gfxdl | game | hook | draws | tris | dlNodes | ratio | obj# |
|---|---|---|---|---|---|---|---|---|---|---|
| 58/1 | 1701 | 81,662 | **42,802** | 7,177 | 3,787 | 195 | 9,226 | 222 | 0.32 | 45 |
| 55/1 | 808 | 83,592 | **36,084** | 6,423 | 3,432 | 198 | 7,334 | 244 | 0.31 | 51 |
| 13/2 | 703 | 63,997 | 25,148 | 12,364 | 10,077 | 190 | 4,113 | 509 | 0.21 | 136 |
| 57/1 | 933 | 61,729 | 23,924 | 16,357 | 13,043 | 150 | 2,830 | 221 | 0.31 | 144 |
| 8/2 | 417 | 57,330 | 22,716 | **21,918** | **17,099** | 175 | 3,681 | 646 | 0.19 | 109 |
| 18/1 | 396 | 53,982 | 18,324 | **22,743** | **18,282** | 139 | 2,762 | 293 | 0.27 | 75 |
| 19/1 | 755 | 53,826 | 17,817 | **22,580** | **16,339** | 160 | 2,128 | 572 | 0.19 | 180 |
| 23/2 | 555 | 52,089 | 16,253 | **24,062** | **18,973** | 135 | 2,175 | 518 | 0.21 | 174 |
| 16/1 | 9358 | 32,811 | 14,678 | 3,139 | 0 | 124 | 2,881 | 462 | 0.20 | 127 |

- **Render-bound** (58, 55, 13/2, 57): geometry-heavy, `gfxdl` 24–43 ms, mod code
  cheap. Levels 58 and 55 push 7–9k triangles.
- **Logic-bound** (8, 18, 19, 23): `game` 22–24 ms of which `hook` is 16–19 ms,
  with 109–180 objects. Mod bytecode, not the renderer.

Nothing fixes both. Both need fixing.

One thing ruled out: **this is not GPU-bound.** `swap` is large (13 ms median,
28–35 ms on 55/58) but work + swap lands on a vsync multiple, so it is vsync
alignment, not a GPU stall. The internal render target is already 320×240
(`HANDHELD_FBO_DEFAULT_WIDTH`), so there is no easy resolution lever left either.

## Paths forward, in the order I would do them

### 1. Make the render-skip cap a config knob — hours, zero risk

`RENDER_SKIP_MAX_CONSECUTIVE` is a compile-time 2 (`pc_main.c:290`) and the data
shows it pinned at the cap 9,339 times out of 9,349 runs. Its comment reasons
that two skips is what lands the simulation on 30 Hz, and it does — 29.1 ticks/s.
But it costs 14.3 displayed fps.

A cap of 1 gives roughly 20 fps displayed for a simulation around 26 Hz. Whether
26 Hz sim at 20 fps feels better than 29 Hz sim at 14 fps is a judgement no
profile can make — it needs to be felt. Expose it, try both, and let the answer
pick itself. This is the cheapest available change to perceived smoothness and it
is worth doing before any further optimisation.

### 2. Milestone 2 — bucket the opaque list. Worth more than planned

Each display list is emitted 5.1× per frame and 90% of draw calls exist because
the shader or texture changed. Grouping instances that share a `Gfx*` collapses
both causes together. Estimate: draws 174 → ~95, shader switches 92 → ~40, for
roughly **3–4 ms** off the driver's 8.7 ms. Up from the 1.5–2.5 ms estimated
before, because the redundancy is worse here than in run 7.

The deferred O(1) tri-state guard rides along in this build; `gfx_sp_tri1` is
6.1 ms per rendered frame and the guard is a measurable slice of it.

### 3. Tighter culling — the only real lever on vertex and triangle cost

51 of 123.8 objects survive culling, and each drawn object averages 260 vertices.
Triangles and vertices together are ~8.7 ms of `gfxdl` and no amount of batching
touches them — only drawing fewer things does. `obj_is_in_view`
(`rendering_graph_node.c`) does not test vertical view, by its own docstring, and
distance-based LOD for actors does not exist. Taking `objsDrawn` from 51 to ~35
would cut ~30% of the vertex and triangle work, around 2.5 ms.

This is the largest remaining renderer item after batching, and the only one that
helps levels 58 and 55, where the geometry — not the state changes — is the cost.

### 4. Rebuild the hook table, then attack the 13 ms

`us_hook` is 12.9 ms per multiplayer frame and mod bytecode is the largest single
consumer in the whole profile. The run-8 hook table cannot rank it yet: an
attribution bug (fixed in `9bcb1aa85`) put 78% of hook time into the catch-all,
because a dispatcher calls into `smlua_call_hook` once per mod while the macro
tags the call site only once. What the table does establish — that hook time is
dominated by mod callbacks rather than engine dispatch — stands.

The next capture will name the hooks. Two candidates already visible and worth
watching, since their per-call costs are real measurements even where the totals
are undercounted:

- `HOOK_ON_HUD_RENDER` at **734 µs/call**
- `HOOK_ON_GEO_PROCESS` at **198 µs/call**, and it fires per graph node
- `HOOK_ON_PLAY_MODE_UPDATE` at **14.6 ms/call** — only 74 calls all session, so
  it costs nothing in throughput, but each one is a visible hitch

### Not worth doing

- **Anything further on the packet codec.** 668 µs/frame.
- **Lowering internal resolution.** Already 320×240, and this is not GPU-bound.
- **The Milestone 3 network restructure.** It was scoped at ~1.0–1.3 ms when the
  gap was 5.6 ms. The gap is now 16 ms and `us_net` is 1.4 ms in total. Park it.

## Honest assessment

Items 2 and 3 together are perhaps 6 ms of the 16 ms needed, and item 4 is
unquantified until the next capture. **Smooth 30 fps in the heavy co-op rooms is
not reachable by engine optimisation alone**, because 13 ms per frame is mod
bytecode the engine cannot reach and 19.7 ms is simulation that has to happen at
wall-clock rate whether or not we draw.

What is reachable: a materially better *displayed* framerate from item 1 today,
plus roughly 6 ms from items 2 and 3 which would take the heavy rooms from one
render in three to one in two. Getting past that needs the mod side — which is
what item 4 exists to identify.
