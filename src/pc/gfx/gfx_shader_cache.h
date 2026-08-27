#pragma once

#include <stdint.h>
#include <stdbool.h>

// On-disk cache of linked shader program binaries.
//
// Every colour combiner the game meets for the first time is turned into GLSL
// and handed to the driver to compile and link, synchronously, on the game
// thread. Run 23 measured what that costs on the RK3326's Mali-G31: 57 programs
// were built across a half-hour session, and the frames that built them averaged
// 282ms in the display-list path against a 12.5ms baseline -- roughly 150-215ms
// per program, with a worst single frame of 1.29 seconds. It is not a warm-up
// cost that is over once a level has loaded, either; new combiners kept turning
// up a minute into a level as fresh materials came into view.
//
// The compile result is deterministic for a given driver and a given pair of
// shader sources, so it only has to be paid once per device rather than once per
// launch. GL_OES_get_program_binary (core in ES 3.0, ARB_get_program_binary on
// desktop) hands back the linked binary; storing those and feeding them to
// glProgramBinary on the next run turns a ~180ms compile into a memcpy.
//
// Nothing here is required for correctness. If the extension is missing, the
// file is unreadable, the driver has changed underneath us or a binary is simply
// rejected, every entry point degrades to "not cached" and the caller compiles
// from source exactly as before.

// Resolves the extension entry points and reads any existing cache file.
// Safe to call when the extension is absent; the cache is then inert.
void gfx_shader_cache_init(void);

// Tries to populate `program` (a fresh glCreateProgram handle) from the cached
// binary for `sourceHash`. Returns true only if the program came back linked and
// usable, in which case the caller must not compile or link it itself.
bool gfx_shader_cache_load(uint64_t sourceHash, uint32_t program);

// Records the binary of a freshly linked `program` against `sourceHash`. The
// file is not rewritten here -- see gfx_shader_cache_flush().
void gfx_shader_cache_store(uint64_t sourceHash, uint32_t program);

// Writes the cache back to disk if anything was added since it was read. Called
// on shutdown; cheap and safe to call when nothing has changed.
void gfx_shader_cache_flush(void);

// Frees the in-memory copy. Does not write; call gfx_shader_cache_flush() first
// if that is wanted.
void gfx_shader_cache_shutdown(void);

// FNV-1a over a NUL-terminated string, exposed so the shader generator can hash
// the sources it just built without duplicating the constants.
uint64_t gfx_shader_cache_hash_source(const char *vertexSource, const char *fragmentSource);
