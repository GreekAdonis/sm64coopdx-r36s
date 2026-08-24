#include <stdint.h>
#include <stdbool.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

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

#include "../platform.h"
#include "../configfile.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_pc.h"
#include "pc/debug_context.h"
#include "pc/profile_log.h"

#define TEX_CACHE_STEP 512

struct ShaderProgram {
    uint64_t hash;
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[7];
    GLint uniform_locations[9];
    uint8_t attrib_sizes[7];
    uint8_t num_attribs;
    bool used_noise;
    bool used_lightmap;
    bool world_geometry;
    // Last value of configFiltering uploaded to this program's filtering
    // uniform, or -1 if it has never been uploaded. configFiltering only changes
    // when the player changes a setting, but the uniform was being re-sent on
    // every single shader switch.
    int uploaded_filtering;

    // The same idea applied to the rest of the per-program uniforms. A shader
    // switch used to re-upload all of them unconditionally, including two
    // SHADER_FLAG_MAX-element arrays, and there are ~45 switches a frame going
    // into a driver that is already a quarter of the main thread.
    //
    // These are compared by value rather than driven by a dirty flag on the
    // globals, so no writer anywhere has to cooperate to keep them correct.
    // Each array is 8 elements; a memcmp of 32 bytes is far cheaper than the
    // glUniform*v call it avoids.
    uint32_t uploaded_noise_frame;   // frame_count at the last noise upload
    bool     uploaded_noise_valid;
    bool     uploaded_lightmap_valid;
    uint8_t  uploaded_lightmap[3];
    bool     uploaded_flags_valid;
    int      uploaded_flags[SHADER_FLAG_MAX];
    f32      uploaded_flag_values[SHADER_FLAG_MAX];
    bool     uploaded_tex_valid[2];
    GLfloat  uploaded_tex_size[2][2];
    int      uploaded_tex_filter[2];
};

struct GLTexture {
    GLuint gltex;
    GLfloat size[2];
    bool filter;
};

static struct ShaderProgram shader_program_pool[CC_MAX_SHADERS];
static uint8_t shader_program_pool_size = 0;
static uint8_t shader_program_pool_index = 0;
static GLuint opengl_vbo;
static GLuint opengl_vao;

static int tex_cache_size = 0;
static int num_textures = 0;
static struct GLTexture *tex_cache = NULL;

static struct ShaderProgram *opengl_prg = NULL;
static struct GLTexture *opengl_tex[2];
static int opengl_curtex = 0;

static uint32_t frame_count;

#ifdef HANDHELD
// RK3326 (Mali-G31) is fill-rate bound at the panel's fixed 1024x768 mode; there
// is no lower display mode to fall back on, so instead the 3D+HUD scene is
// rendered into a lower-resolution offscreen FBO and upscaled to the window with
// a single blit in gfx_opengl_finish_render(). This cuts fragment shader
// invocations to (320*240)/(1024*768) =~ 10% of native at 4:3 (320x240 also
// happens to match the original N64 SM64 render resolution), with the window
// staying at the panel's native mode the whole time. The internal resolution
// is configurable (configHandheldResW/H); these are just the defaults used
// when unconfigured (0).
#define HANDHELD_FBO_DEFAULT_WIDTH  320
#define HANDHELD_FBO_DEFAULT_HEIGHT 240

static GLuint sHandheldFbo;
static GLuint sHandheldColorTex;
static GLuint sHandheldDepthRbo;
static GLuint sHandheldBlitProgram;
static GLint sHandheldBlitPosLoc = -1;
static GLint sHandheldBlitSrcSizeLoc = -1;
static GLint sHandheldBlitScaleLoc = -1;
static uint32_t sHandheldFboW = HANDHELD_FBO_DEFAULT_WIDTH;
static uint32_t sHandheldFboH = HANDHELD_FBO_DEFAULT_HEIGHT;
static uint32_t sHandheldFboWindowW;
static uint32_t sHandheldFboWindowH;
static bool sHandheldFboReady;
// True while draw calls should be rescaled into the low-res FBO (the 3D world
// pass); false once G_HANDHELD_HUD_PASS_EXT has switched to rendering the HUD
// straight to the window at native resolution for the rest of the frame.
static bool sHandheldWorldPassActive;
#ifdef USE_GLES
// GL_EXT_discard_framebuffer lets us tell Mali's tile-based renderer that an
// attachment's contents don't need to be flushed back to system RAM at the
// end of the tile pass. Resolved via SDL_GL_GetProcAddress (not linked
// directly): the symbol is present in the Mali driver at runtime, but cross-
// compile toolchains commonly ship a stub libGLESv2 that doesn't export it,
// which fails the link even though the symbol would resolve fine on-device.
// PFNGLDISCARDFRAMEBUFFEREXTPROC comes from gl2ext.h (declared regardless of
// GL_GLEXT_PROTOTYPES, unlike the direct glDiscardFramebufferEXT symbol).
static PFNGLDISCARDFRAMEBUFFEREXTPROC sHandheldDiscardFramebufferEXT;
#endif

static GLuint gfx_opengl_handheld_compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "handheld blit shader compile failed: %s\n", log);
        sys_fatal("handheld internal-resolution blit shader failed to compile");
    }
    return shader;
}

static void gfx_opengl_handheld_create_blit_program(void) {
    // "sharp-bilinear": ordinary bilinear filtering blurs every texel edge,
    // including ones that land exactly on a source-pixel boundary, which is
    // what makes the plain upscale look soft. This snaps each sample to the
    // nearest source pixel except within a thin blend band (sized from the
    // upscale ratio) around each texel edge, so pixel interiors stay crisp
    // while edges still get an antialiased transition instead of nearest-
    // neighbor's hard/shimmery step. uSrcSize is the FBO size in texels;
    // uScale is (window size / FBO size) per axis.
#ifdef USE_GLES
    const char *vs_src =
        "#version 100\n"
        "attribute vec2 aPos;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    vUV = aPos * 0.5 + 0.5;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    const char *fs_src =
        "#version 100\n"
        "precision mediump float;\n"
        "uniform sampler2D uTex;\n"
        "uniform vec2 uSrcSize;\n"
        "uniform vec2 uScale;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    vec2 texel = vUV * uSrcSize;\n"
        "    vec2 texelFloor = floor(texel);\n"
        "    vec2 s = fract(texel);\n"
        "    vec2 regionRange = 0.5 - 0.5 / uScale;\n"
        "    vec2 centerDist = s - 0.5;\n"
        "    vec2 f = (centerDist - clamp(centerDist, -regionRange, regionRange)) * uScale + 0.5;\n"
        "    vec2 modTexel = texelFloor + f;\n"
        "    gl_FragColor = texture2D(uTex, modTexel / uSrcSize);\n"
        "}\n";
#else
    const char *vs_src =
        "#version 120\n"
        "attribute vec2 aPos;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    vUV = aPos * 0.5 + 0.5;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    const char *fs_src =
        "#version 120\n"
        "uniform sampler2D uTex;\n"
        "uniform vec2 uSrcSize;\n"
        "uniform vec2 uScale;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    vec2 texel = vUV * uSrcSize;\n"
        "    vec2 texelFloor = floor(texel);\n"
        "    vec2 s = fract(texel);\n"
        "    vec2 regionRange = 0.5 - 0.5 / uScale;\n"
        "    vec2 centerDist = s - 0.5;\n"
        "    vec2 f = (centerDist - clamp(centerDist, -regionRange, regionRange)) * uScale + 0.5;\n"
        "    vec2 modTexel = texelFloor + f;\n"
        "    gl_FragColor = texture2D(uTex, modTexel / uSrcSize);\n"
        "}\n";
#endif
    GLuint vs = gfx_opengl_handheld_compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = gfx_opengl_handheld_compile_shader(GL_FRAGMENT_SHADER, fs_src);

    sHandheldBlitProgram = glCreateProgram();
    glAttachShader(sHandheldBlitProgram, vs);
    glAttachShader(sHandheldBlitProgram, fs);
    glLinkProgram(sHandheldBlitProgram);
    GLint linked;
    glGetProgramiv(sHandheldBlitProgram, GL_LINK_STATUS, &linked);
    if (!linked) {
        sys_fatal("handheld internal-resolution blit program failed to link");
    }

    sHandheldBlitPosLoc = glGetAttribLocation(sHandheldBlitProgram, "aPos");
    GLint sampler_loc = glGetUniformLocation(sHandheldBlitProgram, "uTex");
    sHandheldBlitSrcSizeLoc = glGetUniformLocation(sHandheldBlitProgram, "uSrcSize");
    sHandheldBlitScaleLoc = glGetUniformLocation(sHandheldBlitProgram, "uScale");
    glUseProgram(sHandheldBlitProgram);
    glUniform1i(sampler_loc, 0);
}

static void gfx_opengl_handheld_destroy_fbo(void) {
    if (sHandheldFbo) { glDeleteFramebuffers(1, &sHandheldFbo); sHandheldFbo = 0; }
    if (sHandheldColorTex) { glDeleteTextures(1, &sHandheldColorTex); sHandheldColorTex = 0; }
    if (sHandheldDepthRbo) { glDeleteRenderbuffers(1, &sHandheldDepthRbo); sHandheldDepthRbo = 0; }
    sHandheldFboReady = false;
}

// Lazily (re)creates the internal-resolution FBO whenever the window size or
// the configured internal resolution changes. A no-op on every frame after
// the first once both are stable.
static void gfx_opengl_handheld_ensure_fbo(uint32_t window_w, uint32_t window_h) {
    uint32_t target_w = configHandheldResW ? configHandheldResW : HANDHELD_FBO_DEFAULT_WIDTH;
    uint32_t target_h = configHandheldResH ? configHandheldResH : HANDHELD_FBO_DEFAULT_HEIGHT;

    if (sHandheldFboReady && sHandheldFboWindowW == window_w && sHandheldFboWindowH == window_h
        && sHandheldFboW == target_w && sHandheldFboH == target_h) {
        return;
    }
    gfx_opengl_handheld_destroy_fbo();
    sHandheldFboWindowW = window_w;
    sHandheldFboWindowH = window_h;
    sHandheldFboW = target_w;
    sHandheldFboH = target_h;

    // Only worth downscaling if the window actually exceeds the internal target;
    // otherwise just render straight to the window like the non-HANDHELD path.
    if (window_w == 0 || window_h == 0
        || (window_w <= sHandheldFboW && window_h <= sHandheldFboH)) {
        return;
    }

    glGenTextures(1, &sHandheldColorTex);
    glBindTexture(GL_TEXTURE_2D, sHandheldColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sHandheldFboW, sHandheldFboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &sHandheldDepthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, sHandheldDepthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, sHandheldFboW, sHandheldFboH);

    glGenFramebuffers(1, &sHandheldFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sHandheldFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHandheldColorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sHandheldDepthRbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    // Bypasses select_texture()'s tile bookkeeping (and we don't know which
    // unit was active), so drop both cached tiles rather than risk
    // select_texture() trusting a unit that's actually unbound now.
    opengl_tex[0] = NULL;
    opengl_tex[1] = NULL;
    // gfx_pc.c skips imports it can prove are no-ops, which means it will not
    // re-issue select_texture() on its own. Tell it the binding is gone.
    gfx_texture_state_invalidate();

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "handheld internal-resolution FBO incomplete (0x%x); rendering at native resolution\n", status);
        gfx_opengl_handheld_destroy_fbo();
        return;
    }

    if (!sHandheldBlitProgram) {
        gfx_opengl_handheld_create_blit_program();
    }

    sHandheldFboReady = true;
}
#endif

static bool gfx_opengl_z_is_from_0_to_1(void) {
    return false;
}

  ////////////////////////////////
 // vertex attribute array state //
////////////////////////////////

// A shader switch used to disable every attribute array of the outgoing
// program and then enable every array of the incoming one, plus re-specify a
// pointer for each. The programs in this renderer share a vertex layout family,
// so most of those calls set the state to what it already was -- and there are
// ~45 switches a frame.
//
// Mirror the state instead and issue only the difference. Attribute pointer
// state is per attribute index and survives the glBufferData re-upload in
// draw_triangles, so a pointer only has to be re-sent when the layout for that
// index actually changes.
#define GFX_MAX_ATTRIB_INDEX 32

struct GLAttribLayout {
    bool valid;
    uint8_t size;
    uint8_t num_floats;
    uint8_t offset;
};

static uint32_t sEnabledAttribs = 0;
static struct GLAttribLayout sAttribLayout[GFX_MAX_ATTRIB_INDEX];

// Call after driving the attribute arrays outside this bookkeeping, so the
// mirror stops claiming state GL no longer has.
static void gfx_opengl_attrib_state_invalidate(GLint loc) {
    if (loc < 0 || loc >= GFX_MAX_ATTRIB_INDEX) { return; }
    sEnabledAttribs &= ~(1u << loc);
    sAttribLayout[loc].valid = false;
}

static void gfx_opengl_vertex_array_set_attribs(struct ShaderProgram *prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;
    uint32_t want = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        GLint loc = prg->attrib_locations[i];
        // A location of -1 means the compiler dropped the attribute; passing it
        // to glEnableVertexAttribArray only ever raised a GL error.
        if (loc >= 0 && loc < GFX_MAX_ATTRIB_INDEX) {
            want |= 1u << loc;

            struct GLAttribLayout *have = &sAttribLayout[loc];
            if (!have->valid
                || have->size != prg->attrib_sizes[i]
                || have->num_floats != num_floats
                || have->offset != pos) {
                glVertexAttribPointer(loc, prg->attrib_sizes[i], GL_FLOAT, GL_FALSE,
                                      num_floats * sizeof(float), (void *) (pos * sizeof(float)));
                have->valid      = true;
                have->size       = (uint8_t) prg->attrib_sizes[i];
                have->num_floats = (uint8_t) num_floats;
                have->offset     = (uint8_t) pos;
            }
        }
        pos += prg->attrib_sizes[i];
    }

    for (uint32_t d = want & ~sEnabledAttribs; d != 0; d &= d - 1) {
        glEnableVertexAttribArray((GLuint) __builtin_ctz(d));
    }
    for (uint32_t d = sEnabledAttribs & ~want; d != 0; d &= d - 1) {
        glDisableVertexAttribArray((GLuint) __builtin_ctz(d));
    }
    sEnabledAttribs = want;
}

static inline void gfx_opengl_set_shader_uniforms(struct ShaderProgram *prg) {
    // The noise seed is the frame counter, so it changes once a frame no matter
    // how many times the program is bound within it.
    if (prg->used_noise && (!prg->uploaded_noise_valid || prg->uploaded_noise_frame != frame_count)) {
        glUniform1f(prg->uniform_locations[4], (float)frame_count);
        prg->uploaded_noise_valid = true;
        prg->uploaded_noise_frame = frame_count;
    }
    if (prg->used_lightmap
        && (!prg->uploaded_lightmap_valid
            || prg->uploaded_lightmap[0] != gVertexColor[0]
            || prg->uploaded_lightmap[1] != gVertexColor[1]
            || prg->uploaded_lightmap[2] != gVertexColor[2])) {
        glUniform3f(prg->uniform_locations[5], gVertexColor[0] / 255.0f, gVertexColor[1] / 255.0f, gVertexColor[2] / 255.0f);
        prg->uploaded_lightmap_valid = true;
        prg->uploaded_lightmap[0] = gVertexColor[0];
        prg->uploaded_lightmap[1] = gVertexColor[1];
        prg->uploaded_lightmap[2] = gVertexColor[2];
    }
    if (prg->world_geometry) {
        // Two array uploads that only change when a mod changes a shader flag,
        // which is essentially never, against ~45 shader binds a frame.
        if (!prg->uploaded_flags_valid
            || memcmp(prg->uploaded_flags, gShaderFlags, sizeof(prg->uploaded_flags)) != 0) {
            glUniform1iv(prg->uniform_locations[6], SHADER_FLAG_MAX, gShaderFlags);
            memcpy(prg->uploaded_flags, gShaderFlags, sizeof(prg->uploaded_flags));
        }
        if (!prg->uploaded_flags_valid
            || memcmp(prg->uploaded_flag_values, gShaderFlagValues, sizeof(prg->uploaded_flag_values)) != 0) {
            glUniform1fv(prg->uniform_locations[7], SHADER_FLAG_MAX, gShaderFlagValues);
            memcpy(prg->uploaded_flag_values, gShaderFlagValues, sizeof(prg->uploaded_flag_values));
        }
        prg->uploaded_flags_valid = true;
    }

    if (prg->uploaded_filtering != (int) configFiltering) {
        glUniform1i(prg->uniform_locations[8], configFiltering);
        prg->uploaded_filtering = (int) configFiltering;
    }
}

static inline void gfx_opengl_set_texture_uniforms(struct ShaderProgram *prg, const int tile) {
    if (prg->used_textures[tile] && opengl_tex[tile]) {
        if (!prg->uploaded_tex_valid[tile]
            || prg->uploaded_tex_size[tile][0] != opengl_tex[tile]->size[0]
            || prg->uploaded_tex_size[tile][1] != opengl_tex[tile]->size[1]) {
            glUniform2f(prg->uniform_locations[tile*2 + 0], opengl_tex[tile]->size[0], opengl_tex[tile]->size[1]);
            prg->uploaded_tex_size[tile][0] = opengl_tex[tile]->size[0];
            prg->uploaded_tex_size[tile][1] = opengl_tex[tile]->size[1];
        }
        if (!prg->uploaded_tex_valid[tile]
            || prg->uploaded_tex_filter[tile] != (int) opengl_tex[tile]->filter) {
            glUniform1i(prg->uniform_locations[tile*2 + 1], opengl_tex[tile]->filter);
            prg->uploaded_tex_filter[tile] = (int) opengl_tex[tile]->filter;
        }
        prg->uploaded_tex_valid[tile] = true;
    }
}

static void gfx_opengl_unload_shader(struct ShaderProgram *old_prg) {
    // No longer disables the outgoing program's attribute arrays. Every
    // unload here is immediately followed by a load, and
    // gfx_opengl_vertex_array_set_attribs() issues the enable/disable
    // difference against what is actually on -- so disabling them first only
    // guaranteed the next load would have to turn most of them back on.
    if (old_prg != NULL) {
        if (old_prg == opengl_prg)
            opengl_prg = NULL;
    } else {
        opengl_prg = NULL;
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram *new_prg) {
    opengl_prg = new_prg;
    glUseProgram(new_prg->opengl_program_id);
    gfx_opengl_vertex_array_set_attribs(new_prg);
    gfx_opengl_set_shader_uniforms(new_prg);
    gfx_opengl_set_texture_uniforms(new_prg, 0);
    gfx_opengl_set_texture_uniforms(new_prg, 1);
}

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_INPUT_5:
                return with_alpha || !inputs_have_alpha ? "vInput5" : "vInput5.rgb";
            case SHADER_INPUT_6:
                return with_alpha || !inputs_have_alpha ? "vInput6" : "vInput6.rgb";
            case SHADER_INPUT_7:
                return with_alpha || !inputs_have_alpha ? "vInput7" : "vInput7.rgb";
            case SHADER_INPUT_8:
                return with_alpha || !inputs_have_alpha ? "vInput8" : "vInput8.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)" : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a" :
                    (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)" : "vec3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_COMBINEDA:
                return hint_single_element ? "texel.a" :
                    (with_alpha ? "vec4(texel.a, texel.a, texel.a, texel.a)" : "vec3(texel.a, texel.a, texel.a)");
            case SHADER_NOISE:
                return with_alpha ? "vec4(noise)" : "vec3(noise)";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_INPUT_5:
                return "vInput5.a";
            case SHADER_INPUT_6:
                return "vInput6.a";
            case SHADER_INPUT_7:
                return "vInput7.a";
            case SHADER_INPUT_8:
                return "vInput8.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_COMBINEDA:
                return "texel.a";
            case SHADER_NOISE:
                return "noise.a";
        }
    }
    return "unknown";
}

static void append_formula(char *buf, size_t *len, uint8_t* cmd, bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(struct ColorCombiner* cc) {
    struct CCFeatures ccf = { 0 };
    gfx_cc_get_features(cc, &ccf);

    bool opt_alpha = cc->cm.use_alpha;
    bool opt_fog = cc->cm.use_fog;
    bool opt_texture_edge = cc->cm.texture_edge;
    bool opt_2cycle = cc->cm.use_2cycle;
    bool opt_light_map = cc->cm.light_map;
    bool world_geometry = cc->cm.world_geometry;

#ifdef USE_GLES
    bool opt_dither = false;
#else
    bool opt_dither = cc->cm.use_dither;
#endif

    char vs_buf[8192];
    char fs_buf[8192];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;

    // Vertex shader
#ifdef USE_GLES
    append_line(vs_buf, &vs_len, "#version 100");
#else
    append_line(vs_buf, &vs_len, "#version 120");
#endif
    append_line(vs_buf, &vs_len, "attribute vec4 aVtxPos;");
    for (int t = 0; t < 2; t++) {
        if (ccf.used_textures[t]) {
            vs_len += sprintf(vs_buf + vs_len, "attribute vec2 aTexCoord%d;\n", t);
            vs_len += sprintf(vs_buf + vs_len, "varying vec2 vTexCoord%d;\n", t);
            num_floats += 2;
        }
    }
    if (opt_fog) {
        append_line(vs_buf, &vs_len, "attribute vec4 aFog;");
        append_line(vs_buf, &vs_len, "varying vec4 vFog;");
        num_floats += 4;
    }
    if (opt_light_map) {
        append_line(vs_buf, &vs_len, "attribute vec2 aLightMap;");
        append_line(vs_buf, &vs_len, "varying vec2 vLightMap;");
        num_floats += 2;
    }
    for (int i = 0; i < ccf.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "attribute vec%d aInput%d;\n", opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "varying vec%d vInput%d;\n", opt_alpha ? 4 : 3, i + 1);
        num_floats += opt_alpha ? 4 : 3;
    }
    append_line(vs_buf, &vs_len, "void main() {");
    for (int t = 0; t < 2; t++) {
        if (ccf.used_textures[t]) {
            vs_len += sprintf(vs_buf + vs_len, "vTexCoord%d = aTexCoord%d;\n", t, t);
        }
    }
    if (opt_fog) {
        append_line(vs_buf, &vs_len, "vFog = aFog;");
    }
    if (opt_light_map) {
        append_line(vs_buf, &vs_len, "vLightMap = aLightMap;");
    }
    for (int i = 0; i < ccf.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    append_line(vs_buf, &vs_len, "gl_Position = aVtxPos;");
    append_line(vs_buf, &vs_len, "}");

    // Fragment shader
#ifdef USE_GLES
    append_line(fs_buf, &fs_len, "#version 100");
    append_line(fs_buf, &fs_len, "precision mediump float;");
#else
    append_line(fs_buf, &fs_len, "#version 120");
#endif

    for (int t = 0; t < 2; t++) {
        if (ccf.used_textures[t]) {
            fs_len += sprintf(fs_buf + fs_len, "varying vec2 vTexCoord%d;\n", t);
        }
    }
    if (opt_fog) {
        append_line(fs_buf, &fs_len, "varying vec4 vFog;");
    }
    if (opt_light_map) {
        append_line(fs_buf, &fs_len, "varying vec2 vLightMap;");
    }
    for (int i = 0; i < ccf.num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, "varying vec%d vInput%d;\n", opt_alpha ? 4 : 3, i + 1);
    }

    for (int t = 0; t < 2; t++) {
        if (ccf.used_textures[t]) {
            fs_len += sprintf(fs_buf + fs_len, "uniform sampler2D uTex%d;\n", t);
            fs_len += sprintf(fs_buf + fs_len, "uniform vec2 uTex%dSize;\n", t);
            fs_len += sprintf(fs_buf + fs_len, "uniform bool uTex%dFilter;\n", t);
        }
    }

    // 3 point texture filtering
    // Original author: ArthurCarvalho
    // Modified GLSL implementation by twinaphex, mupen64plus-libretro project.
    if (ccf.used_textures[0] || ccf.used_textures[1]) {
        append_line(fs_buf, &fs_len, "#define TEX_OFFSET(off) texture2D(tex, texCoord - (off)/texSize)");
        append_line(fs_buf, &fs_len, "vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {");
        append_line(fs_buf, &fs_len, "    vec2 offset = fract(texCoord*texSize - vec2(0.5));");
        append_line(fs_buf, &fs_len, "    offset -= step(1.0, offset.x + offset.y);");
        append_line(fs_buf, &fs_len, "    vec4 c0 = TEX_OFFSET(offset);");
        append_line(fs_buf, &fs_len, "    vec4 c1 = TEX_OFFSET(vec2(offset.x - sign(offset.x), offset.y));");
        append_line(fs_buf, &fs_len, "    vec4 c2 = TEX_OFFSET(vec2(offset.x, offset.y - sign(offset.y)));");
        append_line(fs_buf, &fs_len, "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
        append_line(fs_buf, &fs_len, "}");
        append_line(fs_buf, &fs_len, "vec4 sampleTex(in sampler2D tex, in vec2 uv, in vec2 texSize, in bool dofilter, in int filter) {");
        append_line(fs_buf, &fs_len, "    if (dofilter && filter == 2)");
        append_line(fs_buf, &fs_len, "        return filter3point(tex, uv, texSize);");
        append_line(fs_buf, &fs_len, "    else");
        append_line(fs_buf, &fs_len, "        return texture2D(tex, uv);");
        append_line(fs_buf, &fs_len, "}");
    }

    if (world_geometry) {
        append_line(fs_buf, &fs_len, "float dither4x4(vec2 position, float brightness) {");
        append_line(fs_buf, &fs_len, "    int x = int(mod(position.x, 4.0));");
        append_line(fs_buf, &fs_len, "    int y = int(mod(position.y, 4.0));");
        append_line(fs_buf, &fs_len, "    int index = x + y * 4;");
        append_line(fs_buf, &fs_len, "    float limit = 0.0;");
        append_line(fs_buf, &fs_len, "    if (x < 8) {");
        append_line(fs_buf, &fs_len, "        if (index == 0) limit = 0.0625;");
        append_line(fs_buf, &fs_len, "        if (index == 1) limit = 0.5625;");
        append_line(fs_buf, &fs_len, "        if (index == 2) limit = 0.1875;");
        append_line(fs_buf, &fs_len, "        if (index == 3) limit = 0.6875;");
        append_line(fs_buf, &fs_len, "        if (index == 4) limit = 0.8125;");
        append_line(fs_buf, &fs_len, "        if (index == 5) limit = 0.3125;");
        append_line(fs_buf, &fs_len, "        if (index == 6) limit = 0.9375;");
        append_line(fs_buf, &fs_len, "        if (index == 7) limit = 0.4375;");
        append_line(fs_buf, &fs_len, "        if (index == 8) limit = 0.25;");
        append_line(fs_buf, &fs_len, "        if (index == 9) limit = 0.75;");
        append_line(fs_buf, &fs_len, "        if (index == 10) limit = 0.125;");
        append_line(fs_buf, &fs_len, "        if (index == 11) limit = 0.625;");
        append_line(fs_buf, &fs_len, "        if (index == 12) limit = 1.0;");
        append_line(fs_buf, &fs_len, "        if (index == 13) limit = 0.5;");
        append_line(fs_buf, &fs_len, "        if (index == 14) limit = 0.875;");
        append_line(fs_buf, &fs_len, "        if (index == 15) limit = 0.375;");
        append_line(fs_buf, &fs_len, "    }");
        append_line(fs_buf, &fs_len, "    return brightness < limit ? 0.0 : 1.0;");
        append_line(fs_buf, &fs_len, "}");

        append_line(fs_buf, &fs_len, "vec3 rgb2hsv(vec3 c) {");
        append_line(fs_buf, &fs_len, "    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);");
        append_line(fs_buf, &fs_len, "    vec4 p = mix(vec4(c.bg, K.wz),");
        append_line(fs_buf, &fs_len, "                 vec4(c.gb, K.xy),");
        append_line(fs_buf, &fs_len, "                 step(c.b, c.g));");
        append_line(fs_buf, &fs_len, "    vec4 q = mix(vec4(p.xyw, c.r),");
        append_line(fs_buf, &fs_len, "                 vec4(c.r, p.yzx),");
        append_line(fs_buf, &fs_len, "                 step(p.x, c.r));");
        append_line(fs_buf, &fs_len, "    float d = q.x - min(q.w, q.y);");
        append_line(fs_buf, &fs_len, "    float e = 1.0e-10;");
        append_line(fs_buf, &fs_len, "    return vec3(");
        append_line(fs_buf, &fs_len, "        abs(q.z + (q.w - q.y) / (6.0 * d + e)), // hue");
        append_line(fs_buf, &fs_len, "        d / (q.x + e),                          // saturation");
        append_line(fs_buf, &fs_len, "        q.x                                     // value");
        append_line(fs_buf, &fs_len, "    );");
        append_line(fs_buf, &fs_len, "}");
        append_line(fs_buf, &fs_len, "");
        append_line(fs_buf, &fs_len, "vec3 hsv2rgb(vec3 c) {");
        append_line(fs_buf, &fs_len, "    vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);");
        append_line(fs_buf, &fs_len, "    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);");
        append_line(fs_buf, &fs_len, "}");
    }

    if ((opt_alpha && opt_dither) || ccf.do_noise) {
        append_line(fs_buf, &fs_len, "uniform float uFrameCount;");

        append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
        append_line(fs_buf, &fs_len, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
        append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
        append_line(fs_buf, &fs_len, "}");
    }

    if (opt_light_map) {
        append_line(fs_buf, &fs_len, "uniform vec3 uLightmapColor;");
    }

    if (world_geometry) {
        fs_len += sprintf(fs_buf + fs_len, "uniform int uShaderFlags[%d];\n", SHADER_FLAG_MAX);
        fs_len += sprintf(fs_buf + fs_len, "uniform float uShaderFlagValues[%d];\n", SHADER_FLAG_MAX);
    }

    append_line(fs_buf, &fs_len, "uniform int uFilter;");

    append_line(fs_buf, &fs_len, "void main() {");

    if ((opt_alpha && opt_dither) || ccf.do_noise) {
        append_line(fs_buf, &fs_len, "float noise = floor(random(floor(vec3(gl_FragCoord.xy, uFrameCount))) + 0.5);");
    }

    if (ccf.used_textures[0]) {
        append_line(fs_buf, &fs_len, "vec4 texVal0 = sampleTex(uTex0, vTexCoord0, uTex0Size, uTex0Filter, uFilter);");
    }
    if (ccf.used_textures[1]) {
        if (opt_light_map) {
            append_line(fs_buf, &fs_len, "vec4 texVal1 = sampleTex(uTex1, vLightMap, uTex1Size, uTex1Filter, uFilter);");
            append_line(fs_buf, &fs_len, "texVal0.rgb *= uLightmapColor.rgb;");
            append_line(fs_buf, &fs_len, "texVal1.rgb = texVal1.rgb * texVal1.rgb + texVal1.rgb;");
        } else {
            append_line(fs_buf, &fs_len, "vec4 texVal1 = sampleTex(uTex1, vTexCoord1, uTex1Size, uTex1Filter, uFilter);");
        }
    }

    append_str(fs_buf, &fs_len, (opt_alpha) ? "vec4 texel = " : "vec3 texel = ");
    for (int i = 0; i < (opt_2cycle + 1); i++) {
        u8* cmd = &cc->shader_commands[i * 8];
        if (!ccf.color_alpha_same[i] && opt_alpha) {
            append_str(fs_buf, &fs_len, "vec4(");
            append_formula(fs_buf, &fs_len, cmd, ccf.do_single[i*2+0], ccf.do_multiply[i*2+0], ccf.do_mix[i*2+0], false, false, true);
            append_str(fs_buf, &fs_len, ", ");
            append_formula(fs_buf, &fs_len, cmd, ccf.do_single[i*2+1], ccf.do_multiply[i*2+1], ccf.do_mix[i*2+1], true, true, true);
            append_str(fs_buf, &fs_len, ")");
        } else {
            append_formula(fs_buf, &fs_len, cmd, ccf.do_single[i*2+0], ccf.do_multiply[i*2+0], ccf.do_mix[i*2+0], opt_alpha, false, opt_alpha);
        }
        append_line(fs_buf, &fs_len, ";");

        if (i == 0 && opt_2cycle) {
            append_str(fs_buf, &fs_len, "texel = ");
        }
    }

    if (opt_texture_edge && opt_alpha) {
        append_line(fs_buf, &fs_len, "if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }

    // TODO discard if alpha is 0?

    if (world_geometry) {
        // hue
        append_line(fs_buf, &fs_len, "if (uShaderFlags[0] == 1) {");
        append_line(fs_buf, &fs_len, "vec3 hsv = rgb2hsv(texel.rgb);");
        append_line(fs_buf, &fs_len, "hsv.x = fract(hsv.x + uShaderFlagValues[0]);");
        append_line(fs_buf, &fs_len, "vec3 finalColor = hsv2rgb(hsv);");
        append_line(fs_buf, &fs_len, "texel.rgb = finalColor;");
        append_line(fs_buf, &fs_len, "}");

        // saturation
        append_line(fs_buf, &fs_len, "if (uShaderFlags[1] == 1) {");
        append_line(fs_buf, &fs_len, "const vec3 w = vec3(0.2125, 0.7154, 0.0721);");
        append_line(fs_buf, &fs_len, "vec3 intensity = vec3(dot(texel.rgb, w));");
        append_line(fs_buf, &fs_len, "texel.rgb = mix(intensity, texel.rgb, uShaderFlagValues[1]);");
        append_line(fs_buf, &fs_len, "}");

        // brightness
        append_line(fs_buf, &fs_len, "if (uShaderFlags[2] == 1) {");
        append_line(fs_buf, &fs_len, "texel.rgb *= uShaderFlagValues[2];");
        append_line(fs_buf, &fs_len, "}");

        // contrast
        append_line(fs_buf, &fs_len, "if (uShaderFlags[3] == 1) {");
        append_line(fs_buf, &fs_len, "texel.rgb = 0.5 + uShaderFlagValues[3] * (texel.rgb - 0.5);");
        append_line(fs_buf, &fs_len, "}");

        // exposure
        append_line(fs_buf, &fs_len, "if (uShaderFlags[4] == 1) {");
        append_line(fs_buf, &fs_len, "texel.rgb = texel.rgb + (uShaderFlagValues[4] - 2.0) * texel.rgb + texel.rgb;");
        append_line(fs_buf, &fs_len, "}");

        // dithering
        append_line(fs_buf, &fs_len, "if (uShaderFlags[5] == 1) {");
        append_line(fs_buf, &fs_len, "texel.rgb *= dither4x4(gl_FragCoord.xy, dot(texel.rgb, vec3(0.299, 0.587, 0.114)));");
        append_line(fs_buf, &fs_len, "}");

        // posterization
        append_line(fs_buf, &fs_len, "if (uShaderFlags[6] == 1) {");
        append_line(fs_buf, &fs_len, "int levels = int(max(1.0, uShaderFlagValues[6]));");
        append_line(fs_buf, &fs_len, "texel.rgb = floor(texel.rgb * float(levels)) / float(levels);");
        append_line(fs_buf, &fs_len, "}");

        // scan lines
        append_line(fs_buf, &fs_len, "if (uShaderFlags[7] == 1) {");
        append_line(fs_buf, &fs_len, "float scan = sin(gl_FragCoord.y * 1.5) * 0.04;");
        append_line(fs_buf, &fs_len, "texel.rgb -= scan * uShaderFlagValues[7];");
        append_line(fs_buf, &fs_len, "}");
    }

    if (opt_fog) {
        if (opt_alpha) {
            append_line(fs_buf, &fs_len, "texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (opt_alpha && opt_dither) {
        append_line(fs_buf, &fs_len, "texel.a *= noise;");
    }

    if (opt_alpha) {
        append_line(fs_buf, &fs_len, "gl_FragColor = texel;");
    } else {
        append_line(fs_buf, &fs_len, "gl_FragColor = vec4(texel, 1.0);");
    }
    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    /*puts("Vertex shader:");
    puts(vs_buf);
    puts("Fragment shader:");
    puts(fs_buf);
    puts("End");*/

    const GLchar *sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { vs_len, fs_len };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Vertex shader compilation failed\n");
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        sys_fatal("vertex shader compilation failed (see terminal)");
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Fragment shader compilation failed\n");
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        sys_fatal("fragment shader compilation failed (see terminal)");
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    size_t cnt = 0;

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_index];
    shader_program_pool_index = (shader_program_pool_index + 1) % CC_MAX_SHADERS;
    if (shader_program_pool_size < CC_MAX_SHADERS) { shader_program_pool_size++; }

    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attrib_sizes[cnt] = 4;
    ++cnt;

    for (int t = 0; t < 2; t++) {
        if (ccf.used_textures[t]) {
            char name[16];
            sprintf(name, "aTexCoord%d", t);
            prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
            prg->attrib_sizes[cnt] = 2;
            ++cnt;
        }
    }

    if (opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    if (opt_light_map) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aLightMap");
        prg->attrib_sizes[cnt] = 2;
        ++cnt;
    }

    for (int i = 0; i < ccf.num_inputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attrib_sizes[cnt] = opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->hash = cc->hash;
    prg->opengl_program_id = shader_program;
    prg->num_inputs = ccf.num_inputs;
    prg->used_textures[0] = ccf.used_textures[0];
    prg->used_textures[1] = ccf.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;
    // Slots in this pool are recycled, so these have to be reset per program
    // rather than relying on zero-initialization. A recycled slot that still
    // claimed its uniforms were uploaded would leave the new program running
    // with whatever the previous occupant last sent.
    prg->uploaded_filtering = -1;
    prg->uploaded_noise_valid = false;
    prg->uploaded_lightmap_valid = false;
    prg->uploaded_flags_valid = false;
    prg->uploaded_tex_valid[0] = false;
    prg->uploaded_tex_valid[1] = false;

    glUseProgram(shader_program);
    for (int t = 0; t < 2; t++) {
        if (ccf.used_textures[t]) {
            char name[16];
            sprintf(name, "uTex%d", t);
            GLint sampler_location = glGetUniformLocation(shader_program, name);
            sprintf(name, "uTex%dSize", t);
            prg->uniform_locations[t * 2] = glGetUniformLocation(shader_program, name);
            sprintf(name, "uTex%dFilter", t);
            prg->uniform_locations[t * 2 + 1] = glGetUniformLocation(shader_program, name);
            glUniform1i(sampler_location, t);
        }
    }

    if ((opt_alpha && opt_dither) || ccf.do_noise) {
        prg->uniform_locations[4] = glGetUniformLocation(shader_program, "uFrameCount");
        prg->used_noise = true;
    } else {
        prg->used_noise = false;
    }

    if (opt_light_map) {
        prg->uniform_locations[5] = glGetUniformLocation(shader_program, "uLightmapColor");
        prg->used_lightmap = true;
    } else {
        prg->used_lightmap = false;
    }

    if (world_geometry) {
        prg->uniform_locations[6] = glGetUniformLocation(shader_program, "uShaderFlags");
        prg->uniform_locations[7] = glGetUniformLocation(shader_program, "uShaderFlagValues");
        prg->world_geometry = true;
    } else {
        prg->world_geometry = false;
    }

    prg->uniform_locations[8] = glGetUniformLocation(shader_program, "uFilter");

    gfx_opengl_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_opengl_lookup_shader(struct ColorCombiner* cc) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].hash == cc->hash) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static GLuint gfx_opengl_new_texture(void) {
    if (num_textures >= tex_cache_size) {
        tex_cache_size += TEX_CACHE_STEP;
        tex_cache = realloc(tex_cache, sizeof(struct GLTexture) * tex_cache_size);
        if (!tex_cache) sys_fatal("out of memory allocating texture cache");
        // invalidate these because they might be pointing to garbage now
        opengl_tex[0] = NULL;
        opengl_tex[1] = NULL;
        gfx_texture_state_invalidate();
    }
    glGenTextures(1, &tex_cache[num_textures].gltex);
    return num_textures++;
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id) {
    struct GLTexture *tex = tex_cache + texture_id;
    if (opengl_tex[tile] == tex) {
        // Already bound to this unit -- the uniforms are already consistent
        // with it too, since a shader change re-pushes them for whatever's
        // currently in opengl_tex[tile] (see gfx_opengl_load_shader). Skipping
        // the redundant glActiveTexture/glBindTexture pair matters on tile-based
        // mobile GPUs (Mali), where a rebind can flush pending FBO work early.
        opengl_curtex = tile;
        PROFILE_ADD(texBindSkips, 1);
        return;
    }
    opengl_tex[tile] = tex;
    opengl_curtex = tile;
    PROFILE_ADD(texBinds, 1);
    glActiveTexture(GL_TEXTURE0 + tile);
    glBindTexture(GL_TEXTURE_2D, tex->gltex);
    gfx_opengl_set_texture_uniforms(opengl_prg, tile);
}

static void gfx_opengl_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    CTX_BEGIN_TIMED(CTX_TEXUPLOAD);
#if defined(HANDHELD) && defined(USE_GLES)
    // The G31 is bandwidth-starved, and N64 texture data never carried more
    // than 5-6 bits of colour precision per channel to begin with (see the
    // SCALE_x_8 upscales in gfx_pc.c's import_texture_* functions), so
    // repacking to RGBA5551 here roughly halves upload bandwidth and texture
    // memory footprint at no visible cost. Bounded by the largest rgba32_buf
    // any import_texture_* in gfx_pc.c can produce (0x8000 bytes = 8192 texels).
    size_t num_pixels = (size_t)width * (size_t)height;
    uint16_t buf16[8192];
    if (num_pixels <= sizeof(buf16) / sizeof(buf16[0])) {
        for (size_t i = 0; i < num_pixels; i++) {
            uint8_t r = rgba32_buf[4 * i + 0];
            uint8_t g = rgba32_buf[4 * i + 1];
            uint8_t b = rgba32_buf[4 * i + 2];
            uint8_t a = rgba32_buf[4 * i + 3];
            buf16[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7));
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, buf16);
        opengl_tex[opengl_curtex]->size[0] = width;
        opengl_tex[opengl_curtex]->size[1] = height;
        PROFILE_ADD(texBytes, num_pixels * 2);
        CTX_END_TIMED(CTX_TEXUPLOAD);
        return;
    }
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    opengl_tex[opengl_curtex]->size[0] = width;
    opengl_tex[opengl_curtex]->size[1] = height;
    PROFILE_ADD(texBytes, (size_t)width * (size_t)height * 4);
    CTX_END_TIMED(CTX_TEXUPLOAD);
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const GLenum filter = linear_filter ? GL_LINEAR : GL_NEAREST;
    glActiveTexture(GL_TEXTURE0 + tile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
    opengl_curtex = tile;
    if (opengl_tex[tile]) {
        opengl_tex[tile]->filter = linear_filter;
        gfx_opengl_set_texture_uniforms(opengl_prg, tile);
    }
}

static void gfx_opengl_set_depth_test(bool depth_test) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_opengl_set_depth_mask(bool z_upd) {
    glDepthMask(z_upd ? GL_TRUE : GL_FALSE);
}

static void gfx_opengl_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        glPolygonOffset(-2, -2);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

#ifdef HANDHELD
// rdp.viewport/scissor are computed in window-space (gfx_current_dimensions,
// i.e. the panel's native 1024x768); rescale into the smaller FBO we actually
// render into. Both axes use the same source/target aspect, so this can't distort.
static inline void gfx_opengl_handheld_rescale(int *x, int *y, int *width, int *height) {
    if (!sHandheldWorldPassActive) return;
    *x = (*x * (int)sHandheldFboW) / (int)sHandheldFboWindowW;
    *y = (*y * (int)sHandheldFboH) / (int)sHandheldFboWindowH;
    *width = (*width * (int)sHandheldFboW) / (int)sHandheldFboWindowW;
    *height = (*height * (int)sHandheldFboH) / (int)sHandheldFboWindowH;
}
#endif

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
#ifdef HANDHELD
    gfx_opengl_handheld_rescale(&x, &y, &width, &height);
#endif
    glViewport(x, y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
#ifdef HANDHELD
    gfx_opengl_handheld_rescale(&x, &y, &width, &height);
#endif
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    //printf("flushing %d tris\n", buf_vbo_num_tris);
    const GLsizeiptr size = (GLsizeiptr)(sizeof(float) * buf_vbo_len);
    // Explicitly orphan the previous store before uploading new data: this
    // tells the driver it can hand back a fresh allocation immediately instead
    // of stalling the CPU until the GPU is done reading the old contents from
    // the last draw. Some mobile drivers (Mali included) don't reliably infer
    // this from a same-size glBufferData(..., data, ...) call on its own.
    glBufferData(GL_ARRAY_BUFFER, size, NULL, GL_STREAM_DRAW);
    glBufferData(GL_ARRAY_BUFFER, size, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

static inline bool gl_version_is_supported(int major, int minor, bool is_es) {
    if (is_es) {
        return major >= 2;
    }
    return (major > 2) || (major == 2 && minor >= 1);
}

static inline bool gl_get_version(int *major, int *minor, bool *is_es) {
    const char *vstr = (const char *)glGetString(GL_VERSION);
    if (!vstr || !vstr[0]) return false;

    if (!strncmp(vstr, "OpenGL ES ", 10)) {
        vstr += 10;
        *is_es = true;
    } else if (!strncmp(vstr, "OpenGL ES-CM ", 13)) {
        vstr += 13;
        *is_es = true;
    }

    return (sscanf(vstr, "%d.%d", major, minor) == 2);
}

static void gfx_opengl_init(void) {
#if FOR_WINDOWS || defined(OSX_BUILD)
    GLenum err;
    if ((err = glewInit()) != GLEW_OK)
        sys_fatal("could not init GLEW:\n%s", glewGetErrorString(err));
#endif

    tex_cache_size = TEX_CACHE_STEP;
    tex_cache = calloc(tex_cache_size, sizeof(struct GLTexture));
    if (!tex_cache) sys_fatal("out of memory allocating texture cache");

    // check GL version
    int vmajor = 0;
    int vminor = 0;
    bool is_es = false;
    if (!gl_get_version(&vmajor, &vminor, &is_es) || !gl_version_is_supported(vmajor, vminor, is_es)) {
        sys_fatal("OpenGL 2.1+ is required.\nReported version: %s%d.%d", is_es ? "ES" : "", vmajor, vminor);
    }

    // A fresh context has every attribute array disabled and no pointer state,
    // so the mirror has to start from that and not from whatever a previous
    // context left behind.
    sEnabledAttribs = 0;
    memset(sAttribLayout, 0, sizeof(sAttribLayout));

    glGenBuffers(1, &opengl_vbo);

    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);

    if (vmajor >= 3 && !is_es) {
        glGenVertexArrays(1, &opengl_vao);
        glBindVertexArray(opengl_vao);
    }

    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#if defined(HANDHELD) && defined(USE_GLES)
    sHandheldDiscardFramebufferEXT =
        (PFNGLDISCARDFRAMEBUFFEREXTPROC) SDL_GL_GetProcAddress("glDiscardFramebufferEXT");
#endif
}

bool gfx_opengl_check_compatibility(void) {
    // check GL version
    int vmajor = 0;
    int vminor = 0;
    bool is_es = false;
    if (!gl_get_version(&vmajor, &vminor, &is_es)) {
        return false;
    }
    return gl_version_is_supported(vmajor, vminor, is_es);
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame(void) {
    frame_count++;

#ifdef HANDHELD
    gfx_opengl_handheld_ensure_fbo(gfx_current_dimensions.width, gfx_current_dimensions.height);
    sHandheldWorldPassActive = sHandheldFboReady;
    glBindFramebuffer(GL_FRAMEBUFFER, sHandheldFboReady ? sHandheldFbo : 0);
    if (sHandheldFboReady) {
        // gfx_pc.c only reissues set_viewport/set_scissor when the *logical*
        // (window-space) rdp values change, which usually doesn't happen
        // frame-to-frame. The physical GL viewport/scissor rect is leftover
        // state from last frame's full-window HUD pass, so it must be reset
        // to the FBO's own extent explicitly here -- otherwise only the
        // bottom-left slice of the scene lands inside the (much smaller) FBO.
        glViewport(0, 0, sHandheldFboW, sHandheldFboH);
        glScissor(0, 0, sHandheldFboW, sHandheldFboH);
    }
#endif

    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE); // Must be set to clear Z-buffer
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

static void gfx_opengl_end_frame(void) {
}

#ifdef HANDHELD
// Ends the low-res world pass: blits the FBO upscaled onto the window, then
// leaves rendering targeting the window directly at native resolution for
// whatever draws next (normally the HUD, via G_HANDHELD_HUD_PASS_EXT). Also
// called as a fallback from gfx_opengl_finish_render() in case a frame never
// hits that marker (e.g. loading/crash screens), so the FBO always makes it
// to the window before swap either way.
void gfx_opengl_handheld_end_world_pass(void) {
    if (!sHandheldWorldPassActive) return;
    sHandheldWorldPassActive = false;

    // gfx_pc.c caches GL state (bound program+attribs, blend/depth enables)
    // and only reissues calls when it thinks something changed, so anything
    // the blit touches must be restored to match what it believes is current.
    GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);

#ifdef USE_GLES
    if (sHandheldDiscardFramebufferEXT) {
        // The FBO's depth buffer is cleared fresh every frame (start_frame)
        // and never sampled after this point, so tell the driver it doesn't
        // need to flush it back to system RAM at the end of the tile pass --
        // pure bandwidth saved on Mali's tile-based renderer. Must happen
        // while sHandheldFbo is still bound, before it's unbound below.
        const GLenum attachments[1] = { GL_DEPTH_ATTACHMENT };
        sHandheldDiscardFramebufferEXT(GL_FRAMEBUFFER, 1, attachments);
    }
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei) sHandheldFboWindowW, (GLsizei) sHandheldFboWindowH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(sHandheldBlitProgram);
    glUniform2f(sHandheldBlitSrcSizeLoc, (float) sHandheldFboW, (float) sHandheldFboH);
    glUniform2f(sHandheldBlitScaleLoc,
                (float) sHandheldFboWindowW / (float) sHandheldFboW,
                (float) sHandheldFboWindowH / (float) sHandheldFboH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sHandheldColorTex);
    // This bind bypasses select_texture()'s tile bookkeeping, so whatever it
    // thinks is bound to unit 0 is now wrong -- force the next select_texture(0, ...)
    // to actually rebind instead of trusting its (now stale) cache.
    opengl_tex[0] = NULL;
    gfx_texture_state_invalidate();

    static const float kBlitQuad[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kBlitQuad), kBlitQuad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(sHandheldBlitPosLoc);
    glVertexAttribPointer(sHandheldBlitPosLoc, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *) 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(sHandheldBlitPosLoc);
    // The blit drives the attribute arrays directly, so the mirror is now stale
    // for this index: the array is off, and its pointer describes the blit quad
    // rather than whatever the renderer last set.
    gfx_opengl_attrib_state_invalidate(sHandheldBlitPosLoc);

    if (opengl_prg) {
        glUseProgram(opengl_prg->opengl_program_id);
        gfx_opengl_vertex_array_set_attribs(opengl_prg);
    }
    if (blend_was_enabled) glEnable(GL_BLEND);
    if (depth_was_enabled) glEnable(GL_DEPTH_TEST);
    // The old scissor rect is still in FBO-space; reset it to the full window
    // since rendering_state's cache may not think it needs to reissue one.
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei) sHandheldFboWindowW, (GLsizei) sHandheldFboWindowH);
}
#endif

static void gfx_opengl_finish_render(void) {
#ifdef HANDHELD
    gfx_opengl_handheld_end_world_pass();
#ifdef USE_GLES
    if (sHandheldDiscardFramebufferEXT) {
        // Nothing reads the window's own depth/stencil back: the 3D pass
        // renders into the low-res FBO above, not the window itself, the
        // blit and HUD both draw with depth testing off, and depth is
        // cleared fresh every frame anyway. Discard them here, right before
        // the next swap, so the driver skips flushing them to system RAM.
        const GLenum attachments[2] = { GL_DEPTH_EXT, GL_STENCIL_EXT };
        sHandheldDiscardFramebufferEXT(GL_FRAMEBUFFER, 2, attachments);
    }
#endif
#endif
}

static const char* gfx_opengl_get_name(void) {
    return "OpenGL";
}

static void gfx_opengl_shutdown(void) {
#ifdef HANDHELD
    gfx_opengl_handheld_destroy_fbo();
    if (sHandheldBlitProgram) { glDeleteProgram(sHandheldBlitProgram); sHandheldBlitProgram = 0; }
#endif
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_z_is_from_0_to_1,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_test,
    gfx_opengl_set_depth_mask,
    gfx_opengl_set_zmode_decal,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render,
    gfx_opengl_get_name,
    gfx_opengl_shutdown
};
