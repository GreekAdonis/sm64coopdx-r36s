#pragma once

// Signal-based sampling profiler for the main thread, compiled in only with
// PROFILE=1. Samples the game thread's own CPU clock, so time spent sleeping
// on the frame cap is not sampled and the histogram is CPU work only.
//
// Writes "<log>.samples" next to the CSV: a module map plus a list of
// "<pc> <count>" lines, symbolised offline with tools/profile_report.py.
//
// The list is split into time windows. Run 12 is why: a 1957-second session
// spent 23 minutes sitting in the menu and 30 seconds in the room that was
// actually being investigated, and because the table was a single whole-session
// aggregate, the menu swamped the interesting part completely -- the sampled
// profile could not corroborate anything the per-frame CSV said about those 30
// seconds. Windows make the dump sliceable after the fact, so a bad patch can be
// looked at on its own with --since/--until.

#ifdef PROFILE_BUILD

// `path` is where the dump is written; the file is opened here so that windows
// can be appended as the session runs and a killed process still leaves behind
// everything up to the last window boundary.
void profile_sample_init(const char *path, double windowSeconds);

// Closes the current window if `nowSeconds` has crossed its end. Cheap enough to
// call once per frame (a float compare on all but one frame in a window).
// `nowSeconds` is seconds since profiling started, i.e. the CSV's `time_s`, so
// window bounds and frame rows share one clock.
void profile_sample_tick(double nowSeconds);

// Flushes the final partial window and closes the file.
void profile_sample_dump(double nowSeconds);

#else

#define profile_sample_init(_p, _w) ((void)0)
#define profile_sample_tick(_t) ((void)0)
#define profile_sample_dump(_t) ((void)0)

#endif
