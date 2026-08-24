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

COUNTERS = ["subframes", "draws", "tris", "verts", "texloads", "texbytes",
            "texflushes", "binds", "bindskips", "impskips", "shaders", "objects",
            "players", "hookcalls", "hookbhv", "fieldgets", "fieldsets"]

# Why each batch split happened. Absent from older logs; the printer skips
# whatever a given CSV does not carry.
FLUSH_CAUSES = ["fltexture", "flshader", "flalpha", "fldepth", "flsampler",
                "flviewport", "flfull", "flcomb"]


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

    if "hookcalls" in rows[0]:
        # The decisive ratio. us_hook is time spent inside smlua_call_hook() and
        # hookcalls counts exactly those calls, so their quotient says where the
        # time actually is. A few microseconds each means the engine's per-call
        # overhead dominates, which is worth fixing once for every mod. Hundreds
        # of microseconds each means the time is inside mod bytecode, where no
        # client-side change reaches it.
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
        if calls:
            per = hook_us / calls
            verdict = ("per-call overhead dominates; engine-side work helps every mod"
                       if per < 20 else
                       "time is inside mod bytecode; engine-side work will not reach it")
            print(f"  -> us per call    {per:10.2f}   {verdict}")
            print(f"  -> field accesses per call {(gets+sets)/calls:10.1f}")

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


def report_samples(path, binary, top_n):
    if not os.path.exists(path):
        return
    print("\n" + "=" * 78)
    print("SAMPLED PROFILE (main thread CPU time)")
    print("=" * 78)

    header, maps, samples = {}, [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("map "):
                _, start, end, base, name = line.split(" ", 4)
                maps.append((int(start, 16), int(end, 16), int(base, 16), name))
                continue
            if line.startswith("0x"):
                pc, count = line.split()
                samples.append((int(pc, 16), int(count)))
                continue
            k, _, v = line.partition(" ")
            header[k] = v

    total = sum(c for _, c in samples)
    if not total:
        print("  no samples recorded")
        return
    print(f"  {total} samples at {header.get('hz','?')}Hz "
          f"(~{total/max(1,int(header.get('hz',1))):.0f}s of main-thread CPU time)")
    for k in ("dropped_other_thread", "dropped_table_full", "dropped_no_pc"):
        if header.get(k, "0") != "0":
            print(f"  {k}: {header[k]}")

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--samples", default=None,
                    help="defaults to <csv>.samples")
    ap.add_argument("--binary", default=None,
                    help="unstripped build that produced the samples")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--since", type=float, default=None,
                    help="ignore frames before this timestamp (skip menus/loading)")
    args = ap.parse_args()

    meta, rows = read_csv(args.csv)
    if args.since is not None:
        rows = [r for r in rows if r["time_s"] >= args.since]
    if not rows:
        print("no frames in log")
        return 1

    report_frames(meta, rows, args.top)
    report_levels(rows)
    report_samples(args.samples or args.csv + ".samples", args.binary, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
