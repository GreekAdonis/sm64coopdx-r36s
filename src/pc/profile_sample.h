#pragma once

// Signal-based sampling profiler for the main thread, compiled in only with
// PROFILE=1. Samples the game thread's own CPU clock, so time spent sleeping
// on the frame cap is not sampled and the histogram is CPU work only.
//
// Writes "<log>.samples" next to the CSV: a base address plus a list of
// "<pc> <count>" lines, symbolised offline with tools/profile_report.py.

#ifdef PROFILE_BUILD

void profile_sample_init(void);
void profile_sample_dump(const char *path);

#else

#define profile_sample_init()   ((void)0)
#define profile_sample_dump(_p) ((void)0)

#endif
