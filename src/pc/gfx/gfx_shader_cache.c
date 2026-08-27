#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif

#ifdef __MINGW32__
# define FOR_WINDOWS 1
#else
# define FOR_WINDOWS 0
#endif

#if FOR_WINDOWS || defined(OSX_BUILD)
# define GLEW_STATIC
# include <GL/glew.h>
#endif

#define GL_GLEXT_PROTOTYPES 1

#include <SDL2/SDL.h>
#ifdef USE_GLES
#include <SDL2/SDL_opengles2.h>
#else
#include <SDL2/SDL_opengl.h>
#endif

#include "gfx_shader_cache.h"
#include "pc/fs/fs.h"
#include "pc/platform.h"
#include "pc/configfile.h"

// GLES spells this GL_APIENTRY, desktop GL headers spell it APIENTRY, and on
// most Linux toolchains both expand to nothing.
#if !defined(APIENTRY)
# if defined(GL_APIENTRY)
#  define APIENTRY GL_APIENTRY
# else
#  define APIENTRY
# endif
#endif

// GL_OES_get_program_binary and ARB_get_program_binary agree on these values,
// so one set covers both spellings.
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#endif

#define SHADER_CACHE_FILE  "shader_cache.bin"
#define SHADER_CACHE_MAGIC "SM64CSC\x01"

// A session that meets this many distinct combiners is already pathological;
// the cap is here so a corrupt or adversarial file cannot make us allocate
// without bound, not because a real cache is expected to approach it.
#define SHADER_CACHE_MAX_ENTRIES 1024
#define SHADER_CACHE_MAX_BINARY  (4u * 1024u * 1024u)

// Shortest gap between two write-throughs, so a burst of compiles on level entry
// costs one file write rather than one per shader. See gfx_shader_cache_store().
#define SHADER_CACHE_WRITE_DEBOUNCE_MS 2000u

struct ShaderCacheEntry {
    uint64_t sourceHash;
    uint32_t binaryFormat;
    uint32_t length;
    void    *binary;
};

struct ShaderCacheHeader {
    char     magic[8];
    uint64_t driverHash;   // GL_VENDOR + GL_RENDERER + GL_VERSION
    uint32_t count;
    uint32_t reserved;
};

typedef void (APIENTRY *PFN_GetProgramBinary)(GLuint program, GLsizei bufSize, GLsizei *length,
                                              GLenum *binaryFormat, void *binary);
// The OES spec types this last parameter GLint and the core one GLsizei. Both
// are 32-bit signed integers on every ABI this builds for, so one typedef serves.
typedef void (APIENTRY *PFN_ProgramBinary)(GLuint program, GLenum binaryFormat,
                                           const void *binary, GLsizei length);

static PFN_GetProgramBinary sGetProgramBinary = NULL;
static PFN_ProgramBinary    sProgramBinary    = NULL;

static struct ShaderCacheEntry *sEntries = NULL;
static uint32_t sCount = 0;
static uint32_t sCapacity = 0;
static uint64_t sDriverHash = 0;
static bool sAvailable = false;   // the extension is usable
static bool sDirty = false;       // something was added since the file was read
static bool sInited = false;
static uint32_t sLastWriteTicks = 0;

static uint64_t fnv1a(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = seed;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t gfx_shader_cache_hash_source(const char *vertexSource, const char *fragmentSource) {
    uint64_t h = 0xcbf29ce484222325ULL;
    h = fnv1a(vertexSource, strlen(vertexSource), h);
    // A separator, so that moving a line from the end of the vertex shader to
    // the start of the fragment shader cannot collide with the original.
    h = fnv1a("\0|\0", 3, h);
    h = fnv1a(fragmentSource, strlen(fragmentSource), h);
    return h;
}

static uint64_t driver_identity_hash(void) {
    uint64_t h = 0xcbf29ce484222325ULL;
    const GLenum ids[3] = { GL_VENDOR, GL_RENDERER, GL_VERSION };
    for (int i = 0; i < 3; i++) {
        const char *s = (const char *)glGetString(ids[i]);
        if (s == NULL) { s = "?"; }
        h = fnv1a(s, strlen(s), h);
        h = fnv1a("\0", 1, h);
    }
    return h;
}

static bool extension_supported(void) {
    // ES 3.0 and desktop GL 4.1 have this in core, but the string check covers
    // both the extension and (harmlessly) a core implementation that also
    // advertises it. If the entry points resolve and at least one binary format
    // exists, it works -- which is a more useful test than parsing versions.
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    // A driver without the feature leaves the query alone and raises
    // GL_INVALID_ENUM; clear it so the caller does not inherit our error.
    while (glGetError() != GL_NO_ERROR) { formats = 0; }
    return formats > 0;
}

static void free_entries(void) {
    for (uint32_t i = 0; i < sCount; i++) { free(sEntries[i].binary); }
    free(sEntries);
    sEntries = NULL;
    sCount = 0;
    sCapacity = 0;
}

static bool reserve(uint32_t needed) {
    if (needed <= sCapacity) { return true; }
    uint32_t cap = sCapacity ? sCapacity * 2 : 64;
    while (cap < needed) { cap *= 2; }
    if (cap > SHADER_CACHE_MAX_ENTRIES) { cap = SHADER_CACHE_MAX_ENTRIES; }
    if (needed > cap) { return false; }
    struct ShaderCacheEntry *grown = realloc(sEntries, sizeof(*sEntries) * cap);
    if (grown == NULL) { return false; }
    sEntries = grown;
    sCapacity = cap;
    return true;
}

static void read_cache_file(void) {
    const char *path = fs_get_write_path(SHADER_CACHE_FILE);
    if (path == NULL) { return; }
    FILE *f = fopen(path, "rb");
    if (f == NULL) { return; }

    struct ShaderCacheHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return; }
    if (memcmp(hdr.magic, SHADER_CACHE_MAGIC, sizeof(hdr.magic)) != 0 ||
        hdr.driverHash != sDriverHash ||
        hdr.count > SHADER_CACHE_MAX_ENTRIES) {
        // A different driver, a different build of this file's format, or
        // garbage. Not an error -- the cache simply starts empty and is
        // rewritten on shutdown.
        fclose(f);
        return;
    }

    for (uint32_t i = 0; i < hdr.count; i++) {
        uint64_t hash;
        uint32_t fmt, len;
        if (fread(&hash, sizeof(hash), 1, f) != 1) { break; }
        if (fread(&fmt, sizeof(fmt), 1, f) != 1) { break; }
        if (fread(&len, sizeof(len), 1, f) != 1) { break; }
        if (len == 0 || len > SHADER_CACHE_MAX_BINARY) { break; }
        if (!reserve(sCount + 1)) { break; }
        void *buf = malloc(len);
        if (buf == NULL) { break; }
        if (fread(buf, 1, len, f) != len) { free(buf); break; }
        sEntries[sCount].sourceHash   = hash;
        sEntries[sCount].binaryFormat = fmt;
        sEntries[sCount].length       = len;
        sEntries[sCount].binary       = buf;
        sCount++;
    }
    fclose(f);
}

void gfx_shader_cache_init(void) {
    if (sInited) { return; }
    sInited = true;

    if (!configShaderCache) { return; }

    sGetProgramBinary = (PFN_GetProgramBinary)SDL_GL_GetProcAddress("glGetProgramBinaryOES");
    sProgramBinary    = (PFN_ProgramBinary)SDL_GL_GetProcAddress("glProgramBinaryOES");
    if (sGetProgramBinary == NULL || sProgramBinary == NULL) {
        sGetProgramBinary = (PFN_GetProgramBinary)SDL_GL_GetProcAddress("glGetProgramBinary");
        sProgramBinary    = (PFN_ProgramBinary)SDL_GL_GetProcAddress("glProgramBinary");
    }
    if (sGetProgramBinary == NULL || sProgramBinary == NULL) { return; }
    if (!extension_supported()) { return; }

    sAvailable = true;
    sDriverHash = driver_identity_hash();
    read_cache_file();
}

bool gfx_shader_cache_load(uint64_t sourceHash, uint32_t program) {
    if (!sAvailable) { return false; }

    for (uint32_t i = 0; i < sCount; i++) {
        if (sEntries[i].sourceHash != sourceHash) { continue; }

        // Clear any pending error so the link check below cannot be confused by
        // something an earlier call left behind.
        while (glGetError() != GL_NO_ERROR) { }

        sProgramBinary(program, (GLenum)sEntries[i].binaryFormat,
                       sEntries[i].binary, (GLsizei)sEntries[i].length);

        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (glGetError() == GL_NO_ERROR && linked == GL_TRUE) { return true; }

        // The driver rejected it -- a driver update that kept the same version
        // string, or a corrupt entry. Drop it and let the caller compile; the
        // fresh binary replaces this one on the next flush.
        free(sEntries[i].binary);
        sEntries[i] = sEntries[--sCount];
        sDirty = true;
        return false;
    }
    return false;
}

void gfx_shader_cache_store(uint64_t sourceHash, uint32_t program) {
    if (!sAvailable) { return; }
    if (sCount >= SHADER_CACHE_MAX_ENTRIES) { return; }

    GLint length = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (length <= 0 || (uint32_t)length > SHADER_CACHE_MAX_BINARY) { return; }

    void *buf = malloc((size_t)length);
    if (buf == NULL) { return; }

    GLsizei written = 0;
    GLenum fmt = 0;
    sGetProgramBinary(program, (GLsizei)length, &written, &fmt, buf);
    if (written <= 0) { free(buf); return; }
    if (!reserve(sCount + 1)) { free(buf); return; }

    sEntries[sCount].sourceHash   = sourceHash;
    sEntries[sCount].binaryFormat = (uint32_t)fmt;
    sEntries[sCount].length       = (uint32_t)written;
    sEntries[sCount].binary       = buf;
    sCount++;
    sDirty = true;

    // Write through rather than waiting for shutdown. ArkOS and EmulationStation
    // routinely kill the game instead of letting it exit through the menu -- the
    // same reason profile_log.c installs signal handlers -- and a cache that only
    // persisted on a clean exit would never survive the one platform this exists
    // for.
    //
    // The cost rides along with a compile that has already cost ~180ms, so it is
    // hidden inside a frame that was lost anyway; the debounce keeps a burst of
    // six new shaders on level entry down to one write. A warm cache stores
    // nothing and therefore writes nothing.
    const uint32_t now = SDL_GetTicks();
    if (sLastWriteTicks == 0 || (now - sLastWriteTicks) > SHADER_CACHE_WRITE_DEBOUNCE_MS) {
        gfx_shader_cache_flush();
        sLastWriteTicks = now;
    }
}

void gfx_shader_cache_flush(void) {
    if (!sAvailable || !sDirty || sCount == 0) { return; }

    const char *path = fs_get_write_path(SHADER_CACHE_FILE);
    if (path == NULL) { return; }

    // Write beside the real file and rename over it, so an interrupted write
    // leaves the previous cache intact rather than a truncated one that the
    // next run has to detect and discard.
    char tmp[SYS_MAX_PATH];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) { return; }

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) { return; }

    struct ShaderCacheHeader hdr;
    memcpy(hdr.magic, SHADER_CACHE_MAGIC, sizeof(hdr.magic));
    hdr.driverHash = sDriverHash;
    hdr.count = sCount;
    hdr.reserved = 0;

    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    for (uint32_t i = 0; ok && i < sCount; i++) {
        ok = ok && fwrite(&sEntries[i].sourceHash, sizeof(uint64_t), 1, f) == 1;
        ok = ok && fwrite(&sEntries[i].binaryFormat, sizeof(uint32_t), 1, f) == 1;
        ok = ok && fwrite(&sEntries[i].length, sizeof(uint32_t), 1, f) == 1;
        ok = ok && fwrite(sEntries[i].binary, 1, sEntries[i].length, f) == sEntries[i].length;
    }
    ok = (fclose(f) == 0) && ok;

    if (!ok) { remove(tmp); return; }

    // rename() will not replace an existing file on Windows.
#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp, path) != 0) { remove(tmp); return; }
    sDirty = false;
}

void gfx_shader_cache_shutdown(void) {
    free_entries();
    sAvailable = false;
    sDirty = false;
    sInited = false;
    sGetProgramBinary = NULL;
    sProgramBinary = NULL;
}
