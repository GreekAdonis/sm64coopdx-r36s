# RK3326 optimisation plan

Implementation plan for the findings in `rk3326-cpu-investigation.md` (part 1) and
`rk3326-cpu-investigation-2-player-scaling.md` (part 2).

Base: `worktree-interp-skip-tri-dirty` @ `c2196e8c8`.
Implementation branch: `worktree-rk3326-cpu-opt`, branched from `c2196e8c8`.

> **Status: Milestone 1 is implemented and ready to build.** Six commits.
> Everything in it landed except item 1.6, the O(1) tri-state guard, which moved
> to Milestone 2 — see the note under that item for why. Every changed file is
> `-Wall -Wextra` clean under both the profile and release configurations.
>
> Two pre-existing memory-safety bugs turned up while implementing and are fixed
> in passing; both are described in their items below.
>
> | | change | commit |
> |---|---|---|
> | 1.1 | per-hook-type attribution | `785eea636` |
> | 1.2 | codec call counters | folded into `376efd0ee` |
> | 1.3 | persistent `z_stream` + heap overflow fix | `376efd0ee` |
> | 1.4 | stop double-zeroing packets + stack overflow fix | `405069936` |
> | 1.5 | `sync_object_should_own` | `467798a76` |
> | 1.6 | O(1) tri-state guard | **deferred to Milestone 2** |
> | 1.7 | fewer GL calls per shader switch | `6c8508e40` |

**Goal.** The 9-player frame does 39.3ms of work against 33.3ms. It spans 2.34
vblanks, rounds to 3, and presents as 20fps. We need ~5.6ms, and partial credit
is worth nothing until the gap closes — 4ms of savings still reads as 20fps.

---

## The constraint that shapes everything: builds are expensive

Every measurement costs a device build plus a populated 9-player room. So the
plan is organised into **four milestones, each one build and one capture**, not
into eleven individual changes. Within a milestone, changes are bundled only when
each one has its own distinct signal in the CSV, so a single capture can still
attribute the win. Each change is its own commit, so any one can be reverted
without disturbing the others.

Milestone 1 deliberately carries the instrumentation, so that by the time we are
deciding what to do about the ~9ms of mod time, we already have the data.

---

## A nonlinearity worth knowing about before we start

`cur_obj_update` (`behavior_script.c:1311-1343`) re-runs an object's behaviour
until its `areaTimer` catches up to `gNetworkAreaTimer`, which is wall-clock
driven. At 20fps every area-timer object runs its behaviour ~1.5× per frame; at
30fps it runs once.

So crossing 33.3ms does not just stop the frame-doubling — it also removes the
catch-up multiplier from `us_objects`, which is 13.6ms at 9 players. **The last
millisecond recovered is worth more than the first.** I have not tried to
quantify this (it is circular to measure from the same capture), so none of it is
counted in the budget below. Treat it as headroom that makes the target more
reachable than the raw arithmetic suggests, not as licence to aim lower.

---

## Milestone 1 — instrumentation and every low-risk win

Seven commits, one build. Expected **~2.4–4.0 ms**. Nothing here changes the wire
format, the protocol, or what is drawn.

### 1.1 Per-hook-type attribution *(profile build only, no runtime cost)*

The single largest unattributed cost is ~1.2ms per player inside mod callbacks,
and we cannot name the callback. `hookCalls` is one global counter.

`smlua_call_event_hooks` is a macro (`smlua_hooks.h:177`), so this needs **no
autogen regeneration**:

```c
#define smlua_call_event_hooks(hookEventType, ...) \
    (gCurHookType = hookEventType, smlua_call_event_hooks_##hookEventType(__VA_ARGS__))
```

`smlua_call_hook()` then accumulates into `hookTypeCalls[HOOK_MAX]` and
`hookTypeUs[HOOK_MAX]`, dumped beside the CSV like the sampler file. Guarded by
`PROFILE_BUILD` so the shipping binary is untouched.

**Why first:** this is the only route into the ~9ms that currently has no owner,
and it costs one build we are spending anyway.

### 1.2 Codec call counter *(profile build only)*

Add `codecCompressCalls` / `codecDecompressCalls`. `us_netcodec` divided by calls
gives per-call cost directly, which makes 1.3 verifiable in isolation even though
it ships in a bundle.

### 1.3 Persistent `z_stream`, and fix the buffer overflow — **~1.5–2.0 ms**

`packet.c:38`. Replace `compress2()` with a process-lifetime `z_stream`:

```c
deflateInit2(&sDeflate, Z_BEST_SPEED, Z_DEFLATED, 11, 4, Z_DEFAULT_STRATEGY);
// per packet: deflateReset, set next_in/avail_in/next_out/avail_out, deflate(Z_FINISH)
```

Measured 7–15× (`tools/packet-codec-bench.c`), byte-identical output, and
wire-compatible — `windowBits` 11 declares a smaller window, which `inflate()`
accepts, and `memLevel` has no wire representation. Fall back to `compress2()` if
`deflateInit2` fails, so a low-memory device degrades rather than drops packets.

**Also fix a latent heap overflow while we are in here.** `increase_comp_buffer`
allocates `PACKET_LENGTH` = 3000 bytes, but `compress2` is handed
`compressedLen = compressBound(dataLength + 4)`, which is **3017** for a
maximum-size packet (verified). `compress2` treats that as the destination
capacity, so a maximally-sized incompressible packet — an already-compressed mod
bytestring, say — can write up to 17 bytes past the allocation. Allocate
`compressBound(PACKET_LENGTH + sizeof(u32))` instead.

Leave `packet_decompress()` alone. Part 2 measured `uncompress()` at 0.18–4.8µs
with a persistent stream barely helping, because `inflateInit` defers its window
allocation. There is nothing to win there.

### 1.4 Stop zeroing every packet twice — **~0.2–0.5 ms**

`struct Packet p = { 0 }` zeroes all ~3,048 bytes, and then `packet_init`
(`packet_read_write.c:16`) does `memset(packet->buffer, 0, PACKET_LENGTH)` again.
Every constructed packet pays ~6KB of memset. At ~80 owned sync objects that is
~480KB per frame before anything is sent.

Two steps, in order of safety:

1. **Free and obviously correct:** at sites that call `packet_init` immediately
   after — `packet_object.c:367`, `packet_player.c:233` — the initializer's
   zeroing of `buffer` is dead, because `packet_init` overwrites it. Replace with
   `memset(&p, 0, offsetof(struct Packet, buffer))`. Behaviour is identical by
   construction.
2. **Needs a short audit:** `packet_init`'s own `memset` is also mostly dead —
   `packet_write` advances `cursor`/`dataLength` and every read is bounded by
   `dataLength` (`packet_hash`), `dataLength + 4` (the hash write and the
   compressor). Removing it requires confirming no reader indexes past
   `dataLength`. Do this as a separate commit so it can be dropped alone.

`network.c:421` (the receive path) is a third site; `packet_decompress`
overwrites the buffer, so the same step-1 treatment applies.

### 1.5 `sync_object_should_own` — **~0.1–0.3 ms**

`sync_object.c:322-326`. Three changes to a loop that runs once per sync object
per frame and always iterates all `MAX_PLAYERS` = 16 slots:

- Hoist the loop-invariant `player_distance(&gMarioStates[0], so->o)`.
- Compare squared distances. Ownership needs only the ordering, and the
  comparison is monotonic in the square. Add a `static` squared helper rather
  than changing `player_distance()`, which is Lua-exposed.
- Skip the degenerate `i == 0` iteration.

Separately, change `player_distance()`'s `sqrt` to `sqrtf` — it promotes three
`f32`s to `f64` on a core with no FP64 vector unit — and hoist the
`clock_elapsed()` call out of the `sync_objects_update` loop
(`packet_object.c:528`).

### 1.6 O(1) tri-state guard — **~0.2–0.4 ms**

`gfx_pc.c:1306-1316` compares eleven fields on every triangle to guard work that
~97% of triangles skip (miss rate is bounded by batch splits: ~77 against 2,571
triangles). Replace the key with a single `u32` version counter bumped by the
setters that write those registers.

**Deferred to Milestone 2.** Two reasons found while implementing:

1. The cache's own comment already records this decision and argues against it:
   *"The combine mode is keyed by value rather than by a generation counter on
   purpose… Watching the words directly means no writer has to cooperate:
   `gfx_dp_texture_rectangle()` and `gfx_dp_fill_rectangle()` both restore
   `rdp.combine_mode` by plain struct assignment, which any hand-maintained
   dirty flag would have missed."* Overriding that needs the verification build
   to actually run, not just to exist.
2. The mitigation is self-defeating as specified. If the cross-check is compiled
   into profile builds, the profile build keeps paying the eleven comparisons —
   so the capture we measure with would not show the win. It needs its own
   build, and a build is exactly the resource this plan is rationing.

Milestone 2 already includes a visual-verification step and a runtime toggle for
the bucketing work, so the guard belongs in that build, where the same session
can check both.

This still answers part 1's open thread: the cache is *not* the problem, the
guard is. Do not revert `9d59484d6`.

### 1.7 Fewer GL calls per shader switch — **~0.4–0.8 ms**

`gfx_opengl.c:330-348`. 45 shader switches per frame, each doing ~20+ driver
calls into a `libGLESv2` that is already 24% of the main thread:

- Track a bitmask of enabled attribute arrays; emit only the delta instead of
  `N` disables followed by `N` enables.
- Skip `glVertexAttribPointer` when the incoming program's `num_floats` and
  `attrib_sizes` match what that attribute index was last given.
- Version the shader-flag uniforms. `glUniform1iv`/`glUniform1fv` of
  `SHADER_FLAG_MAX` arrays currently re-upload on every bind; they change only
  when a mod changes them. Same for the `frame_count` noise uniform (once per
  program per frame) and the texture size/filter uniforms. The file already uses
  this idiom for `prg->uploaded_filtering`.

**Verification:** `shaders` stays constant while `us_gfxdl` falls.

### Milestone 1 exit check

Capture as before, then `tools/profile_report.py <csv> --binary <unstripped>`.
It now also prints a **HOOK TYPES** section from the new `<csv>.hooks` file;
the column to read there is **us/call**, since a hook whose body walks the
player list shows up as a small number of very expensive calls.

| signal | expectation |
|---|---|
| `us_netcodec` | 2.5ms → **< 0.4ms** |
| `us_netcodec / codeccomp` | 7–15× lower per call |
| `us_gfxdl` at constant `draws`/`shaders` | down ~0.4–0.8ms |
| `us_net` − codec − socket | down ~0.2–0.5ms |
| rendering | pixel-identical — nothing in Milestone 1 changes what is drawn |
| `<csv>.hooks` | names the hooks carrying the ~1.2ms/player |

If `us_netcodec` does not collapse, stop and re-measure before continuing — it
would mean the compression cost is not where the benchmark says it is.

The renderer change in 1.7 is the only item that could plausibly break the
picture, and it fails loudly rather than subtly: a desynchronised attribute
mirror means geometry reads the wrong vertex layout, which is immediately
obvious. If anything looks wrong, revert `6c8508e40` alone; the rest of the
milestone is independent of it.

---

## Milestone 2 — renderer batching

One commit, one build. Expected **~1.5–2.5 ms**. This is the largest single item
and the one with real correctness risk, so it gets its own build.

### 2.1 Bucket the opaque master list by display-list pointer

`geo_process_master_list_sub` (`rendering_graph_node.c:563`) walks
`node->listHeads[i]` in insertion order. `dlDistinct`/`dlNodes` = 121/390 = 0.31,
so each distinct display list is emitted ~3.2× per frame, just not adjacently.
58.6% of batch splits are shader switches and 33.5% are texture re-imports; a
shared `Gfx*` implies the same combiner and the same textures, so grouping
instances collapses both causes at once.

Implementation: before emitting a layer, stably re-link its list so nodes sharing
a `displayList` pointer are adjacent. Open-addressed map from `Gfx*` to sublist
tail, O(n) over ~390 nodes. Stability matters — keep the relative order of nodes
sharing a pointer, so the interpolation table stays aligned with emit order.

Each `DisplayListNode` carries its own transform and is emitted as a
self-contained `gSPMatrix` + `gSPDisplayList` pair, and `sMtxTbl` entries are
appended inside the same loop recording their own `pos`, so reordering needs no
change to the interpolation path.

**Layer safety.** Start with `LAYER_OPAQUE` (1) only. It is z-buffered, depth-
writing and unblended, so draw order is not observable. Do **not** touch
`LAYER_FORCE` (0, explicitly ordered), the decal layers (2, 6 — ordered against
the surface they decal), or the transparent layers (5, 7 — order-dependent by
definition). `LAYER_ALPHA` (4) and `LAYER_OPAQUE_INTER` (3) are probably safe and
worth trying **as a second commit, after the first is confirmed**, not bundled.

**Runtime toggle.** Put this behind a flag that can be flipped at runtime, so a
single session can A/B it — toggle off, confirm the picture is identical, toggle
on, watch `draws` and `flshader` fall. Given the correctness risk that is worth
the small amount of plumbing, and it means one build answers both "is it correct"
and "is it worth it".

### Milestone 2 exit check

| signal | expectation |
|---|---|
| `draws` | 79 → **~40–55** |
| `flshader` | 45 → **~20–30** |
| `fltexture` | 26 → **~12–18** |
| `us_gfxdl` | 13.4ms → **~11ms** |
| picture with toggle on vs off | identical |

If `draws` barely moves, the instances are not sharing display lists the way
`dlDistinct` implies, and the remaining renderer work should be abandoned rather
than pushed.

---

## Milestone 3 — network restructure

Three commits, one build. Expected **~1.0–1.3 ms**. Deliberately last: it is the
most invasive, and Milestone 1.3 has already taken most of the codec cost off the
table, which lowers what is left to win here.

### 3.1 Compress once per broadcast

CoopNet sets `requireServerBroadcast = false` (`coopnet.c:324`), so
`network_send()` (`network.c:396-416`) fans out and `network_send_to()` compresses
per recipient. The payload is identical across recipients — destination is
`PACKET_DESTINATION_BROADCAST` for all of them, flags do not depend on the
recipient, `packet_set_ordered_data` writes only on the first iteration, and
`packet_hash` is a pure function of the buffer.

Hoist the invariant work (flags, destination, hash, compression) out of the loop;
leave the per-recipient bookkeeping — rate limiting, `lastSent`,
`network_remember_reliable` — inside it. Scope the fast path to
`p->requestBroadcast`, and keep the existing path for everything else.

### 3.2 `coopnet_send()` for broadcasts

`ns_coopnet_network_send` (`coopnet.c:233`) only ever calls `coopnet_send_to()`
per peer. `libcoopnet.h:71` exports a lobby-wide `coopnet_send()` that the tree
never uses. This collapses N syscalls to one, against `us_netsocket` = 1.5ms.

**Blocked on verification:** confirm against `coop-deluxe/coopnet @9d9b3dd` (the
commit `lib/coopnet/README.md` pins) that `coopnet_send()` targets lobby members
rather than all connected peers, and that it is main-thread safe in the way
`coopnet_send_to()` is. If that cannot be established from the source, skip it —
3.1 stands on its own. Note this is unrelated to the send-thread dead end: it
removes syscalls rather than moving them off-thread, so it does not touch the
unsynchronised `mPeers` map.

### 3.3 One reliable entry per broadcast

`network_remember_reliable` (`packet_reliable.c:95`) is called inside the
per-recipient loop and `calloc`s plus copies a whole ~3KB `struct Packet` for
each recipient — ~48KB for one logical packet in a 9-player room. Then
`network_update_reliable` (`packet_reliable.c:155`) walks that N×-longer list
every frame with two `clock_elapsed()` calls per node.

Store one entry per logical broadcast with a per-recipient ack bitmask. This is
the most intricate change in the plan; if Milestones 1–2 have already closed the
gap, drop it.

---

## Milestone 4 — decide with data, not guesses

By here we will have per-hook-type numbers from 1.1 and a measured frame budget.
Two branches:

- **Under 33.3ms.** Stop optimising. Confirm the area-timer catch-up has stopped
  firing (`renderskips` at zero, `us_objects` down) and re-capture to see where
  the new steady state sits.
- **Still over.** The remaining cost is ~1.2ms per player of mod bytecode looping
  the player list inside once-per-frame hooks. No client-side engine change
  reaches it. The options are then (a) whatever 1.1 names — if it is one hook in
  one mod, that is a mod fix, not an engine fix — or (b) Layer 2, the authority
  backstop, so a client that cannot keep up stops asserting sync-object ownership
  and broadcasting state from a simulation running at the wrong rate. Layer 2 is
  still blocked on the ownership-vacuum design problem from the original brief
  and needs the mod-space route via `so->override_ownership` plus
  `PACKET_LUA_CUSTOM_BYTESTRING`.

---

## Budget

| milestone | expected | risk |
|---|---|---|
| 1 — instrumentation + low-risk wins | 2.4–4.0 ms | low |
| 2 — opaque list bucketing | 1.5–2.5 ms | medium |
| 3 — network restructure | 1.0–1.3 ms | medium |
| **total** | **4.9–7.8 ms** | |

Against a 5.6ms requirement. Milestones 1+2 alone are 3.9–6.5ms, and the
area-timer nonlinearity above sits on top of that unmeasured. Milestone 3 is the
reserve.

## What is deliberately not in this plan

- **Anything targeting the Lua field-get path.** The hash index already landed in
  `a08fcd55c`; the residual ~2.3ms is Lua→C metamethod dispatch, which cannot be
  removed while `__index` reads live C memory.
- **Behaviour LOD for distant objects.** `hookBehavior` is 11 of 468 hook calls
  per frame, so it would reach ~2% of them.
- **Threading the netcode.** Dead end, established in the original brief:
  `Client::PeerSendTo` mutates an unsynchronised `std::map`.
- **Renderer work targeting player count.** Part 2 could not substantiate it —
  the apparent slope implies 234 draws at 9 players against an actual 79, and
  controlled cells show fewer draws at 9 players than at 8.
