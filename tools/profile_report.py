#!/usr/bin/env python3
"""Summarise a FRAME_PROFILE=1 play session.

Reads the per-frame CSV written by src/pc/profile_log.c and, if present, the
sampled program counters written next to it by src/pc/profile_sample.c.

    tools/profile_report.py sm64coopdx-profile.csv
    tools/profile_report.py sm64coopdx-profile.csv --binary build/us_pc/sm64coopdx.arm

The binary is only needed to turn sampled addresses into function names; it must
be the same unstripped build that produced the samples.
"""

import argparse
import bisect
import csv
import os
import subprocess
import sys
from collections import defaultdict

# columns that are microsecond timings, in the order they are reported
TIMINGS = [
    ("us_total", "TOTAL (whole frame, includes the frame-cap wait)"),
    ("us_delay", "  frame-cap wait (idle)"),
    ("us_net", "  network"),
    ("us_netcodec", "    packet compress/decompress (threadable)"),
    ("us_netsocket", "    send syscall (threadable)"),
    ("us_netrecv", "    receive / coopnet update"),
    ("us_game", "  game loop"),
    ("us_levelscript", "    level script"),
    ("us_objects", "      object update"),
    ("us_geo", "      geo / scene graph (builds the display list)"),
    ("us_smlua", "  lua update"),
    ("us_hook", "    lua hooks"),
    ("us_audio", "  audio"),
    ("us_render", "  render (includes the wait)"),
    ("us_interp", "    interpolation"),
    ("us_gfxdl", "    display list -> GL calls"),
    ("us_lighting", "      lighting engine"),
    ("us_texupload", "      texture upload"),
    ("us_swap", "    swap buffers (GPU stall shows up here)"),
]

COUNTERS = ["subframes", "draws", "tris", "verts", "triscliprej", "triscullrej",
            "texloads", "texbytes",
            "texflushes", "binds", "bindskips", "impskips", "shaders", "objects",
            "players", "hookcalls", "hookbhv", "fieldgets", "fieldsets", "fieldmemo",
            "objsdrawn", "objsculled", "objsculledsize",
            "dlnodes", "dldistinct", "renderskips"]

# Why each batch split happened. Absent from older logs; the printer skips
# whatever a given CSV does not carry.
FLUSH_CAUSES = ["fltexture", "flshader", "flalpha", "fldepth", "flsampler",
                "flviewport", "flfull", "flcomb"]

# Draws and texture splits divided by whether their pass can be reordered.
# Absent from logs written before run 17.
REORDER_SPLIT = ["drawsopaque", "drawsblend", "fltexopaque", "fltexblend"]


def pct(values, p):
    if not values:
        return 0
    s = sorted(values)
    k = min(len(s) - 1, max(0, int(round((p / 100.0) * (len(s) - 1)))))
    return s[k]


def mean(values):
    return sum(values) / len(values) if values else 0


def read_csv(path):
    rows = []
    meta = ""
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("#"):
                meta += line[1:].strip()
    with open(path, newline="") as f:
        body = (l for l in f if not l.startswith("#"))
        for row in csv.DictReader(body):
            try:
                rows.append({k: (float(v) if k == "time_s" else int(v))
                             for k, v in row.items() if v != ""})
            except (TypeError, ValueError):
                continue  # truncated last line if the game was killed mid-write
    return meta, rows


def busy(row):
    """Frame time minus the deliberate frame-cap wait: the real work."""
    return row["us_total"] - row["us_delay"]


def report_frames(meta, rows, top_n):
    print("=" * 78)
    print("FRAME TIMING")
    print("=" * 78)
    if meta:
        print(meta)
    dur = rows[-1]["time_s"] - rows[0]["time_s"] if len(rows) > 1 else 0
    print(f"{len(rows)} frames over {dur:.1f}s "
          f"({len(rows)/dur:.1f} game frames/s average)" if dur else f"{len(rows)} frames")

    busies = [busy(r) for r in rows]
    totals = [r["us_total"] for r in rows]
    budget = 33333  # a 30fps frame
    over = [b for b in busies if b > budget]
    print(f"\nwork per frame (us, excluding the frame-cap wait):")
    print(f"  mean {mean(busies):8.0f}   p50 {pct(busies,50):8.0f}   "
          f"p90 {pct(busies,90):8.0f}   p99 {pct(busies,99):8.0f}   max {max(busies):8.0f}")
    print(f"  frames whose work exceeded the 33.3ms budget: "
          f"{len(over)} / {len(rows)} ({100.0*len(over)/len(rows):.1f}%)")
    print(f"  wall frame time: mean {mean(totals):.0f}us -> {1e6/mean(totals):.1f} fps")

    print("\nwhere the time goes (us per frame; indented rows are inside the row above):")
    print(f"  {'':52} {'mean':>8} {'p50':>8} {'p99':>8}")
    for key, label in TIMINGS:
        if key not in rows[0]:
            continue
        vals = [r[key] for r in rows]
        share = 100.0 * mean(vals) / mean(busies) if mean(busies) else 0
        tag = f"{share:5.1f}%" if key not in ("us_total", "us_delay") else "      "
        print(f"  {label:52} {mean(vals):8.0f} {pct(vals,50):8.0f} {pct(vals,99):8.0f}  {tag}")

    print("\ncounters per frame:")
    for key in COUNTERS:
        if key not in rows[0]:
            continue
        vals = [r[key] for r in rows]
        print(f"  {key:12} mean {mean(vals):10.1f}   p50 {pct(vals,50):8}   max {max(vals):8}")

    present = [k for k in FLUSH_CAUSES if k in rows[0]]
    if present:
        # A draw call costs the same whatever forced it, so the biggest row here
        # is the one worth attacking -- and a cause that barely registers is not
        # worth optimising however slow it looks in the source.
        totals = {k: sum(r[k] for r in rows) for k in present}
        grand = sum(totals.values()) or 1
        print("\nwhat split the batches (draw calls per frame, by cause):")
        for key in sorted(present, key=lambda k: -totals[k]):
            vals = [r[key] for r in rows]
            print(f"  {key:12} mean {mean(vals):8.1f}   p50 {pct(vals,50):6}   "
                  f"max {max(vals):6}   {100.0*totals[key]/grand:5.1f}% of splits")

    if all(k in rows[0] for k in REORDER_SPLIT):
        # Whether batching by texture is worth attempting at all.
        #
        # Draw submission measured 50.9us per call on the RK3326 (run 16, R^2
        # 0.991 against draws and verts), so cutting draw calls is the lever --
        # but sorting draws only helps where the order is free to change.
        # Depth-writing passes resolve by z-buffer; blended ones must stay
        # back-to-front. Only the opaque half is addressable.
        #
        # The floor for perfect same-texture batching is the number of distinct
        # display lists, so (fltexopaque - its share of dldistinct) is roughly
        # what a reorder could reclaim. If fltexblend holds most of the churn,
        # the idea does not survive contact with this table.
        n = len(rows)
        do = sum(r["drawsopaque"] for r in rows) / n
        db = sum(r["drawsblend"] for r in rows) / n
        to = sum(r["fltexopaque"] for r in rows) / n
        tb = sum(r["fltexblend"] for r in rows) / n
        print("\nreorderable work per frame (can draw order change?):")
        print(f"  draw calls, depth-writing  {do:10.1f}   reorderable")
        print(f"  draw calls, blended        {db:10.1f}   must keep order")
        print(f"  texture splits, depth-writing {to:7.1f}   recoverable by batching")
        print(f"  texture splits, blended       {tb:7.1f}   not recoverable")
        if to + tb > 0:
            print(f"  -> {100.0*to/(to+tb):.0f}% of texture splits are in reorderable passes")
        US_PER_DRAW = 50.9
        if do + db > 0:
            print(f"  -> at {US_PER_DRAW:.0f}us/draw, the reorderable draws cost "
                  f"{US_PER_DRAW*do/1000:.1f}ms per frame")

    if "hookcalls" in rows[0]:
        # us_hook divided by hookcalls says how much of a hook call is the
        # binding's own dispatch. A few microseconds each means dispatch
        # dominates and fixing it helps every mod at once.
        #
        # It does NOT say the rest is unreachable. us_hook is wall time inside
        # smlua_pcall(), so a large per-call figure includes every engine
        # function the callback invoked through the bindings. Run 15 had
        # add_surface() at 32% of CPU reached from mod behaviours, fixed
        # engine-side. Use the windowed sampled profile to tell mod bytecode
        # (luaV_execute, luaH_get, internshrstr, GC) from engine code that mods
        # merely call (smlua_*, dynos_*, obj_*, find_*).
        n = len(rows)
        calls = sum(r["hookcalls"] for r in rows)
        bhv = sum(r["hookbhv"] for r in rows)
        gets = sum(r["fieldgets"] for r in rows)
        sets = sum(r["fieldsets"] for r in rows)
        hook_us = sum(r.get("us_hook", 0) for r in rows)
        print("\nlua traffic per frame:")
        print(f"  hook calls        {calls/n:10.1f}   ({bhv/n:.1f} per-object behaviour)")
        print(f"  field gets        {gets/n:10.1f}")
        print(f"  field sets        {sets/n:10.1f}")
        if "fieldmemo" in rows[0] and (gets + sets) > 0:
            memo = sum(r["fieldmemo"] for r in rows)
            print(f"  field memo hits   {memo/n:10.1f}   "
                  f"({100.0*memo/(gets+sets):.1f}% of accesses skipped the index)")
        if calls:
            per = hook_us / calls
            verdict = ("binding dispatch dominates; fixing it helps every mod"
                       if per < 20 else
                       "callback bodies dominate -- check the sampled profile for "
                       "engine functions they call")
            print(f"  -> us per call    {per:10.2f}   {verdict}")
            print(f"  -> field accesses per call {(gets+sets)/calls:10.1f}")

    # What the geo pass handed the renderer. Absent from older logs.
    if any("dlnodes" in r for r in rows):
        n = len(rows)
        alive = sum(r.get("objects", 0) for r in rows)
        drawn = sum(r.get("objsdrawn", 0) for r in rows)
        nodes = sum(r.get("dlnodes", 0) for r in rows)
        distinct = sum(r.get("dldistinct", 0) for r in rows)
        print("\ngeo pass output per frame:")
        print(f"  objects alive     {alive/n:10.1f}")
        print(f"  objects drawn     {drawn/n:10.1f}", end="")
        if alive:
            culled = 100.0 * (1.0 - drawn / alive)
            print(f"   ({culled:.0f}% culled before the renderer sees them)")
        else:
            print()
        skips = sum(r.get("renderskips", 0) for r in rows)
        if skips:
            nsk = sum(1 for r in rows if r.get("renderskips", 0))
            print(f"  renders dropped   {skips/n:10.2f}   (on {100.0*nsk/n:.1f}% of frames,"
                  f" to hold the simulation at wall-clock 30Hz)")

        # The cost of a tick with the render taken out. Above the 33.3ms budget,
        # dropping renders cannot reach 30Hz however many are dropped, so the
        # trade the policy is making has nothing left to buy.
        simonly = [r["simonly_us"] for r in rows if r.get("simonly_us", 0) > 0]
        if simonly:
            m = mean(simonly)
            print(f"  simulation-only   {m/1000.0:10.1f}ms per tick", end="")
            if m > 33333:
                print(f"   ({m/33333.0:.2f}x the budget -- dropping renders"
                      f" cannot reach 30Hz; ceiling is {1e6/m:.1f} Hz)")
            else:
                print(f"   ({m/33333.0:.2f}x the budget)")
        print(f"  display lists     {nodes/n:10.1f}")
        print(f"  distinct of those {distinct/n:10.1f}", end="")
        if nodes:
            share = distinct / nodes
            verdict = ("instances share display lists; batching by display list has"
                       " something to collapse" if share < 0.5 else
                       "display lists are close to per-instance; batching by display"
                       " list would not help")
            print(f"   ({share:.2f} per node -- {verdict})")
        else:
            print()

    print(f"\nworst {top_n} frames by work:")
    worst = sorted(rows, key=busy, reverse=True)[:top_n]
    for r in worst:
        parts = [(k, r[k]) for k, _ in TIMINGS
                 if k not in ("us_total", "us_delay") and k in r]
        parts.sort(key=lambda kv: kv[1], reverse=True)
        blame = " ".join(f"{k[3:]}={v}" for k, v in parts[:4])
        print(f"  t={r['time_s']:8.2f}s lvl={r['level']:>3} area={r['area']} "
              f"work={busy(r):7}us tris={r.get('tris',0):6} draws={r.get('draws',0):5}  {blame}")


def report_levels(rows):
    print("\n" + "=" * 78)
    print("BY LEVEL / AREA")
    print("=" * 78)
    groups = defaultdict(list)
    for r in rows:
        groups[(r["level"], r["area"])].append(r)
    print(f"  {'level':>5} {'area':>4} {'frames':>7} {'p50 work':>9} {'p99 work':>9} "
          f"{'fps':>6} {'tris':>7} {'draws':>6}")
    for key in sorted(groups, key=lambda k: -pct([busy(r) for r in groups[k]], 50)):
        g = groups[key]
        if len(g) < 30:
            continue  # ignore transitions
        b = [busy(r) for r in g]
        t = [r["us_total"] for r in g]
        print(f"  {key[0]:>5} {key[1]:>4} {len(g):>7} {pct(b,50):>9.0f} {pct(b,99):>9.0f} "
              f"{1e6/mean(t):>6.1f} {mean([r.get('tris',0) for r in g]):>7.0f} "
              f"{mean([r.get('draws',0) for r in g]):>6.0f}")


def load_symbols(binary):
    """[(addr, name)] sorted by addr, from the binary's symbol table."""
    try:
        out = subprocess.run(["nm", "-C", "--defined-only", "-n", binary],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"  (could not read symbols from {binary}: {e})")
        return []
    syms = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1].lower() in "tw":
            try:
                syms.append((int(parts[0], 16), parts[2]))
            except ValueError:
                pass
    return syms


def report_samples(path, binary, top_n, since=None, until=None):
    if not os.path.exists(path):
        return
    print("\n" + "=" * 78)
    print("SAMPLED PROFILE (main thread CPU time)")
    print("=" * 78)

    # The dump is a module map followed by one or more windows:
    #
    #   window <idx> <start_s> <end_s> <taken> <other> <full> <nopc>
    #   0x<pc> <count>
    #   ...
    #
    # A window is kept if it overlaps [since, until). Windows are whole units --
    # there is no way to split one, since a sample carries no timestamp of its
    # own -- so a narrow --since/--until still pulls in the windows straddling
    # the edges. Older dumps have no "window" line at all; those parse as one
    # window spanning the session, which is exactly what they were.
    header, maps = {}, []
    windows = []          # (start, end, drops{}, [(pc, count)])
    cur = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("map "):
                _, start, end, base, name = line.split(" ", 4)
                maps.append((int(start, 16), int(end, 16), int(base, 16), name))
                continue
            if line.startswith("window "):
                p = line.split()
                cur = (float(p[2]), float(p[3]),
                       {"dropped_other_thread": int(p[5]),
                        "dropped_table_full": int(p[6]),
                        "dropped_no_pc": int(p[7])},
                       [])
                windows.append(cur)
                continue
            if line.startswith("0x"):
                pc, count = line.split()
                if cur is None:
                    cur = (0.0, float("inf"),
                           {k: int(header.get(k, 0))
                            for k in ("dropped_other_thread", "dropped_table_full",
                                      "dropped_no_pc")},
                           [])
                    windows.append(cur)
                cur[3].append((int(pc, 16), int(count)))
                continue
            k, _, v = line.partition(" ")
            header[k] = v

    lo = -float("inf") if since is None else since
    hi = float("inf") if until is None else until
    kept = [w for w in windows if w[1] > lo and w[0] < hi]

    samples = [pc_count for w in kept for pc_count in w[3]]
    total = sum(c for _, c in samples)
    if not total:
        print("  no samples recorded in this window")
        return
    print(f"  {total} samples at {header.get('hz','?')}Hz "
          f"(~{total/max(1,int(header.get('hz',1))):.0f}s of main-thread CPU time)")
    if len(windows) > 1:
        span = f"{kept[0][0]:.0f}s-{kept[-1][1]:.0f}s" if kept else "none"
        print(f"  {len(kept)} of {len(windows)} windows ({span})")
        if since is not None or until is not None:
            print("  note: windows are whole; the edges may extend past the range asked for")
    for k in ("dropped_other_thread", "dropped_table_full", "dropped_no_pc"):
        n = sum(w[2].get(k, 0) for w in kept)
        if n:
            print(f"  {k}: {n}")

    def module_of(pc):
        for start, end, base, name in maps:
            if start <= pc < end:
                return name, base
        return "(unknown)", 0

    by_module = defaultdict(int)
    for pc, count in samples:
        by_module[module_of(pc)[0]] += count
    print("\n  by shared object:")
    for name, count in sorted(by_module.items(), key=lambda kv: -kv[1]):
        print(f"    {100.0*count/total:6.2f}%  {os.path.basename(name) or name}")

    if not binary:
        print("\n  (pass --binary <the unstripped build> for function names)")
        return
    syms = load_symbols(binary)
    if not syms:
        return
    addrs = [a for a, _ in syms]

    by_func = defaultdict(int)
    for pc, count in samples:
        name, base = module_of(pc)
        if name != "(main)":
            by_func[f"[{os.path.basename(name)}]"] += count
            continue
        i = bisect.bisect_right(addrs, pc - base) - 1
        by_func[syms[i][1] if i >= 0 else "??"] += count

    print(f"\n  top {top_n} functions (self time):")
    for name, count in sorted(by_func.items(), key=lambda kv: -kv[1])[:top_n]:
        print(f"    {100.0*count/total:6.2f}%  {count:7}  {name}")


def report_hooks(path, frames, since=None, until=None):
    """Per-hook-type call counts and time, written by smlua_hooks.c.

    us_hook says how much of a frame went into mod callbacks; this says which
    ones. Hook call volume barely moves with player count while hook time
    climbs, so the interesting column is us/call -- a hook whose body walks the
    player list shows up as a small number of very expensive calls.

    Written as time windows (see profile_log.c). Only the windows overlapping
    [since, until) are summed, so the table can be pinned to the stretch of play
    being investigated rather than averaged over a whole session -- which for
    run 12 meant 30 seconds of the room that mattered against 23 minutes of menu.
    Windows are whole, so a narrow range still pulls in the ones at the edges.
    """
    try:
        with open(path) as f:
            lines = [l.rstrip("\n") for l in f]
    except OSError:
        return

    lo = -float("inf") if since is None else since
    hi = float("inf") if until is None else until

    # (start, end, {hook: [calls, us]}). A file with no "window" line is a dump
    # from before windowing existed: one window spanning the session.
    windows = []
    cur = None
    for line in lines:
        if not line or line.startswith("hook,"):
            continue
        if line.startswith("window "):
            p = line.split()
            cur = (float(p[2]), float(p[3]), {})
            windows.append(cur)
            continue
        if cur is None:
            cur = (0.0, float("inf"), {})
            windows.append(cur)
        parts = line.split(",")
        if len(parts) != 3:
            continue
        try:
            calls, us = int(parts[1]), float(parts[2])
        except ValueError:
            continue
        slot = cur[2].setdefault(parts[0], [0, 0.0])
        slot[0] += calls
        slot[1] += us

    kept = [w for w in windows if w[1] > lo and w[0] < hi]
    merged = {}
    for _, _, hooks in kept:
        for name, (calls, us) in hooks.items():
            slot = merged.setdefault(name, [0, 0.0])
            slot[0] += calls
            slot[1] += us

    entries = [(n, c, u) for n, (c, u) in merged.items() if c]
    if not entries:
        return

    total_us = sum(e[2] for e in entries)
    scope = "whole session" if len(kept) == len(windows) else \
            f"{kept[0][0]:.0f}s-{kept[-1][1]:.0f}s, {len(kept)} of {len(windows)} windows"
    print("\n" + "=" * 78)
    print(f"HOOK TYPES ({scope})")
    print("=" * 78)
    print(f"  {total_us/1e6:.1f}s across {sum(e[1] for e in entries)} calls"
          f" over {frames} frames")
    # An un-windowed dump cannot be narrowed, so its totals still cover the whole
    # session while `frames` counts only the selected ones -- which makes us/frame
    # meaningless rather than merely approximate. Say so instead of printing it
    # as though it lined up.
    if len(windows) == 1 and (since is not None or until is not None):
        print("  NOTE: this dump has no windows, so these totals are for the whole"
              " session\n        and us/frame does not match the frame range"
              " selected. Re-record to\n        slice it (see"
              " SM64_PROFILE_SAMPLE_WINDOW).")
    print()
    print(f"  {'hook':<44} {'calls':>9} {'us/frame':>9} {'us/call':>8} {'share':>6}")
    for name, calls, us in sorted(entries, key=lambda e: -e[2]):
        print(f"  {name:<44} {calls:>9} {us/frames:>9.1f} {us/calls:>8.1f}"
              f" {100.0*us/total_us:>5.1f}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--samples", default=None,
                    help="defaults to <csv>.samples")
    ap.add_argument("--hooks", default=None,
                    help="defaults to <csv>.hooks")
    ap.add_argument("--binary", default=None,
                    help="unstripped build that produced the samples")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--since", type=float, default=None,
                    help="ignore frames before this timestamp (skip menus/loading)")
    ap.add_argument("--until", type=float, default=None,
                    help="ignore frames after this timestamp; with --since this"
                         " narrows the whole report to one stretch of play")
    args = ap.parse_args()

    meta, rows = read_csv(args.csv)
    if args.since is not None:
        rows = [r for r in rows if r["time_s"] >= args.since]
    if args.until is not None:
        rows = [r for r in rows if r["time_s"] <= args.until]
    if not rows:
        print("no frames in log")
        return 1

    report_frames(meta, rows, args.top)
    report_levels(rows)
    report_samples(args.samples or args.csv + ".samples", args.binary, args.top,
                   args.since, args.until)
    report_hooks(args.hooks or args.csv + ".hooks", len(rows),
                 args.since, args.until)
    return 0


if __name__ == "__main__":
    sys.exit(main())
