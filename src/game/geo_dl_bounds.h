#pragma once

#include <stdbool.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "types.h"

// Model-space bounding boxes for static display lists, so the geo pass can
// frustum-cull the level's own geometry the way it already culls objects.
//
// Only objects were ever culled. A level's static display lists were appended to
// the master lists in full, every frame, whatever the camera was pointing at,
// and the waste showed up at the far end of the pipeline: gfx_sp_vertex()
// transformed and lit every vertex, then gfx_sp_tri1() threw the triangle away
// on the clip test. Run 23 measured 1,372 triangles per subframe rejected that
// way against 1,532 actually drawn -- close to one wasted triangle for every
// useful one -- and matched-workload comparisons (same drawn-triangle count, low
// versus high clip rejection) put it at 16-53% of the display-list pass. It is
// worst exactly where it hurts: 3.8 rejected per drawn in one custom level,
// 2.1 in THI, against 0.21 in Castle Grounds.
//
// The bounds are computed once per display list by walking it for its G_VTX
// commands, then cached. Everything here is fail-safe: a list this cannot
// confidently bound -- an opcode it does not model, one bigger than the walk
// limits, one with no vertices at all -- is reported as unbounded, and an
// unbounded list is always drawn. The worst outcome of a miss is the behaviour
// that existed before this file.

// Fills `outCenter` and `outRadius` with a model-space bounding sphere for
// `displayList`, computing and caching it on first sight. Returns false if the
// list cannot be bounded, in which case the caller must draw it unconditionally.
bool geo_dl_bounds_get(const void *displayList, Vec3f outCenter, f32 *outRadius);

// Drops every cached entry. Display lists are static data, but a texture pack or
// model pack unloading can free one and hand the same address back for something
// else, and a stale box would then cull live geometry.
void geo_dl_bounds_reset(void);
