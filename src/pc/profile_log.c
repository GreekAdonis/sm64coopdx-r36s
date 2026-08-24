#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>

#include "profile_log.h"

#ifdef PROFILE_BUILD

#include "debug_context.h"
#include "profile_sample.h"
#include "configfile.h"
#include "utils/misc.h"
#include "game/area.h"
#include "game/level_update.h"
#include "game/object_list_processor.h"
#include "pc/network/network_player.h"
#include "pc/pc_main.h"

#define PROFILE_FLUSH_INTERVAL 128
#define PROFILE_DEFAULT_PATH   "sm64coopdx-profile.csv"

struct ProfileCounters gProfileCounters = { 0 };

static FILE *sFile = NULL;
static char  sSamplePath[512] = { 0 };
static char  sHookPath[512] = { 0 };
static char  sIoBuf[1 << 16];
static u64   sFrame = 0;
static f64   sStartTime = 0;
static u32   sSinceFlush = 0;

// microseconds spent in a context during the frame that just ended
static s32 profile_ctx_us(enum DebugContext ctx) {
    return (s32)(debug_context_get_time(ctx) * 1000000.0);
}

// Display lists appended this frame, as an open-addressed set cleared per frame.
// The busiest frame in run 6 appended under 800 nodes, so at this size the table
// sits below a 20% load factor and probe chains stay short. Past the fill limit
// the distinct count saturates instead of degrading into long chains; a
// saturated row is obvious in the CSV (dldistinct == PROFILE_DL_MAX) and still
// answers the only question being asked of it.
#define PROFILE_DL_SLOTS 4096
#define PROFILE_DL_MAX   2048
static const void *sDlSeen[PROFILE_DL_SLOTS];

void profile_note_display_list(const void *displayList) {
    gProfileCounters.dlNodes++;
    if (!displayList) { return; }
    if (gProfileCounters.dlDistinct >= PROFILE_DL_MAX) { return; }

    // Display lists are at least 8-byte aligned, so the low bits carry nothing.
    // Drop them, mix, and take the high bits of the product.
    u64 h = (u64)(uintptr_t)displayList >> 3;
    h *= 0x9E3779B97F4A7C15ULL;
    size_t i = (size_t)(h >> 48) & (PROFILE_DL_SLOTS - 1);

    while (sDlSeen[i] != NULL) {
        if (sDlSeen[i] == displayList) { return; }
        i = (i + 1) & (PROFILE_DL_SLOTS - 1);
    }
    sDlSeen[i] = displayList;
    gProfileCounters.dlDistinct++;
}

static void profile_log_signal(int sig) {
    profile_log_shutdown();
    signal(sig, SIG_DFL);
    raise(sig);
}

void profile_log_init(void) {
    if (sFile) { return; }

    const char *path = getenv("SM64_PROFILE_LOG");
    if (!path || !*path) { path = PROFILE_DEFAULT_PATH; }
    if (!strcmp(path, "0") || !strcmp(path, "off")) {
        printf("[profile] frame logging disabled by SM64_PROFILE_LOG\n");
        return;
    }

    sFile = fopen(path, "w");
    if (!sFile) {
        printf("[profile] could not open '%s' for writing, logging disabled\n", path);
        return;
    }
    setvbuf(sFile, sIoBuf, _IOFBF, sizeof(sIoBuf));

    snprintf(sSamplePath, sizeof(sSamplePath), "%s.samples", path);
    snprintf(sHookPath, sizeof(sHookPath), "%s.hooks", path);

    // one metadata line, then the header; both consumers (tools/profile_report.py
    // and anything reading it as plain CSV) skip lines starting with '#'
    fprintf(sFile, "# sm64coopdx profile build"
                   " framerate_mode=%d vsync=%d handheld_res=%ux%u audio_threaded=%d\n",
            (int)configFramerateMode, (int)configWindow.vsync,
            configHandheldResW, configHandheldResH, (int)configAudioThreaded);

    fprintf(sFile,
            "frame,time_s,level,area,act,players,objects,subframes,"
            "us_total,us_net,us_netcodec,us_netsocket,us_interp,us_game,us_levelscript,us_objects,us_geo,"
            "us_smlua,us_hook,us_audio,us_render,us_gfxdl,us_lighting,us_texupload,"
            "us_swap,us_delay,"
            "draws,tris,verts,texloads,texbytes,texflushes,binds,bindskips,impskips,shaders,"
            "fldepth,flviewport,flshader,flalpha,fltexture,flsampler,flfull,flcomb,"
            "hookcalls,hookbhv,fieldgets,fieldsets,codeccomp,codecdecomp,"
            "objsdrawn,dlnodes,dldistinct,renderskips,simlag_us,"
            "mario_x,mario_y,mario_z\n");

    sStartTime = clock_elapsed_f64();
    printf("[profile] logging frames to '%s'\n", path);

    atexit(profile_log_shutdown);

    // ArkOS/EmulationStation can kill the game rather than letting it exit
    // through the menu, and a killed process runs no atexit handlers, so catch
    // the termination signals and write everything out before dying. stdio in a
    // signal handler is not async-signal-safe, but the process is ending either
    // way and losing the whole session's data is the worse outcome.
    signal(SIGINT, profile_log_signal);
    signal(SIGTERM, profile_log_signal);
    signal(SIGHUP, profile_log_signal);
    signal(SIGQUIT, profile_log_signal);

    profile_sample_init();
}

void profile_log_frame(void) {
    if (!sFile) { return; }

    struct ProfileCounters *c = &gProfileCounters;
    struct MarioState *m = &gMarioStates[0];

    fprintf(sFile,
            "%llu,%.3f,%d,%d,%d,%d,%u,%u,"
            "%d,%d,%d,%d,%d,%d,%d,"
            "%d,%d,%d,%d,%d,%d,%d,"
            "%d,%d,%d,%d,"
            "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
            "%u,%u,%u,%u,%u,%u,%u,%u,"
            "%u,%u,%u,%u,%u,%u,"
            "%u,%u,%u,%u,%d,"
            "%d,%d,%d\n",
            (unsigned long long)sFrame,
            clock_elapsed_f64() - sStartTime,
            (int)gCurrLevelNum, (int)gCurrAreaIndex, (int)gCurrActNum,
            (int)network_player_connected_count(), gObjectCounter, c->subFrames,
            profile_ctx_us(CTX_TOTAL),
            profile_ctx_us(CTX_NETWORK),
            profile_ctx_us(CTX_NET_CODEC),
            profile_ctx_us(CTX_NET_SOCKET),
            profile_ctx_us(CTX_INTERP),
            profile_ctx_us(CTX_GAME_LOOP),
            profile_ctx_us(CTX_LEVEL_SCRIPT),
            profile_ctx_us(CTX_OBJECTS),
            profile_ctx_us(CTX_GEO),
            profile_ctx_us(CTX_SMLUA),
            profile_ctx_us(CTX_HOOK),
            profile_ctx_us(CTX_AUDIO),
            profile_ctx_us(CTX_RENDER),
            profile_ctx_us(CTX_GFX_DL),
            profile_ctx_us(CTX_LIGHTING),
            profile_ctx_us(CTX_TEXUPLOAD),
            profile_ctx_us(CTX_SWAP),
            profile_ctx_us(CTX_DELAY),
            c->drawCalls, c->tris, c->verts, c->texLoads, c->texBytes,
            c->texCacheFlushes, c->texBinds, c->texBindSkips, c->texImportSkips, c->shaderLoads,
            c->flushDepth, c->flushViewport, c->flushShader, c->flushAlpha,
            c->flushTexture, c->flushSampler, c->flushBufferFull, c->flushCombiner,
            c->hookCalls, c->hookBehavior, c->luaFieldGets, c->luaFieldSets,
            c->codecCompressCalls, c->codecDecompressCalls,
            c->objsDrawn, c->dlNodes, c->dlDistinct, c->renderSkips,
            (int)(gSimLagSeconds * 1000000.0),
            (int)m->pos[0], (int)m->pos[1], (int)m->pos[2]);

    sFrame++;
    memset(c, 0, sizeof(*c));
    memset(sDlSeen, 0, sizeof(sDlSeen));

    // the SD card in these handhelds is slow, so write in chunks rather than
    // per frame, and never inside a frame we are trying to measure
    if (++sSinceFlush >= PROFILE_FLUSH_INTERVAL) {
        sSinceFlush = 0;
        fflush(sFile);
    }
}

void profile_log_shutdown(void) {
    if (!sFile) { return; }
    fflush(sFile);
    fclose(sFile);
    sFile = NULL;
    profile_sample_dump(sSamplePath);
    profile_dump_hook_types(sHookPath);
    printf("[profile] wrote %llu frames\n", (unsigned long long)sFrame);
}

#endif
