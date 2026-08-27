#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "sm64.h"

#include "pc/lua/smlua.h"
#include "pc/lua/utils/smlua_text_utils.h"
#include "game/memory.h"
#include "audio/data.h"
#include "audio/external.h"

#include "network/network.h"
#include "lua/smlua.h"

#include "rom_assets.h"
#include "rom_checker.h"
#include "pc_main.h"
#include "loading.h"
#include "cliopts.h"
#include "configfile.h"
#include "thread.h"
#include "controller/controller_api.h"
#include "controller/controller_keyboard.h"
#include "controller/controller_mouse.h"
#include "fs/fs.h"

#include "game/display.h" // for gGlobalTimer
#include "game/game_init.h"
#include "game/main.h"
#include "game/rumble_init.h"

#include "pc/lua/utils/smlua_audio_utils.h"

#include "pc/network/version.h"
#include "pc/network/socket/socket.h"
#include "pc/network/network_player.h"
#include "pc/update_checker.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_unicode.h"
#include "pc/djui/djui_panel.h"
#include "pc/djui/djui_panel_modlist.h"
#include "pc/djui/djui_ctx_display.h"
#include "pc/djui/djui_fps_display.h"
#include "pc/djui/djui_lua_profiler.h"
#include "pc/debuglog.h"
#include "pc/utils/misc.h"
#include "pc/mods/mods.h"

#include "debug_context.h"
#include "profile_log.h"
#include "menu/intro_geo.h"

#include "gfx_dimensions.h"
#include "game/segment2.h"

#include "engine/math_util.h"

#ifdef DISCORD_SDK
#include "pc/discord/discord.h"
#endif

#include "pc/mumble/mumble.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <SDL2/SDL.h>

extern Vp gViewportFullscreen;

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

s32 gRumblePakPfs;
u32 gNumVblanks = 0;

u8 gRenderingInterpolated = 0;
f32 gRenderingDelta = 0;
f32 gFramePercentage = 0.f;

#define FRAMERATE 30
static const f64 sFrameTime = (1.0 / ((double)FRAMERATE));
static f64 sFpsTimeLast = 0;
static f64 sFrameTimeStart = 0;
static u32 sDrawnFrames = 0;

bool gGameInited = false;
bool gGfxInited = false;

f32 gMasterVolume;

u8 gLuaVolumeMaster = 127;
u8 gLuaVolumeLevel = 127;
u8 gLuaVolumeSfx = 127;
u8 gLuaVolumeEnv = 127;

struct AudioAPI* gAudioApi = &audio_null;
struct GfxWindowManagerAPI* gWindowApi = &gfx_dummy_wm_api;
struct GfxRenderingAPI* gRenderApi = &gfx_dummy_renderer_api;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {}
void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {}

void send_display_list(struct SPTask *spTask) {
    if (!gGameInited) { return; }
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#ifdef VERSION_EU
#define SAMPLES_HIGH 560 // gAudioBufferParameters.maxAiBufferLength
#define SAMPLES_LOW 528 // gAudioBufferParameters.minAiBufferLength
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

extern void patch_mtx_before(void);
extern void patch_screen_transition_before(void);
extern void patch_title_screen_before(void);
extern void patch_dialog_before(void);
extern void patch_hud_before(void);
extern void patch_paintings_before(void);
extern void patch_bubble_particles_before(void);
extern void patch_snow_particles_before(void);
extern void patch_djui_before(void);
extern void patch_djui_hud_before(void);
extern void patch_scroll_targets_before(void);

extern void patch_mtx_interpolated(f32 delta);
extern void patch_screen_transition_interpolated(f32 delta);
extern void patch_title_screen_interpolated(f32 delta);
extern void patch_dialog_interpolated(f32 delta);
extern void patch_hud_interpolated(f32 delta);
extern void patch_paintings_interpolated(f32 delta);
extern void patch_bubble_particles_interpolated(f32 delta);
extern void patch_snow_particles_interpolated(f32 delta);
extern void patch_djui_interpolated(f32 delta);
extern void patch_djui_hud(f32 delta);
extern void patch_scroll_targets_interpolated(f32 delta);

static void patch_interpolations_before(void) {
    patch_mtx_before();
    patch_screen_transition_before();
    patch_title_screen_before();
    patch_dialog_before();
    patch_hud_before();
    patch_paintings_before();
    patch_bubble_particles_before();
    patch_snow_particles_before();
    patch_djui_before();
    patch_djui_hud_before();
    patch_scroll_targets_before();
}

static inline void patch_interpolations(f32 delta) {
    patch_mtx_interpolated(delta);
    patch_screen_transition_interpolated(delta);
    patch_title_screen_interpolated(delta);
    patch_dialog_interpolated(delta);
    patch_hud_interpolated(delta);
    patch_paintings_interpolated(delta);
    patch_bubble_particles_interpolated(delta);
    patch_snow_particles_interpolated(delta);
    patch_djui_interpolated(delta);
    patch_djui_hud(delta);
    patch_scroll_targets_interpolated(delta);
}

static void compute_fps(f64 curTime) {
    u32 fps = round((f64) sDrawnFrames / MAX(0.001, curTime - sFpsTimeLast));
    djui_fps_display_update(fps);
    sFpsTimeLast = curTime;
    sDrawnFrames = 0;
}

static s32 get_num_frames_to_draw(f64 t, u32 frameLimit) {
    if (frameLimit % FRAMERATE == 0) {
        return frameLimit / FRAMERATE;
    }
    s64 numFramesCurr = (s64) (t * (f64) frameLimit);
    s64 numFramesNext = (s64) ((t + sFrameTime) * (f64) frameLimit);
    return (s32) MAX(1, numFramesNext - numFramesCurr);
}

static u32 get_display_refresh_rate(void) {
    static u32 refreshRate = 0;
    if (!refreshRate) {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
            if (mode.refresh_rate > 0) { refreshRate = (u32) mode.refresh_rate; }
        } else {
            refreshRate = 60;
        }
    }
    return refreshRate;
}

static u32 get_target_refresh_rate(void) {
    if (configFramerateMode == RRM_MANUAL) { return configFrameLimit; }
    if (configFramerateMode == RRM_UNLIMITED) { return 3000; } // Has no effect
    return get_display_refresh_rate();
}

static void select_graphics_backend(void) {
    if (gCLIOpts.headless) {
        return;
    }

#if defined(_WIN32)
    if (configGraphicsBackend == GAPI_GL && !gfx_sdl_check_opengl_compatibility()) {
        configGraphicsBackend = GAPI_D3D11;
    }
#endif
    int backend = configGraphicsBackend;
#if defined(_WIN32)
    if (gCLIOpts.backend != -1) { backend = gCLIOpts.backend; }
#endif

    switch (backend) {
        case GAPI_GL:
            gWindowApi = &gfx_sdl;
            gRenderApi = &gfx_opengl_api;
            gAudioApi  = &audio_sdl;
            break;
#if defined(_WIN32)
        case GAPI_D3D11:
            gWindowApi = &gfx_dxgi;
            gRenderApi = &gfx_direct3d11_api;
            gAudioApi  = &audio_sdl;
            break;
#endif
        default:
            gWindowApi = &gfx_sdl;
            gRenderApi = &gfx_opengl_api;
            gAudioApi  = &audio_sdl;
            break;
    }

    if (!gAudioApi->init()) {
        gAudioApi = &audio_null;
    }
}

// Adaptive render dropping.
//
// gNetworkAreaTimer is driven by wall clock at 30Hz (see clock_elapsed_ticks()),
// and cur_obj_update() re-runs an object's behaviour until its own areaTimer
// catches up to it. That means the simulation's workload per second is fixed by
// the clock, not by our framerate: at 20fps every area-timer object simply runs
// its behaviour one and a half times per frame instead of once. So a slow client
// cannot "do less simulation" -- there is no less to do, and skimping on it is
// what desyncs the room, because the client keeps broadcasting sync-object state
// from a simulation running at the wrong rate.
//
// Rendering is the part that can give. Measured in a nine-player room on the
// RK3326: 38.9ms of work against a 33.3ms budget, of which the display-list
// build, the interpolation pass and the swap were 13.8ms. Dropping those on the
// frames where we are behind buys back more than twice the overrun, so the
// simulation converges back to 30Hz instead of falling further behind.
//
// Two guards keep the failure mode sane. Consecutive skips are capped so the
// player always gets a picture, however choppy, rather than a frozen window; and
// a deadline that has receded more than a second is abandoned rather than
// chased, so a client that is hopelessly slow does not end up skipping every
// render forever paying down a backlog it can never clear. A client that still
// cannot keep up past those is beyond what this can fix -- the missing backstop
// there is relinquishing sync-object ownership so it stops speaking for objects
// it is simulating badly.

// Past this the backlog is not payable -- a level load, a mod download, or
// hardware that simply cannot run this room. Give up on it rather than skipping
// every render forever chasing a deadline that keeps receding.
#define RENDER_SKIP_GIVE_UP_SECONDS 1.0

// The deadline may never fall further behind wall clock than this. Bounds the
// debt so it describes recent lateness rather than everything accrued since the
// last give-up, which is what the enter/exit thresholds need it to mean.
//
// This is a floor on the ceiling, not the ceiling itself: see render_skip_max_debt().
#define RENDER_SKIP_MAX_DEBT_TICKS 3.0

// How far above the enter threshold the ceiling must sit. The clamp has to leave
// room for `behind` to actually cross the threshold and then keep rising, or the
// hysteresis has nothing to work with.
#define RENDER_SKIP_DEBT_HEADROOM 1.5

// The debt ceiling, in seconds.
//
// Run 10 was configured with render_skip_enter_ms 133 -- four ticks -- against a
// fixed three-tick ceiling. `behind` was clamped to 100ms before the threshold
// was tested, so it could never reach 133ms and not one render was dropped in
// 526 seconds of play. The CSV showed simlag_us at exactly 100000 for p50, p90
// and max, which is the clamp reporting itself rather than a measurement.
//
// A ceiling that can silently sit below the threshold it gates makes the config
// dishonest, so derive it from the threshold instead of fixing it. The constant
// above stays as a floor for small thresholds, where the point of the clamp is
// to keep the debt recent rather than to leave the policy room to act.
static f64 render_skip_max_debt(void) {
    const f64 floor = sFrameTime * RENDER_SKIP_MAX_DEBT_TICKS;
    const f64 needed = (configRenderSkipEnterMs / 1000.0) * RENDER_SKIP_DEBT_HEADROOM;
    return needed > floor ? needed : floor;
}

// Fraction of the standing debt forgiven by each iteration that met its budget,
// and how much longer than a tick an iteration may take and still count as
// on time. The tolerance absorbs scheduling jitter around the frame cap, which
// lands an on-time iteration at a tick plus a hair rather than exactly a tick.
#define RENDER_SKIP_DEBT_DECAY 0.0625
#define RENDER_SKIP_ON_TIME_TOLERANCE 1.05

// Smoothing on the simulation-only cost estimate. Slow enough that one heavy
// frame does not flip the policy, fast enough to follow a room change.
#define RENDER_SKIP_SIM_COST_ALPHA 0.05

static f64 sSimDeadline   = 0.0;
static f64 sLastDeadlineCheck = 0.0;
static f64 sSimOnlyStart  = 0.0;
static f64 sSimOnlyCost   = 0.0;
static u32 sRenderSkipRun = 0;
static bool sRenderSkipping = false;
f64 gSimLagSeconds = 0.0;   // published for the profile log
f64 gSimOnlySeconds = 0.0;  // ditto
bool gSkipSceneGraph = false;

// Whether dropping renders can still achieve anything.
//
// The policy trades displayed frames for simulation rate, to hold the wall-clock
// 30Hz the netcode's area timer runs on. That trade only pays while the target
// is reachable -- and it is reachable only if a tick without a render fits in
// the budget. Once the simulation alone overruns, dropping every render still
// misses 30Hz, so every dropped frame buys a goal that cannot be met.
//
// Run 11 landed exactly there: a simulation-only tick cost 43.9ms against a
// 33.3ms budget, 1.32x over. The policy dutifully dropped two renders in three
// and delivered 5.7fps, where not dropping at all would have delivered 11.6fps
// for 5.6Hz less simulation that was never going to be enough either way.
//
// The comment on RENDER_SKIP_GIVE_UP_SECONDS above already names what a client
// this far gone actually needs -- to stop owning sync objects it is simulating
// badly. Until that exists, the least bad thing is to stop paying for nothing.
static bool render_skip_is_futile(void) {
    if (configRenderSkipFutilePct == 0) { return false; }
    if (sSimOnlyCost <= 0.0) { return false; }
    return sSimOnlyCost > sFrameTime * (configRenderSkipFutilePct / 100.0);
}

// Drops a render only once the simulation has fallen far enough behind wall
// clock for peers to notice, and stops as soon as it is comfortably back.
//
// The original policy skipped whenever it was behind by any amount at all. With
// no deadband that latches on: in a room even slightly over budget, every
// rendered iteration puts the client behind again, so it skipped the maximum
// every time. Run 8 measured exactly that -- the consecutive-skip run lengths
// were {1: 10, 2: 9339}, i.e. pinned at the cap, holding 29.1Hz of simulation
// at the cost of 14.3 displayed fps.
//
// Dropping renders is still the right trade when it is needed: a client behind
// wall clock keeps owning sync objects and broadcasting state from a simulation
// running at the wrong rate, which is what used to take whole rooms down. The
// question is only when it is needed, and "we overran by a millisecond" is not
// the same thing as "we are desyncing".
//
// So: enter at configRenderSkipEnterMs of lag, leave at configRenderSkipExitMs.
// The gap between them is what stops the decision chattering frame to frame.
// A room that is comfortably inside budget now never drops a frame; one that is
// genuinely falling behind still gets protected.
static bool should_skip_render(void) {
    // Single player has no shared clock to stay in step with, and
    // network_check_singleplayer_pause() stops the area timer there anyway.
    if (gNetworkType == NT_NONE) {
        sSimDeadline = 0.0;
        sLastDeadlineCheck = 0.0;
        sRenderSkipRun = 0;
        sRenderSkipping = false;
        gSimLagSeconds = 0.0;
        return false;
    }

    f64 now = clock_elapsed_f64();
    if (sSimDeadline == 0.0) { sSimDeadline = now; }

    // Wall time this iteration actually took, for the debt decay below.
    const f64 iterTime = (sLastDeadlineCheck > 0.0) ? (now - sLastDeadlineCheck) : sFrameTime;
    sLastDeadlineCheck = now;

    // A single enormous iteration is a hitch, not a trend, and dropping renders
    // could not have prevented it -- so do not charge the client for it.
    //
    // Run 18 recorded two: 7.6 seconds on a server join and 1.4 seconds in the
    // middle of ordinary play, both spent inside network_update() and both
    // mostly *blocked* rather than computing (the sampler runs on the thread's
    // CPU clock, and the window holding the join showed 55.7% CPU against wall).
    // Render skipping recovers CPU time. It cannot recover time the process
    // spent waiting on a socket, so debt accrued that way is unpayable by the
    // only mechanism this policy has.
    //
    // Left alone the lateness clamps to the debt ceiling and stays pinned there:
    // after both stalls simlag sat at exactly 100ms for dozens of frames, which
    // is the ceiling reporting itself. What happened next depended on where the
    // cost estimate landed, and both outcomes were wrong -- either the policy
    // skipped renders to pay a debt no amount of skipping could clear, or the
    // stall had poisoned sSimOnlyCost badly enough that render_skip_is_futile()
    // suppressed skipping for the two seconds the estimate took to decay back.
    //
    // Resetting is what RENDER_SKIP_GIVE_UP_SECONDS was for; that test is dead
    // because the clamp below bounds `behind` well under a second before it is
    // ever reached. This restores the intent at a threshold that can fire.
    if (configRenderSkipStallMs != 0 && iterTime > (configRenderSkipStallMs / 1000.0)) {
        sSimDeadline = now;
        sRenderSkipRun = 0;
        sRenderSkipping = false;
        gSimLagSeconds = 0.0;
        return false;
    }

    // One iteration is one simulation tick, so the deadline advances by exactly
    // one tick whether or not we met it. Wall clock running past it is the
    // amount we are behind, and it accumulates fractional overruns that counting
    // whole gNetworkAreaTimer ticks would round away.
    sSimDeadline += sFrameTime;
    f64 behind = now - sSimDeadline;

    // Cap how far the deadline is allowed to fall behind. Without this the
    // accumulator just runs until RENDER_SKIP_GIVE_UP_SECONDS, so `behind`
    // reports debt accrued since the last reset -- which run 9 measured at up
    // to a second old -- rather than how late this iteration actually is.
    //
    // That made every threshold below meaningless. A client running 28.8Hz
    // against a 30Hz requirement accrues 40ms of debt a second, reaches the
    // one-second give-up after ~25s and resets, so the measure was a sawtooth
    // between 0 and 1000ms: 90% of multiplayer frames sat above the 66ms enter
    // threshold and 0.2% below the 16ms exit one. The policy latched on and
    // never let go.
    //
    // Clamped, `behind` means "how late are we now", which is the thing the
    // thresholds are supposed to be asking about.
    const f64 maxDebt = render_skip_max_debt();
    if (behind > maxDebt) {
        sSimDeadline = now - maxDebt;
        behind = maxDebt;
    }

    // An iteration that met its budget pays down a slice of the standing debt.
    //
    // Without this the accumulator is a ratchet. The deadline advances by exactly
    // one tick per iteration while the frame cap guarantees an iteration takes at
    // least one tick of wall clock, so `behind` can rise and then only ever hold.
    // Run 10 measured that directly: flat on 4,825 of 8,073 networked transitions,
    // never once below 65ms, and pinned across a 160-second stretch that was
    // holding a comfortable 30fps at 32.8ms per frame. A client that hitched once
    // during a level load read as permanently desyncing, and configRenderSkipExitMs
    // was unreachable, so the policy could latch on with no way to let go.
    //
    // Debt is only payable while renders are being skipped -- those iterations
    // leave the delay loop early, so they can run shorter than a tick -- which is
    // exactly the mechanism doing the catching up. Forgiving a fixed fraction per
    // on-time tick drains a full ceiling to below a 24ms exit threshold in about
    // three quarters of a second at 30Hz, and a client that is genuinely behind
    // never gets the on-time ticks to decay with, so an ongoing deficit still
    // reads as one.
    if (behind > 0.0 && iterTime <= sFrameTime * RENDER_SKIP_ON_TIME_TOLERANCE) {
        const f64 forgiven = behind * RENDER_SKIP_DEBT_DECAY;
        sSimDeadline += forgiven;
        behind -= forgiven;
    }

    gSimLagSeconds = behind;

    // The positive half of this is unreachable now that the clamp above bounds
    // `behind` well below a second; it is kept for the negative half, which
    // catches the clock jumping backwards under us.
    if (behind > RENDER_SKIP_GIVE_UP_SECONDS || behind < -RENDER_SKIP_GIVE_UP_SECONDS) {
        sSimDeadline = now;
        sLastDeadlineCheck = now;
        sRenderSkipRun = 0;
        sRenderSkipping = false;
        gSimLagSeconds = 0.0;
        return false;
    }

    if (configRenderSkipMax == 0 || render_skip_is_futile()) {
        sRenderSkipping = false;
        sRenderSkipRun = 0;
        return false;
    }

    const f64 enter = configRenderSkipEnterMs / 1000.0;
    const f64 exit  = configRenderSkipExitMs / 1000.0;

    if (sRenderSkipping) {
        if (behind < exit) { sRenderSkipping = false; }
    } else if (behind > enter) {
        sRenderSkipping = true;
    }

    if (!sRenderSkipping || sRenderSkipRun >= configRenderSkipMax) {
        sRenderSkipRun = 0;
        return false;
    }

    sRenderSkipRun++;
    return true;
}

void produce_interpolation_frames_and_delay(void) {
    // Close the simulation-only measurement started in produce_one_frame(): the
    // render has not begun yet, so what has elapsed is exactly the tick's cost
    // without it. Smoothed, because the decision it feeds should not swing on
    // one heavy frame.
    if (sSimOnlyStart > 0.0) {
        const f64 simOnly = clock_elapsed_f64() - sSimOnlyStart;
        sSimOnlyStart = 0.0;
        // A hitch says nothing about what a tick costs, so it does not get a vote.
        //
        // This has to test the sample in front of it rather than a flag set by
        // should_skip_render(), which is where the first attempt went wrong.
        // That function runs before sSimOnlyStart is taken for the frame and
        // derives its iterTime from the *previous* iteration, so a flag set there
        // discarded the following frame's sample -- while the stall's own sample
        // had already been folded in one frame earlier. It suppressed the
        // recovery and left the poisoning untouched, which is precisely backwards.
        //
        // Run 19 shows what that costs: a 327ms level-load frame drove the
        // estimate to 344ms, and at alpha 0.05 it needed four seconds to decay
        // back under budget. For all four, render_skip_is_futile() saw a
        // hopeless simulation and refused to skip anything, so frames ran 62-82ms
        // at roughly 47% speed -- the load failing to self-correct.
        const f64 stallCutoff = configRenderSkipStallMs / 1000.0;
        if (configRenderSkipStallMs != 0 && simOnly > stallCutoff) {
            // leave the estimate alone
        } else if (sSimOnlyCost <= 0.0) {
            sSimOnlyCost = simOnly;
        } else {
            sSimOnlyCost += (simOnly - sSimOnlyCost) * RENDER_SKIP_SIM_COST_ALPHA;
        }
        gSimOnlySeconds = sSimOnlyCost;
    }

    u32 refreshRate = get_target_refresh_rate();

    // Decided in produce_one_frame(), before the game loop, so that render_game()
    // could skip the scene graph walk as well. Reading it here keeps the two in
    // step: the display list this function declines to submit is the same one
    // that was never built.
    const bool skipRender = gSkipSceneGraph;

    gRenderingInterpolated = true;

    u32 displayRefreshRate = get_display_refresh_rate();
    bool shouldDelay = configFramerateMode != RRM_UNLIMITED;
    if (configWindow.vsync && displayRefreshRate <= refreshRate) {
        shouldDelay = false;
        refreshRate = displayRefreshRate;
    }

    f64 targetTime = sFrameTimeStart + sFrameTime;
    s32 numFramesToDraw = get_num_frames_to_draw(sFrameTimeStart, refreshRate);

    f64 curTime = clock_elapsed_f64();
    f64 loopStartTime = curTime;
    f64 expectedTime = 0;
    u16 framesDrawn = 0;
    const f64 interpFrameTime = sFrameTime / (f64) numFramesToDraw;

    // interpolate and render
    // make sure to draw at least one frame to prevent the game from freezing completely
    // (including inputs and window events) if the game update duration is greater than 33ms
    do {
        curTime = clock_elapsed_f64();
        ++framesDrawn;

        // when we know how many frames to draw, use a precise delta
        f64 idealTime = shouldDelay ? (sFrameTimeStart + interpFrameTime * framesDrawn) : curTime;
        f32 delta = clamp((idealTime - sFrameTimeStart) / sFrameTime, 0.f, 1.f);
        gFramePercentage = clamp((curTime - sFrameTimeStart) / sFrameTime, 0.f, 1.f);
        gRenderingDelta = delta;

        gfx_start_frame();

        // Deliberately after gfx_start_frame(): that is where window and input
        // events are pumped, and a client that stops reading them looks hung and
        // cannot even be closed. Everything below it -- interpolation, the
        // display-list run and the swap -- is what we are here to skip.
        if (skipRender) {
            PROFILE_ADD(renderSkips, 1);
            break;
        }

        if (!gSkipInterpolationTitleScreen) {
            CTX_BEGIN_TIMED(CTX_INTERP);
            patch_interpolations(delta);
            CTX_END_TIMED(CTX_INTERP);
        }
        CTX_BEGIN_TIMED(CTX_GFX_DL);
        send_display_list(gGfxSPTask);
        gfx_end_frame_render();
        CTX_END_TIMED(CTX_GFX_DL);
        CTX_BEGIN_TIMED(CTX_SWAP);
        gfx_display_frame();
        CTX_END_TIMED(CTX_SWAP);
        PROFILE_ADD(subFrames, 1);

        // delay if our framerate is capped
        if (shouldDelay) {
            expectedTime += (targetTime - curTime) / (f64) numFramesToDraw;
            f64 now = clock_elapsed_f64();
            f64 elapsedTime = now - loopStartTime;
            f64 delay = (expectedTime - elapsedTime);
            if (delay > 0.0) {
                CTX_BEGIN_TIMED(CTX_DELAY);
                precise_delay_f64(delay);
                CTX_END_TIMED(CTX_DELAY);
            }
        }

        sDrawnFrames++;

        // Unconditional, where this used to be guarded by `if (shouldDelay)`.
        //
        // The guard made the `numFramesToDraw > 0` half of the loop condition
        // dead under vsync -- which is the default -- because the vsync branch
        // above sets shouldDelay to false. Nothing then decremented the counter,
        // so the only exit was the wall-clock test, and since that test runs
        // *after* a whole image has been drawn the loop would commit to another
        // ~20ms subframe whenever any budget at all remained.
        //
        // Run 14 measured the result: 1,183 frames drew between 3 and 7
        // interpolated images for a single game tick, averaging 89ms against a
        // 33.3ms budget with only 10.8ms of actual game work in them. That is
        // 60% of every frame over two ticks in the whole session.
        //
        // The delay path is unaffected: it already decremented here, and the
        // expectedTime division above reads numFramesToDraw before this line.
        numFramesToDraw--;
    } while ((curTime = clock_elapsed_f64()) < targetTime && numFramesToDraw > 0);

    // compute and update the frame rate every second
    if ((curTime = clock_elapsed_f64()) >= sFpsTimeLast + 1.0) {
        compute_fps(curTime);
    }

    // advance frame start time
    if (curTime > sFrameTimeStart + 2 * sFrameTime) {
        sFrameTimeStart = curTime;
    } else {
        sFrameTimeStart += sFrameTime;
    }

    // A skipped render leaves the schedule anchored in the future, which the
    // next frame then spends.
    //
    // The branch above advances the anchor by a whole tick whatever actually
    // happened. That is right for a frame that rendered, because the swap it
    // blocked on is what consumed the tick. A skipped frame blocks on nothing
    // and costs about 11ms, so the anchor ends up ~22ms ahead of wall clock --
    // and the next frame's targetTime is therefore ~55ms out, which the loop
    // above dutifully fills with extra subframes.
    //
    // That is the oscillation run 14 recorded: 67% of the frames that drew three
    // or more images came immediately after a skipped render, against a 5.6%
    // base rate -- a 12x lift -- and the frame after a skip ran p90 93.2ms
    // against 42.2ms overall. Skip, over-draw, fall behind, skip again.
    //
    // It also pins interpolation. With vsync, delta is computed from
    // (curTime - sFrameTimeStart); an anchor in the future makes that negative,
    // so it clamps to 0 and the extra images all redraw the same instant.
    //
    // Refusing to bank time we did not spend costs nothing when the client is
    // keeping up, because a rendered frame's anchor never runs ahead.
    if (skipRender && sFrameTimeStart > curTime) {
        sFrameTimeStart = curTime;
    }

    gRenderingInterpolated = false;
}

// It's just better to have this off the stack, Because the size isn't small.
// It also may help static analysis and bug catching.
static s16 sAudioBuffer[SAMPLES_HIGH * 2 * 2] = { 0 };

inline static void buffer_audio(void) {
    bool shouldMute = (configMuteFocusLoss && !gWindowApi->has_focus()) || (gMasterVolume == 0);
    if (!shouldMute) {
        set_sequence_player_volume(SEQ_PLAYER_LEVEL, (f32)configMusicVolume / 127.0f * (f32)gLuaVolumeLevel / 127.0f);
        set_sequence_player_volume(SEQ_PLAYER_SFX,   (f32)configSfxVolume / 127.0f * (f32)gLuaVolumeSfx / 127.0f);
        set_sequence_player_volume(SEQ_PLAYER_ENV,   (f32)configEnvVolume / 127.0f * (f32)gLuaVolumeEnv / 127.0f);
    }

    int samplesLeft = gAudioApi->buffered();
    u32 numAudioSamples = samplesLeft < gAudioApi->get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
    for (s32 i = 0; i < 2; i++) {
        create_next_audio_buffer(sAudioBuffer + i * (numAudioSamples * 2), numAudioSamples);
    }

    if (!shouldMute) {
        for (u16 i=0; i < ARRAY_COUNT(sAudioBuffer); i++) {
            sAudioBuffer[i] *= gMasterVolume;
        }
        gAudioApi->play((u8 *)sAudioBuffer, 2 * numAudioSamples * 4);
    }
}

void *audio_thread(UNUSED void *arg) {
    // As long as we have an audio api and that we're threaded, Loop.
    while (gAudioApi) {
        f64 curTime = clock_elapsed_f64();

        // Buffer the audio.
        lock_mutex(&gAudioThread);
        buffer_audio();
        unlock_mutex(&gAudioThread);

        // Delay till the next frame for smooth audio at the correct speed.
        // delay
        f64 targetDelta = 1.0 / (f64)FRAMERATE;
        f64 now = clock_elapsed_f64();
        f64 actualDelta = now - curTime;
        if (actualDelta < targetDelta) {
            f64 delay = ((targetDelta - actualDelta) * 1000.0);
            gWindowApi->delay((u32)delay);
        }
    }

    // Exit the thread if our loop breaks.
    exit_thread();

    return NULL;
}

void produce_one_frame(void) {
    // Evaluated once per game iteration, and before the game loop rather than
    // inside the render step, because game_loop_one_iteration() is where the
    // display list gets built. A dropped render used to pay for the scene graph
    // walk anyway and then throw the result away -- 11.1ms of the 61ms a
    // simulation-only tick costs in run 10's gore room.
    gSkipSceneGraph = should_skip_render();

    // Everything from here to the render step is the simulation-only cost of one
    // tick -- what an iteration would cost if the render were free. That is the
    // quantity the futile check needs; see render_skip_is_futile().
    sSimOnlyStart = clock_elapsed_f64();

    CTX_EXTENT(CTX_NETWORK, network_update);

    CTX_EXTENT(CTX_INTERP, patch_interpolations_before);

    CTX_EXTENT(CTX_GAME_LOOP, game_loop_one_iteration);

    CTX_EXTENT(CTX_SMLUA, smlua_update);

    // If we aren't threaded
    if (gAudioThread.state == INVALID) {
        CTX_EXTENT(CTX_AUDIO, buffer_audio);
    }

    CTX_EXTENT(CTX_RENDER, produce_interpolation_frames_and_delay);
}

// used for rendering 2D scenes fullscreen like the loading or crash screens
void produce_one_dummy_frame(void (*callback)(), u8 clearColorR, u8 clearColorG, u8 clearColorB) {
    // measure frame start time
    f64 frameStart = clock_elapsed_f64();
    f64 targetFrameTime = 1.0 / 60.0; // update at 60fps

    // start frame
    gfx_start_frame();
    config_gfx_pool();
    init_render_image();
    create_dl_ortho_matrix();
    djui_gfx_displaylist_begin();

#ifdef HANDHELD
    // This is a 2D screen-space scene (loading/crash screen) -- render it at
    // native resolution instead of through the low-res internal FBO used for
    // the 3D world pass (see gfx_opengl.c and G_HANDHELD_HUD_PASS_EXT).
    gSPHandheldBeginHudPass(gDisplayListHead++);
#endif

    // fix scaling issues
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gViewportFullscreen));
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, BORDER_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - BORDER_HEIGHT);

    // clear screen
    create_dl_translation_matrix(MENU_MTX_PUSH, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), 240.f, 0.f);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, (GFX_DIMENSIONS_ASPECT_RATIO * SCREEN_HEIGHT) / 130.f, 3.f, 1.f);
    gDPSetEnvColor(gDisplayListHead++, clearColorR, clearColorG, clearColorB, 0xFF);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);

    // call the callback
    callback();

    // render frame
    djui_gfx_displaylist_end();
    end_master_display_list();
    alloc_display_list(0);
    gfx_run((Gfx*) gGfxSPTask->task.t.data_ptr); // send_display_list
    display_and_vsync();

    // delay to go easy on the cpu
    f64 frameEnd = clock_elapsed_f64();
    f64 elapsed = frameEnd - frameStart;
    f64 remaining = targetFrameTime - elapsed;
    if (remaining > 0) {
        gWindowApi->delay((u32)(remaining * 1000.0));
    }

    gfx_end_frame();
}

void audio_shutdown(void) {
    if (gAudioApi) {
        if (gAudioApi->shutdown) gAudioApi->shutdown();
        gAudioApi = NULL;
    }
}

void game_deinit(void) {
    if (gGameInited) { configfile_save(configfile_name()); }
    controller_shutdown();
    audio_shutdown();
    network_shutdown(true, true, false, false);
    smlua_text_utils_shutdown();
    smlua_shutdown();
    mods_shutdown();
    djui_shutdown();
    gfx_shutdown();
    gGameInited = false;
}

void game_exit(void) {
    LOG_INFO("exiting cleanly");
    game_deinit();
    exit(0);
}

void* main_game_init(UNUSED void* dummy) {
    // load language
    if (!djui_language_init(configLanguage)) { snprintf(configLanguage, MAX_CONFIG_STRING, "%s", ""); }

    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Loading"));
    dynos_gfx_init();
    enable_queued_dynos_packs();
    sync_objects_init_system();

    if (gCLIOpts.network != NT_SERVER && !gCLIOpts.skipUpdateCheck) {
        check_for_updates();
    }

    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Loading ROM Assets"));
    rom_assets_load();
    smlua_text_utils_init();

    mods_init();
    enable_queued_mods();
    LOADING_SCREEN_MUTEX(
        gCurrLoadingSegment.percentage = 0;
        loading_screen_set_segment_text("Starting Game");
    );

    audio_init();
    sound_init();
    network_player_init();
    mumble_init();

    gGameInited = true;
    return NULL;
}

int main(int argc, char *argv[]) {
    // handle terminal arguments
    if (!parse_cli_opts(argc, argv)) { return 0; }

#ifdef _WIN32
    // handle Windows console
    if (gCLIOpts.console || gCLIOpts.headless) {
        SetConsoleOutputCP(CP_UTF8);
    } else {
        FreeConsole();
        freopen("NUL", "w", stdout);
    }
#endif

#ifdef _WIN32
    if (gCLIOpts.savePath[0]) {
        char portable_path[SYS_MAX_PATH] = {};
        sys_windows_short_path_from_mbs(portable_path, SYS_MAX_PATH, gCLIOpts.savePath);
        fs_init(portable_path);
    } else {
        fs_init(sys_user_path());
    }
#else
    fs_init(gCLIOpts.savePath[0] ? gCLIOpts.savePath : sys_user_path());
#endif

    configfile_load();

    legacy_folder_handler();

    select_graphics_backend();

    // create the window almost straight away
    if (!gGfxInited) {
        gfx_init(gWindowApi, gRenderApi, TITLE);
        gWindowApi->set_keyboard_callbacks(keyboard_on_key_down, keyboard_on_key_up, keyboard_on_all_keys_up,
            keyboard_on_text_input, keyboard_on_text_editing);
        gWindowApi->set_scroll_callback(mouse_on_scroll);
    }

    // render the rom setup screen
    if (!main_rom_handler()) {
        if (!gCLIOpts.hideLoadingScreen) {
            render_rom_setup_screen(); // holds the game load until a valid rom is provided
        } else {
            printf("ERROR: could not find valid vanilla us sm64 rom in game's user folder\n");
            return 0;
        }
    }

    // start the thread for setting up the game
    bool threadSuccess = false;
    if (!gCLIOpts.hideLoadingScreen && !gCLIOpts.headless) {
        if (init_thread_handle(&gLoadingThread, main_game_init, NULL, NULL, 0) == 0) {
            render_loading_screen(); // render the loading screen while the game is setup
            threadSuccess = true;
            destroy_mutex(&gLoadingThread);
        }
    }
    if (!threadSuccess) {
        main_game_init(NULL); // failsafe incase threading doesn't work
    }

    // initialize sm64 data and controllers
    thread5_game_loop(NULL);

    // Initialize the audio thread if possible.
    //
    // Off by default: upstream disabled this in 50b727b41 ("disable audio
    // threading until it seems stable") and the underlying races were never
    // resolved. It is opt-in rather than deleted because on a quad-core A35
    // handheld, where the game otherwise does networking, game logic, Lua,
    // synthesis and rendering on a single core, moving the software synthesizer
    // to a second core is the largest structural win available. The locking it
    // needs is already in place throughout src/audio/external.c.
    //
    // Enable with the line `audio_threaded true` in sm64config.txt -- the
    // config parser compares against the literal string "true"
    // (configfile.c), so "1", "TRUE" and "yes" all silently read as false.
    // Edit it with the game closed: config is re-saved during startup and
    // again on every settings change, so a value that failed to parse gets
    // written back as false and the edit is lost. Soak-test in a mod-heavy
    // lobby before trusting it.
    if (configAudioThreaded) {
        init_thread_handle(&gAudioThread, audio_thread, NULL, NULL, 0);
    }

    loading_screen_reset();

    // initialize djui
    djui_init();
    djui_unicode_init();
    djui_init_late();
    djui_console_message_dequeue();

    show_update_popup();

    if (can_update_game()) {
        djui_open_update_panel();
    }

    // initialize network
    if (gCLIOpts.network == NT_CLIENT) {
        network_set_system(NS_SOCKET);
        snprintf(gGetHostName, MAX_CONFIG_STRING, "%s", gCLIOpts.joinIp);
        snprintf(configJoinIp, MAX_CONFIG_STRING, "%s", gCLIOpts.joinIp);
        configJoinPort = gCLIOpts.networkPort;
        network_init(NT_CLIENT, false);
    } else if (gCLIOpts.network == NT_SERVER || gCLIOpts.coopnet) {
        if (gCLIOpts.network == NT_SERVER) {
            configNetworkSystem = NS_SOCKET;
            configHostPort = gCLIOpts.networkPort;
        } else {
            configNetworkSystem = NS_COOPNET;
            snprintf(configPassword, MAX_CONFIG_STRING, "%s", gCLIOpts.coopnetPassword);
        }

        // horrible, hacky fix for mods that access marioObj straight away
        // best fix: host with the standard main menu method
        static struct Object sHackyObject = { 0 };
        gMarioStates[0].marioObj = &sHackyObject;

        extern void djui_panel_do_host(bool reconnecting, bool playSound);
        djui_panel_do_host(NULL, false);
    } else {
        network_init(NT_NONE, false);
    }

    // opens the CSV and starts the sampler; no-op unless built with PROFILE=1
    profile_log_init();

    // main loop
    while (true) {
        debug_context_reset();
        CTX_BEGIN(CTX_TOTAL);
        gWindowApi->main_loop(produce_one_frame);
#ifdef DISCORD_SDK
        discord_update();
#endif
        mumble_update();
#ifdef DEBUG
        fflush(stdout);
        fflush(stderr);
#endif
        CTX_END(CTX_TOTAL);

        profile_log_frame();

#ifdef CTX_TIMING_ENABLED
        djui_ctx_display_update();
#endif
        djui_lua_profiler_update();
    }

    return 0;
}
