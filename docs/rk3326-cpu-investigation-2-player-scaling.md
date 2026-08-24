# RK3326 CPU investigation, part 2 — what actually scales with player count

Follow-up to `rk3326-cpu-investigation.md`. Same rules: analysis only, nothing
built or run against the game. Base `c2196e8c8`.

Two questions: is the compression dedup really just "8× → 1×", and does frame
cost genuinely scale with player count? Answers: **no, it's worse than 8× and
there is a cheaper fix**, and **yes, ~4.1ms per additional player, measured.**

---

## 1. Measuring player scaling properly

Part 1 could not separate player count from scene complexity, because run 7's
room only ever held 0 or 8–9 players. The other six captures do have ramps:

| capture | player counts with real frame counts |
|---|---|
| `profiling-results` | 2, 4, 5, 6, 10 |
| `profiling-results2` | 7, 8, 9 |
| `profiling-results3` | 8, 9, 10, 14, 15 |
| `profiling-results-4` | 10, 11, 12, 13 |
| `profiling-results-5` | 13, 14, 15 |
| `profiling-results-6` | 3, 4, 6, 7, 8, 9, 10 |

A plain regression on these is still worthless — player count correlates with
*which room you were in*, and in run 6 the object count swings 128 → 1194 across
player counts, producing negative player coefficients.

What works is **within-(capture, level, area) fixed effects**: compare cells that
differ in player count but share a scene, require ≥60 frames per cell and object
count stable within 25%, take the median-of-cells slope. 17 cells qualify.

### Marginal cost of one more player

| metric | µs/player (median) | cells positive |
|---|---|---|
| **`us_total`** | **+4080** | 13/17 |
| `us_hook` | +1155 | 14/17 |
| `us_game` | +1158 | 13/17 |
| `us_net` | +512 | 11/17 |
| `us_objects` | +223 | 10/17 |
| `hookcalls` | +3 | — |
| `fieldgets` | +226 | — |

So the felt effect is real and large: **each additional player costs about 4ms of
frame time.** Eight remote players is roughly 33ms — which is the whole budget,
and explains why a room is fine at 4 players and unplayable at 9.

The single cleanest cell, worth stating on its own because it is monotonic across
three player counts with the scene pinned — run 5, level 64 area 2, object count
exactly 166 and hook calls 486/489/491:

| players | `us_net` | `us_hook` | `objects` |
|---|---|---|---|
| 13 | 3,304 | 28,508 | 166 |
| 14 | 3,769 | 28,575 | 166 |
| 15 | 4,103 | 26,908 | 166 |

Network rises monotonically at ~400µs/player while hook time is flat. That cell
is the clearest evidence that the network cost is genuinely a function of peer
count and not of scene.

### What this rules out

- **Hook *volume* does not scale.** `hookcalls` moves +3 per player against a
  base of ~470. The engine is not firing more callbacks. The same callbacks take
  longer, and `fieldgets` rises +226/player — mods are looping over the player
  list inside once-per-frame hooks. Since the CTX timers are inclusive
  (`debug_context.c:19-42` — each context accumulates its full wall span, nested
  ones included), and `us_objects` stays near flat while `us_hook` rises, the
  per-player work is *not* in the per-Mario `HOOK_MARIO_UPDATE` callbacks fired
  from `bhv_mario_update` (`object_list_processor.c:272-275`). It is in
  `HOOK_UPDATE`-style once-per-frame mod code. **No engine change reaches this.**
- **The renderer does not reliably scale with players.** The naive slope says
  +2641µs and +26 draws per player, but that is contaminated and I am not
  reporting it as a finding: 26 draws/player would mean 234 draws at 9 players
  against an actual 79. Checking controlled cells shows it is what is *in frame*
  that drives it, not the roster — run 7 level 29 has **fewer** draws at 9
  players (60) than at 8 (75), and run 3 level 69 goes 221 → 210. Your read that
  graphics are close to done is not contradicted by this data.

That leaves `us_net` as the player-scaling cost the engine actually owns:
~400–512µs per player, ~half of it in `us_netcodec`.

---

## 2. The compression finding is bigger than "8× → 1×"

Part 1 found `packet_compress()` running once per recipient. That is one of two
multiplied wastes, and it turns out to be the *less* important one.

`packet.c:38` uses zlib's one-shot convenience call:

```c
compress2(sCompBuffer, &compressedLen, p->buffer, sourceSize, Z_BEST_SPEED);
```

`compress2()` is `deflateInit` + `deflate` + `deflateEnd`. `deflateInit` allocates
the window, the hash head table, the prev table and the pending buffer — on the
order of 260KB across four `malloc`s — zeroes the hash table, and `deflateEnd`
frees it all again. **Per call.** For a packet that is usually a few hundred
bytes, the setup dwarfs the compression.

### Measured

Microbenchmark (`tools/packet-codec-bench.c`: `compress2` vs a persistent
`z_stream` + `deflateReset`; build with `gcc -O2 -o /tmp/b tools/packet-codec-bench.c -lz`), gcc
-O2, 20,000 iterations per size. Caveat: this is **this x86 host, not the A35** —
take the ratios, not the absolute times. The device is slower in both columns,
and the alloc-heavy column suffers more from a 32K L1-D.

| payload | `compress2()` | persistent | speedup | compressed size |
|---|---|---|---|---|
| 64 B | 20.39 µs | 1.36 µs | **15.0×** | 37 / 37 |
| 128 B | 21.40 µs | 1.94 µs | 11.0× | 58 / 58 |
| 256 B | 22.61 µs | 2.49 µs | 9.1× | 101 / 101 |
| 512 B | 23.26 µs | 3.27 µs | 7.1× | 159 / 159 |
| 1024 B | 24.58 µs | 5.10 µs | 4.8× | 263 / 263 |
| 3000 B | 30.35 µs | 10.73 µs | 2.8× | 555 / 555 |

Read the first column down: 20.39µs for 64 bytes, 30.35µs for 3000. **About
20µs of every call is fixed setup that has nothing to do with the payload.**
That is the real cost, and it is paid once per recipient on top.

Compressed sizes are byte-identical in every row, so the ratio is unaffected.

### The fix, and why it is the one to do first

Keep one `z_stream` for the life of the process and `deflateReset()` per packet.
`deflateReset` restores the post-`deflateInit` state, so each packet is still an
independent, complete zlib stream — the output is what `compress2` would have
produced.

Use `deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 11, 4, Z_DEFAULT_STRATEGY)`:

- `memLevel` 4 has **no wire representation at all** — purely internal sizing.
- `windowBits` 11 declares a 2KB window in the zlib header's CINFO field.
  `inflate()` rejects only a window *larger* than its own, so a stock peer
  running `uncompress()` (15 bits) decodes it. Verified in the benchmark: a
  windowBits=11 stream fed to a stock `uncompress()` returned `Z_OK`, 512 bytes,
  bytes matching.

Together these cut the one-time allocation ~16× and shrink `deflateReset`'s hash
clear from a 64KB memset to 4KB.

**This is a self-contained change to `packet_compress()` — roughly fifteen lines
in one file, wire-compatible, no protocol bump, no restructuring of
`network_send()`.** It captures 7–15× on every compression whether or not the
dedup ever lands. The dedup from part 1 is still worth doing (it also removes the
per-recipient `send()` syscalls behind `us_netsocket`), but it needs
`network_send()` restructured and is the riskier of the two. **Do the persistent
stream first.**

The two do not add: dedup alone takes ~2.5ms of `us_netcodec` to ~0.3ms;
persistent streams alone take it to ~0.35ms; both together to well under 0.1ms.

### One assumption from part 1 corrected

I expected the receive side to be equally bad, since `packet_decompress()`
(`packet.c:54`) uses the matching one-shot `uncompress()`. It is not:

| payload | `uncompress()` | persistent | speedup |
|---|---|---|---|
| 64 B | 0.18 µs | 0.13 µs | 1.4× |
| 512 B | 0.73 µs | 0.66 µs | 1.1× |
| 3000 B | 4.82 µs | 4.79 µs | 1.0× |

`inflateInit` defers its window allocation, so there is nothing meaningful to
hoist. Decompression is two orders of magnitude cheaper than compression here
and is not worth touching. `us_netcodec` is essentially all send-side.

---

## 3. Three more per-player costs in the network path

All of these are O(players) and none were in part 1.

### 3.1 Reliable-packet bookkeeping is duplicated per recipient

`network_remember_reliable()` (`packet_reliable.c:95`) is called from inside
`network_send_to()`, i.e. **inside** the per-recipient loop. For each recipient it
does:

```c
struct PacketLinkedList* node = calloc(1, sizeof(struct PacketLinkedList));
node->p = *p;
```

`struct PacketLinkedList` embeds a whole `struct Packet`, which is ~3,048 bytes
(`PACKET_LENGTH` is 3000 plus the header fields). So one reliable broadcast in a
9-player room is eight `calloc`s of 3KB and eight 3KB structure copies — ~48KB of
allocate-and-copy for one logical packet.

Then `network_update_reliable()` (`packet_reliable.c:155`) walks that
N-times-longer list **every frame**, calling `clock_elapsed()` twice per node,
and any retransmit goes back through `network_send_to()` for another full
`compress2()`.

Storing one entry per logical broadcast with a per-recipient ack bitmask would
cut both the memory traffic and the per-frame walk by the player count.

### 3.2 Every packet zeroes 3KB it never uses

`struct Packet p = { 0 };` zero-fills all ~3,048 bytes. It appears 59 times in
`src/pc/network/`, including the hottest paths:

- `packet_object.c:367` — `network_send_object_reliability()`, once per **owned
  sync object per frame**. At ~80 sync objects that is ~240KB of memset per frame
  before anything is sent.
- `packet_player.c:233` — the per-frame player state packet.
- `network.c:421` — every received packet, immediately overwritten by
  `packet_decompress()`.
- `packet.c:229` and `packet.c:249` — the duplicate-and-forward paths.

Nothing ever reads past `dataLength + sizeof(u32)`, and every one of those bytes
is written before it is read (`packet_write` advances `cursor`/`dataLength`;
`packet_hash` reads `buffer[0..dataLength)`; the hash is written at
`buffer[dataLength]`; compression reads `dataLength + 4`). Zeroing only the
scalar header members and leaving `buffer` uninitialised is safe and removes the
whole memset. It also cannot leak stack contents on the wire, because only
`dataLength + 4` bytes are ever handed to the compressor.

### 3.3 `sync_object_should_own()` always pays for 16 players

`MAX_PLAYERS` is 16 (`include/types.h:602`), and `sync_object.c:322-326` loops all
16 slots unconditionally — the cost does not fall when only three people are in
the room:

```c
for (s32 i = 0; i < MAX_PLAYERS; i++) {
    if (i != 0 && !is_player_in_local_area(&gMarioStates[i])) { continue; }
    if (player_distance(&gMarioStates[0], so->o) > player_distance(&gMarioStates[i], so->o)) { return false; }
}
```

Three problems, on a loop that runs once per sync object per frame:

1. `player_distance(&gMarioStates[0], so->o)` is loop-invariant and recomputed
   every iteration — up to 16 redundant distance computations per object.
2. `player_distance()` (`sync_object.c:282`) ends in **`sqrt`**, the double
   version, so three `f32`s are promoted to `f64` on a core with no FP64 vector
   unit. Ownership only needs the *ordering*, and the comparison is monotonic in
   the square, so no square root is needed at all. Coordinate magnitudes (~2×10⁴,
   squared sums ~1.2×10⁹) sit comfortably inside `f32`. The one caller that wants
   a real distance is `packet_object.c:518`, where `updateRate = dist / 1000.0f`;
   that one should use `sqrtf`.
3. The `i == 0` iteration compares the local player's distance against itself.

At ~80 sync objects this is ~2,560 distance computations per frame where ~640
would do.

Adjacent, in the same loop: `packet_object.c:528` calls `clock_elapsed()` once
per sync object. `linux-vdso.so.1` is 0.51% of the sampler. Read it once per
frame.

---

## 4. Revised ranking

Replacing part 1's list, ordered by return on risk:

| | change | expected | risk |
|---|---|---|---|
| 1 | Persistent `z_stream` + `deflateReset` in `packet_compress` | ~1.5–2.0 ms | low — one function, wire-compatible, measured 7–15× |
| 2 | Bucket opaque master list by display-list pointer | ~1.5–2.5 ms | medium — reordering, opaque layers only |
| 3 | Compress once per broadcast + `coopnet_send()` | ~1.0–1.3 ms | medium — restructures `network_send()`; mostly recovers `us_netsocket` once (1) has landed |
| 4 | O(1) tri-state guard (`gfx_pc.c:1306`) | ~0.2–0.4 ms | low |
| 5 | Fewer GL calls per shader switch | ~0.4–0.8 ms | low |
| 6 | Drop the 3KB packet memsets (3.2) | ~0.2 ms | low |
| 7 | `sync_object_should_own` hoist + no `sqrt` (3.3) | ~0.1–0.3 ms | low |
| 8 | One reliable entry per broadcast (3.1) | ~0.1–0.3 ms | medium |

Items 1, 4, 5, 6, 7 are all low-risk and total ~2.4–3.7ms on their own.

**What this does not fix.** ~1.2ms per player of `us_hook` is mod bytecode
iterating the player list inside once-per-frame callbacks, and no client-side
engine change touches it. In a 9-player room that is ~9ms. If every item above
lands and the room still misses 33.3ms, the remaining move is not a CPU
optimisation — it is Layer 2, so a client that cannot keep up stops asserting
ownership of sync objects and broadcasting state from a simulation running at the
wrong rate.

## 5. Still open

- Whether `coopnet_send()` in `@9d9b3dd` broadcasts to the lobby or to all peers.
  Blocks item 3 only.
- Which mod hooks carry the per-player loops. `hookCalls` is a single global
  counter; a per-hook-type breakdown (`HOOK_MAX`-sized array of call count and
  accumulated µs, dumped alongside the CSV) would name the callback in one
  session and would say whether it is one mod or all of them. That is the highest
  value instrumentation left, because it is the only route into the ~9ms that
  currently has no owner.
- Both part 1 threads stand: the camera parent-matrix identity check, and Layer
  2's ownership-vacuum design problem.
