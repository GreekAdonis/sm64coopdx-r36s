# Profiling a play session on the handheld

`FRAME_PROFILE=1` builds the game with two measurement tools bolted on:

1. **A per-frame CSV.** One row per game frame: where that frame's time went,
   broken down through the game loop and the renderer, plus draw-call,
   triangle, vertex and texture counters, plus which level/area you were in.
2. **A sampling profiler.** A timer interrupts the game thread 200 times per
   second and records the program counter. Over a few minutes that is tens of
   thousands of samples, which is enough to say *which functions* the CPU is
   actually in, rather than reasoning about which ones ought to be slow.

Nothing else about the build changes — the optimisation flags, the target and
every gameplay define are the same as a release build, so the numbers describe
the binary people actually run. `DEVELOPMENT=1` is deliberately *not* used: it
changes gameplay code and the network version string.

## Build it

Same container as always, plus `FRAME_PROFILE=1`:

```sh
docker run --rm --platform linux/arm64 \
  -v "$PWD:/build" -w /build debian:bullseye bash -lc '
    apt-get update &&
    apt-get install -y --no-install-recommends \
      build-essential python3 libglew-dev libsdl2-dev libz-dev \
      libcurl4-openssl-dev bsdmainutils &&
    rm -rf build/us_pc &&
    make HANDHELD=1 DISCORD_SDK=0 UPDATER=0 \
      TARGET_RK3326=1 LUA_SOURCE=1 FRAME_PROFILE=1 \
      EXTRA_CPP_FLAGS="-std=c++17" -j$(nproc)
  '
```

`rm -rf build/us_pc` matters: changing a `-D` does not invalidate existing
object files, so a stale build silently links half-instrumented objects.

**Keep the resulting `build/us_pc/sm64coopdx.arm` around.** The sampled
profile is a list of addresses; turning it back into function names needs that
exact unstripped binary. (Linux builds are not stripped, so nothing extra is
needed — just don't overwrite it before the log is analysed.)

## Run it

Copy the binary to the handheld as usual and play. It writes into the current
working directory, which for the ArkOS launcher is
`/roms/ports/sm64coopdx/`:

| File | What it is |
|---|---|
| `sm64coopdx-profile.csv` | one row per game frame |
| `sm64coopdx-profile.csv.samples` | the sampled program counters |

Environment variables, all optional:

| Variable | Default | Meaning |
|---|---|---|
| `SM64_PROFILE_LOG` | `sm64coopdx-profile.csv` | output path; `0` or `off` disables logging entirely |
| `SM64_PROFILE_SAMPLE` | on | `0` or `off` turns off the sampling profiler |
| `SM64_PROFILE_SAMPLE_HZ` | `200` | sample rate, capped at 1000 |

Setting `ctx_profiler true` in `sm64config.txt` additionally shows the live
per-context breakdown on screen, which is handy for confirming the numbers
match what you feel.

Collecting good data:

- **Play the levels that actually lag**, and stay in each for a decent stretch
  — the report groups by level and ignores groups shorter than 30 frames.
- **Note roughly when** each bad patch happened. Every row is timestamped, so
  "it stuttered about a minute in" is enough to find it.
- **Quit through the menu if you can.** The log is flushed every 128 frames
  and on `SIGTERM`/`SIGINT`, so a kill loses at most a few seconds, but a
  clean exit is tidiest.
- Multiplayer changes the answer: the row records the connected player count,
  so a solo run and a co-op run are worth doing separately.

## Read it

```sh
tools/profile_report.py sm64coopdx-profile.csv \
    --binary build/us_pc/sm64coopdx.arm
```

Useful flags: `--since <seconds>` to skip the menu and loading, `--top N` to
widen the worst-frame and hot-function lists.

The frame breakdown is nested — indented rows are *inside* the row above, so
they double-count on purpose:

```
TOTAL                     the whole frame, including the frame-cap wait
  frame-cap wait          deliberate idle; TOTAL minus this is the real work
  network
  game loop
    level script
      object update       behaviour scripts, object collision
      geo / scene graph   the walk that builds the display list
  lua update
    lua hooks
  audio
  render
    interpolation
    display list -> GL    CPU-side DL interpretation and GL call submission
      lighting engine
      texture upload      CPU repack + glTexImage2D
    swap buffers          where a GPU-bound frame stalls
```

How to read the result:

- **`frame-cap wait` is large** — the device is keeping up in that section;
  whatever else is in the row is not the problem.
- **`swap buffers` dominates and the CPU rows are small** — GPU-bound. Look at
  fill rate (internal resolution), overdraw, and the blit.
- **`display list -> GL` dominates** — CPU-bound in the renderer. `draws` and
  `tris` in the counters say whether it is submission overhead or volume.
- **`geo / scene graph` dominates** — the scene graph walk, i.e. too much
  geometry being considered, not too much being drawn.
- **`object update` dominates** — behaviours and object collision; correlate
  with the `objects` counter.
- **`lua hooks` dominates** — a mod. The in-game Lua profiler
  (`lua_profiler true`) attributes it per mod, and the "lua traffic" table
  says whether that time is reachable from the engine at all. `us per call`
  is `us_hook` divided by the number of `smlua_call_hook()` invocations: a
  few microseconds means the binding's per-call overhead dominates and fixing
  it helps every mod at once, while hundreds of microseconds means the time is
  inside mod bytecode and no client-side change will reach it. `field
  accesses per call` counts the cobject `__index`/`__newindex` path, which
  scales with how hard a mod pokes at object fields rather than with how much
  work it does.
- **`display list -> GL` is large but `tris` is small** — batch fragmentation,
  not geometry. Read the "what split the batches" table: it attributes every
  draw call to the state change that forced it (`fltexture`, `flshader`,
  `flalpha`, `fldepth`, `flsampler`, `flviewport`, `flfull`, `flcomb`). A draw
  call costs the same whatever caused it, so the largest row is the one worth
  attacking, and a cause near zero is not worth touching however slow it looks
  in the source. `flfull` is the healthy one — it means a batch filled up.
- **`texflushes` is ever non-zero** — the texture cache is thrashing, which
  makes `texloads`/`texbytes` explode. That is a fixable configuration
  problem, not a hardware limit.
- **A large share of samples in `libmali`** — the driver is doing the work,
  which usually means state changes or uploads rather than shading.

## What it costs

The instrumentation is off in a normal build: the timing macros and counters
compile to nothing, so a release binary is unchanged.

In a profile build, per frame: about two dozen `clock_gettime` calls (~30ns
each on this hardware), a handful of counter increments in the renderer's hot
paths, and one buffered `fprintf`. That is well under 0.1ms against a 33ms
budget. The sampler adds 200 signal deliveries per second of *CPU* time (it
uses the thread's own CPU clock, so it does not fire while the frame cap is
sleeping), which is roughly 0.1% of a core.

The one thing to watch: the sampler interrupts the main thread, so a syscall
can return `EINTR` where it previously never did. The handler is installed
with `SA_RESTART`, and the timer is aimed at the game thread only so audio and
network threads are never interrupted. If anything looks off during a session
— netplay hiccups, audio glitches — set `SM64_PROFILE_SAMPLE=0` and rerun; the
CSV alone is still useful.
