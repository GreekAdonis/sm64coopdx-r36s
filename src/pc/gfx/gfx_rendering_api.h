#ifndef GFX_RENDERING_API_H
#define GFX_RENDERING_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct ShaderProgram;
struct ColorCombiner;

struct GfxRenderingAPI {
    bool (*z_is_from_0_to_1)(void);
    void (*unload_shader)(struct ShaderProgram *old_prg);
    void (*load_shader)(struct ShaderProgram *new_prg);
    struct ShaderProgram *(*create_and_load_new_shader)(struct ColorCombiner* cc);
    struct ShaderProgram *(*lookup_shader)(struct ColorCombiner* cc);
    void (*shader_get_info)(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]);
    uint32_t (*new_texture)(void);
    void (*select_texture)(int tile, uint32_t texture_id);
    void (*upload_texture)(const uint8_t *rgba32_buf, int width, int height);
    void (*set_sampler_parameters)(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt);
    void (*set_depth_test)(bool depth_test);
    void (*set_depth_mask)(bool z_upd);
    void (*set_zmode_decal)(bool zmode_decal);
    void (*set_viewport)(int x, int y, int width, int height);
    void (*set_scissor)(int x, int y, int width, int height);
    void (*set_use_alpha)(bool use_alpha);
    void (*draw_triangles)(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
    void (*init)(void);
    void (*on_resize)(void);
    void (*start_frame)(void);
    void (*end_frame)(void);
    void (*finish_render)(void);
    const char* (*get_name)(void);
    void (*shutdown)(void);

    // Gives a shader program back to the backend, which frees whatever it owns
    // and marks its pool slot reusable. Called when the colour combiner that
    // owns the program is evicted, so that a program's lifetime is exactly its
    // combiner's -- see gfx_lookup_or_create_color_combiner() in gfx_pc.c.
    //
    // Optional. Backends that leave it NULL simply never reclaim, which is what
    // every backend did before this existed. Deliberately last in the struct:
    // the initialisers here are positional, and two of them already stop short
    // of the end.
    void (*release_shader)(struct ShaderProgram *prg);
};

#endif
