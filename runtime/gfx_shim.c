/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

struct twd_gfx_device {
    int32_t refs;
    int backend;
    void *native_window;
    void *backend_lib;
    int driver_caps;
    uint32_t gpu_count;
    void *vk_instance;
    void (*vk_destroy_instance)(void *instance, const void *alloc);
    void *egl_display;
    void *egl_surface;
    void *egl_context;
    unsigned int (*egl_swap_buffers)(void *dpy, void *surf);
    unsigned int (*egl_destroy_surface)(void *dpy, void *surf);
    unsigned int (*egl_destroy_context)(void *dpy, void *ctx);
    unsigned int (*egl_terminate)(void *dpy);
    void *gl_lib;
    void (*gl_viewport)(int32_t x, int32_t y, int32_t w, int32_t h);
    void (*gl_scissor)(int32_t x, int32_t y, int32_t w, int32_t h);
    void (*gl_clear_color)(float r, float g, float b, float a);
    void (*gl_clear)(uint32_t mask);
    void (*gl_draw_arrays)(uint32_t mode, int32_t first, int32_t count);
    void (*gl_draw_elements)(uint32_t mode, int32_t count, uint32_t type, const void *indices);
    void (*gl_gen_buffers)(int32_t n, uint32_t *buffers);
    void (*gl_delete_buffers)(int32_t n, const uint32_t *buffers);
    void (*gl_bind_buffer)(uint32_t target, uint32_t buffer);
    void (*gl_buffer_data)(uint32_t target, ptrdiff_t size, const void *data, uint32_t usage);
    void (*gl_buffer_sub_data)(uint32_t target, ptrdiff_t offset, ptrdiff_t size, const void *data);
    void (*gl_gen_textures)(int32_t n, uint32_t *textures);
    void (*gl_delete_textures)(int32_t n, const uint32_t *textures);
    void (*gl_active_texture)(uint32_t texture);
    void (*gl_bind_texture)(uint32_t target, uint32_t texture);
    void (*gl_tex_image_2d)(uint32_t target, int32_t level, int32_t internalformat,
                            int32_t width, int32_t height, int32_t border,
                            uint32_t format, uint32_t type, const void *pixels);
    void (*gl_tex_sub_image_2d)(uint32_t target, int32_t level, int32_t xoff, int32_t yoff,
                                int32_t width, int32_t height, uint32_t format,
                                uint32_t type, const void *pixels);
    void (*gl_tex_parameter_i)(uint32_t target, uint32_t pname, int32_t param);
    uint32_t (*gl_create_shader)(uint32_t type);
    void (*gl_shader_source)(uint32_t shader, int32_t count, const char *const *str, const int32_t *len);
    void (*gl_compile_shader)(uint32_t shader);
    void (*gl_delete_shader)(uint32_t shader);
    uint32_t (*gl_create_program)(void);
    void (*gl_attach_shader)(uint32_t program, uint32_t shader);
    void (*gl_link_program)(uint32_t program);
    void (*gl_use_program)(uint32_t program);
    void (*gl_delete_program)(uint32_t program);
    uint32_t gl_active_program;
    uint32_t width;
    uint32_t height;
    int vsync;
    uint64_t frame_index;
    uint64_t draw_calls;
    uint64_t clear_calls;
    uint64_t map_calls;
    uint64_t update_calls;
    uint32_t active_buffers;
    uint32_t active_textures;
    uint32_t topology;
    uint32_t bound_rtv_count;
    void *bound_vs;
    void *bound_ps;
    void *bound_input_layout;
    void *bound_vb;
    void *bound_ib;
    uint64_t bound_vs_hash;
    uint64_t bound_ps_hash;
    uint64_t linked_vs_hash;
    uint64_t linked_ps_hash;
    uint32_t bound_vb_gl;
    uint32_t bound_ib_gl;
    void *bound_rtvs[8];
    void *bound_dsv;
    void *bound_ps_srvs[8];
    uint32_t bound_ps_srv_gl[8];
    void *bound_vs_cbs[8];
    void *bound_ps_cbs[8];
    twd_d3d11_rect_t scissor;
    twd_shader_cache_t *shader_cache;
    uint8_t *backbuffer;
    size_t backbuffer_size;
    float last_clear_rgba[4];
    float last_depth;
    uint8_t last_stencil;
    twd_d3d11_viewport_t viewport;
};

struct twd_gfx_buffer {
    twd_gfx_device_t *dev;
    uint32_t kind;
    uint32_t gl_handle;
    size_t byte_size;
    void *staging;
};

struct twd_gfx_texture {
    twd_gfx_device_t *dev;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t row_pitch;
    uint32_t gl_handle;
    size_t byte_size;
    void *pixels;
};

int twd_gfx_select_backend(int preferred) {
    if (preferred == TWD_GFX_BACKEND_VULKAN ||
        preferred == TWD_GFX_BACKEND_GLES3 ||
        preferred == TWD_GFX_BACKEND_NONE) {
        return preferred;
    }
#ifdef __ANDROID__
    void *vk = twd_dynlib_open("libvulkan.so");
    if (vk) {
        twd_dynlib_close(vk);
        return TWD_GFX_BACKEND_VULKAN;
    }
    return TWD_GFX_BACKEND_GLES3;
#else
    return TWD_GFX_BACKEND_NONE;
#endif
}

twd_gfx_device_t *twd_gfx_device_create(const twd_gfx_desc_t *desc) {
    twd_gfx_device_t *dev = (twd_gfx_device_t *)calloc(1, sizeof(*dev));
    if (!dev) return NULL;
    dev->refs = 1;
    int pref = desc ? desc->preferred_backend : -1;
    dev->backend = twd_gfx_select_backend(pref);
    dev->native_window = desc ? desc->native_window : NULL;
    dev->width = (desc && desc->width > 0u) ? desc->width : 1280u;
    dev->height = (desc && desc->height > 0u) ? desc->height : 720u;
    dev->vsync = desc ? (desc->vsync != 0) : 1;
    dev->viewport.width = (float)dev->width;
    dev->viewport.height = (float)dev->height;
    dev->viewport.max_depth = 1.0f;
    if ((size_t)dev->width <= ((size_t)-1) / 4u / (size_t)dev->height) {
        size_t bb_sz = (size_t)dev->width * (size_t)dev->height * 4u;
        dev->backbuffer = (uint8_t *)calloc(1u, bb_sz ? bb_sz : 4u);
        if (dev->backbuffer) dev->backbuffer_size = bb_sz;
    }
    dev->shader_cache = twd_shader_cache_create();

#ifdef __ANDROID__
    if (dev->backend == TWD_GFX_BACKEND_VULKAN) {
        dev->backend_lib = twd_dynlib_open("libvulkan.so");
        typedef int32_t (*vk_create_inst_fn)(const void *, const void *, void **);
        typedef int32_t (*vk_enum_gpus_fn)(void *, uint32_t *, void *);
        typedef void (*vk_destroy_inst_fn)(void *, const void *);
        vk_create_inst_fn vkCreateInst = dev->backend_lib
            ? (vk_create_inst_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "vkCreateInstance")
            : NULL;
        vk_enum_gpus_fn vkEnumGpus = dev->backend_lib
            ? (vk_enum_gpus_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "vkEnumeratePhysicalDevices")
            : NULL;
        vk_destroy_inst_fn vkDestroyInst = dev->backend_lib
            ? (vk_destroy_inst_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "vkDestroyInstance")
            : NULL;
        if (vkCreateInst && vkEnumGpus && vkDestroyInst) {
            dev->driver_caps |= 1;
            struct {
                uint32_t sType;
                uint32_t _pad0;
                const void *pNext;
                uint32_t flags;
                uint32_t _pad1;
                const void *pApplicationInfo;
                uint32_t enabledLayerCount;
                uint32_t _pad2;
                const char *const *ppEnabledLayerNames;
                uint32_t enabledExtensionCount;
                uint32_t _pad3;
                const char *const *ppEnabledExtensionNames;
            } vk_ci;
            memset(&vk_ci, 0, sizeof(vk_ci));
            vk_ci.sType = 1u; /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */
            void *inst = NULL;
            if (vkCreateInst(&vk_ci, NULL, &inst) == 0 && inst) {
                uint32_t gpus = 0u;
                vkEnumGpus(inst, &gpus, NULL);
                dev->gpu_count = gpus;
                dev->vk_instance = inst;
                dev->vk_destroy_instance = vkDestroyInst;
            }
        } else {
            if (dev->backend_lib) {
                twd_dynlib_close(dev->backend_lib);
                dev->backend_lib = NULL;
            }
            dev->backend = TWD_GFX_BACKEND_GLES3;
        }
    }
    if (dev->backend == TWD_GFX_BACKEND_GLES3 && !dev->backend_lib) {
        dev->backend_lib = twd_dynlib_open("libEGL.so");
        typedef void *(*egl_get_dpy_fn)(void *);
        typedef unsigned int (*egl_init_fn)(void *, int32_t *, int32_t *);
        typedef unsigned int (*egl_choose_fn)(void *, const int32_t *, void **, int32_t, int32_t *);
        typedef void *(*egl_create_surf_fn)(void *, void *, void *, const int32_t *);
        typedef void *(*egl_create_ctx_fn)(void *, void *, void *, const int32_t *);
        typedef unsigned int (*egl_make_cur_fn)(void *, void *, void *, void *);
        typedef unsigned int (*egl_swap_fn)(void *, void *);
        typedef unsigned int (*egl_destroy_fn)(void *, void *);
        typedef unsigned int (*egl_term_fn)(void *);
        egl_get_dpy_fn getDpy = dev->backend_lib
            ? (egl_get_dpy_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglGetDisplay")
            : NULL;
        egl_init_fn eglInit = dev->backend_lib
            ? (egl_init_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglInitialize")
            : NULL;
        egl_swap_fn eglSwap = dev->backend_lib
            ? (egl_swap_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglSwapBuffers")
            : NULL;
        if (getDpy && eglInit && eglSwap) {
            dev->driver_caps |= 2;
            void *dpy = getDpy(NULL);
            if (dpy && eglInit(dpy, NULL, NULL)) {
                dev->egl_display = dpy;
                dev->egl_swap_buffers = eglSwap;
                dev->egl_destroy_surface = (egl_destroy_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglDestroySurface");
                dev->egl_destroy_context = (egl_destroy_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglDestroyContext");
                dev->egl_terminate = (egl_term_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglTerminate");
                dev->gpu_count = 1u;
                egl_choose_fn chooseCfg = (egl_choose_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglChooseConfig");
                egl_create_surf_fn mkSurf = (egl_create_surf_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglCreateWindowSurface");
                egl_create_ctx_fn mkCtx = (egl_create_ctx_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglCreateContext");
                egl_make_cur_fn mkCur = (egl_make_cur_fn)(uintptr_t)twd_dynlib_sym(dev->backend_lib, "eglMakeCurrent");
                if (dev->native_window && chooseCfg && mkSurf && mkCtx && mkCur) {
                    static const int32_t cfg_attrs[] = {
                        0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3040, 0x40, 0x3038
                    };
                    static const int32_t ctx_attrs[] = { 0x3098, 3, 0x3038 };
                    void *cfg = NULL;
                    int32_t ncfg = 0;
                    if (chooseCfg(dpy, cfg_attrs, &cfg, 1, &ncfg) && ncfg > 0 && cfg) {
                        void *surf = mkSurf(dpy, cfg, dev->native_window, NULL);
                        void *ctx = mkCtx(dpy, cfg, NULL, ctx_attrs);
                        if (surf && ctx && mkCur(dpy, surf, surf, ctx)) {
                            dev->egl_surface = surf;
                            dev->egl_context = ctx;
                        }
                    }
                }
            }
        }
        dev->gl_lib = twd_dynlib_open("libGLESv3.so");
        if (!dev->gl_lib) dev->gl_lib = twd_dynlib_open("libGLESv2.so");
        if (dev->gl_lib) {
            typedef void (*gl_vp_fn)(int32_t, int32_t, int32_t, int32_t);
            typedef void (*gl_cc_fn)(float, float, float, float);
            typedef void (*gl_clr_fn)(uint32_t);
            typedef void (*gl_da_fn)(uint32_t, int32_t, int32_t);
            typedef void (*gl_de_fn)(uint32_t, int32_t, uint32_t, const void *);
            dev->gl_viewport = (gl_vp_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glViewport");
            dev->gl_scissor = (gl_vp_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glScissor");
            dev->gl_clear_color = (gl_cc_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glClearColor");
            dev->gl_clear = (gl_clr_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glClear");
            dev->gl_draw_arrays = (gl_da_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glDrawArrays");
            dev->gl_draw_elements = (gl_de_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glDrawElements");
            if (dev->gl_clear_color && dev->gl_clear && dev->gl_draw_arrays) {
                dev->driver_caps |= 4;
            }
            typedef void (*gl_gen_fn)(int32_t, uint32_t *);
            typedef void (*gl_del_fn)(int32_t, const uint32_t *);
            typedef void (*gl_bind_fn)(uint32_t, uint32_t);
            typedef void (*gl_bdata_fn)(uint32_t, ptrdiff_t, const void *, uint32_t);
            typedef void (*gl_bsub_fn)(uint32_t, ptrdiff_t, ptrdiff_t, const void *);
            typedef void (*gl_tex2d_fn)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *);
            typedef void (*gl_texsub_fn)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *);
            typedef void (*gl_texparam_fn)(uint32_t, uint32_t, int32_t);
            typedef uint32_t (*gl_mk_sh_fn)(uint32_t);
            typedef void (*gl_sh_src_fn)(uint32_t, int32_t, const char *const *, const int32_t *);
            typedef void (*gl_u32_fn)(uint32_t);
            typedef uint32_t (*gl_mk_prog_fn)(void);
            typedef void (*gl_attach_fn)(uint32_t, uint32_t);

            dev->gl_gen_buffers = (gl_gen_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glGenBuffers");
            dev->gl_delete_buffers = (gl_del_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glDeleteBuffers");
            dev->gl_bind_buffer = (gl_bind_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glBindBuffer");
            dev->gl_buffer_data = (gl_bdata_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glBufferData");
            dev->gl_buffer_sub_data = (gl_bsub_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glBufferSubData");
            dev->gl_gen_textures = (gl_gen_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glGenTextures");
            dev->gl_delete_textures = (gl_del_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glDeleteTextures");
            dev->gl_active_texture = (gl_u32_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glActiveTexture");
            dev->gl_bind_texture = (gl_bind_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glBindTexture");
            dev->gl_tex_image_2d = (gl_tex2d_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glTexImage2D");
            dev->gl_tex_sub_image_2d = (gl_texsub_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glTexSubImage2D");
            dev->gl_tex_parameter_i = (gl_texparam_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glTexParameteri");
            dev->gl_create_shader = (gl_mk_sh_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glCreateShader");
            dev->gl_shader_source = (gl_sh_src_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glShaderSource");
            dev->gl_compile_shader = (gl_u32_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glCompileShader");
            dev->gl_delete_shader = (gl_u32_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glDeleteShader");
            dev->gl_create_program = (gl_mk_prog_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glCreateProgram");
            dev->gl_attach_shader = (gl_attach_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glAttachShader");
            dev->gl_link_program = (gl_u32_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glLinkProgram");
            dev->gl_use_program = (gl_u32_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glUseProgram");
            dev->gl_delete_program = (gl_u32_fn)(uintptr_t)twd_dynlib_sym(dev->gl_lib, "glDeleteProgram");
            if (dev->gl_gen_buffers && dev->gl_buffer_data && dev->gl_gen_textures &&
                dev->gl_tex_image_2d && dev->gl_create_shader && dev->gl_create_program) {
                dev->driver_caps |= 8;
            }
        }
    }
#endif
    return dev;
}

uint32_t twd_gfx_gpu_count(const twd_gfx_device_t *dev) {
    return dev ? dev->gpu_count : 0u;
}

int twd_gfx_has_gl_dispatch(const twd_gfx_device_t *dev) {
    if (!dev || !(dev->driver_caps & 4)) return 0;
    return (dev->driver_caps & 8) ? 2 : 1;
}

uint32_t twd_gfx_gl_compile_glsl(twd_gfx_device_t *dev, uint32_t stage, const char *glsl_src) {
    if (!dev || !glsl_src || !dev->egl_surface || !dev->gl_create_shader ||
        !dev->gl_shader_source || !dev->gl_compile_shader) {
        return 0u;
    }
    uint32_t gl_type = (stage == TWD_SHADER_STAGE_VS) ? 0x8B31u : 0x8B30u; /* GL_VERTEX_SHADER / GL_FRAGMENT_SHADER */
    uint32_t sh = dev->gl_create_shader(gl_type);
    if (!sh) return 0u;
    dev->gl_shader_source(sh, 1, &glsl_src, NULL);
    dev->gl_compile_shader(sh);
    return sh;
}

int twd_gfx_has_driver_symbols(const twd_gfx_device_t *dev) {
    return dev ? dev->driver_caps : 0;
}

int twd_gfx_probe_driver(const twd_gfx_device_t *dev, char *out_name, size_t cap) {
    const char *name = "Headless";
    int backend = dev ? dev->backend : twd_gfx_select_backend(-1);
    if (backend == TWD_GFX_BACKEND_VULKAN) name = "Vulkan";
    else if (backend == TWD_GFX_BACKEND_GLES3) name = "OpenGL ES 3";
    if (out_name && cap > 0u) {
        size_t n = strlen(name);
        if (n >= cap) n = cap - 1u;
        memcpy(out_name, name, n);
        out_name[n] = '\0';
    }
    return backend;
}

void twd_gfx_device_destroy(twd_gfx_device_t *dev) {
    if (!dev) return;
    if (--dev->refs > 0) return;
    twd_shader_cache_destroy(dev->shader_cache);
    dev->shader_cache = NULL;
    if (dev->gl_active_program && dev->gl_delete_program) {
        dev->gl_delete_program(dev->gl_active_program);
        dev->gl_active_program = 0u;
    }
    if (dev->egl_display) {
        if (dev->egl_surface && dev->egl_destroy_surface) {
            dev->egl_destroy_surface(dev->egl_display, dev->egl_surface);
        }
        if (dev->egl_context && dev->egl_destroy_context) {
            dev->egl_destroy_context(dev->egl_display, dev->egl_context);
        }
        if (dev->egl_terminate) {
            dev->egl_terminate(dev->egl_display);
        }
        dev->egl_display = NULL;
    }
    if (dev->vk_instance && dev->vk_destroy_instance) {
        dev->vk_destroy_instance(dev->vk_instance, NULL);
        dev->vk_instance = NULL;
    }
    if (dev->gl_lib) {
        twd_dynlib_close(dev->gl_lib);
        dev->gl_lib = NULL;
    }
    if (dev->backend_lib) {
        twd_dynlib_close(dev->backend_lib);
        dev->backend_lib = NULL;
    }
    free(dev->backbuffer);
    dev->backbuffer = NULL;
    dev->backbuffer_size = 0u;
    free(dev);
}

int twd_gfx_resize(twd_gfx_device_t *dev, uint32_t width, uint32_t height) {
    if (!dev || width == 0u || height == 0u) return -1;
    if ((size_t)width <= ((size_t)-1) / 4u / (size_t)height) {
        size_t new_sz = (size_t)width * (size_t)height * 4u;
        if (new_sz != dev->backbuffer_size) {
            uint8_t *nb = (uint8_t *)calloc(1u, new_sz ? new_sz : 4u);
            if (nb) {
                free(dev->backbuffer);
                dev->backbuffer = nb;
                dev->backbuffer_size = new_sz;
            }
        }
    }
    dev->width = width;
    dev->height = height;
    return 0;
}

void twd_gfx_get_stats(const twd_gfx_device_t *dev, twd_gfx_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!dev) return;
    out->backend = dev->backend;
    out->width = dev->width;
    out->height = dev->height;
    out->frame_index = dev->frame_index;
    out->draw_calls = dev->draw_calls;
    out->clear_calls = dev->clear_calls;
    out->map_calls = dev->map_calls;
    out->update_calls = dev->update_calls;
    out->active_buffers = dev->active_buffers;
    out->active_textures = dev->active_textures;
    out->topology = dev->topology;
    out->bound_rtv_count = dev->bound_rtv_count;
    out->scissor_right = dev->scissor.right;
    out->scissor_bottom = dev->scissor.bottom;
    out->cached_shaders = twd_shader_cache_count(dev->shader_cache);
    twd_shader_cache_stats(dev->shader_cache, &out->shader_cache_hits, &out->shader_cache_misses);
    memcpy(out->last_clear_rgba, dev->last_clear_rgba, sizeof(out->last_clear_rgba));
}

twd_shader_cache_t *twd_gfx_get_shader_cache(twd_gfx_device_t *dev) {
    return dev ? dev->shader_cache : NULL;
}

static uint32_t buf_kind_to_gl_target(uint32_t kind) {
    if (kind == TWD_GFX_BUF_INDEX) return 0x8893u;    /* GL_ELEMENT_ARRAY_BUFFER */
    if (kind == TWD_GFX_BUF_CONSTANT) return 0x8A11u; /* GL_UNIFORM_BUFFER */
    return 0x8892u;                                   /* GL_ARRAY_BUFFER */
}

static void sync_gl_buffer(twd_gfx_buffer_t *buf) {
    if (!buf || !buf->dev || !buf->dev->egl_surface || !buf->dev->gl_bind_buffer || !buf->dev->gl_buffer_sub_data) {
        return;
    }
    if (!buf->gl_handle) return;
    uint32_t tgt = buf_kind_to_gl_target(buf->kind);
    buf->dev->gl_bind_buffer(tgt, buf->gl_handle);
    buf->dev->gl_buffer_sub_data(tgt, 0, (ptrdiff_t)buf->byte_size, buf->staging);
}

static void sync_gl_texture(twd_gfx_texture_t *tex) {
    if (!tex || !tex->dev || !tex->dev->egl_surface || !tex->dev->gl_bind_texture || !tex->dev->gl_tex_sub_image_2d) {
        return;
    }
    if (!tex->gl_handle || !tex->pixels) return;
    tex->dev->gl_bind_texture(0x0DE1u, tex->gl_handle); /* GL_TEXTURE_2D */
    tex->dev->gl_tex_sub_image_2d(0x0DE1u, 0, 0, 0, (int32_t)tex->width, (int32_t)tex->height,
                                  0x1908u, 0x1401u, tex->pixels); /* GL_RGBA, GL_UNSIGNED_BYTE */
}

twd_gfx_buffer_t *twd_gfx_buffer_create(
    twd_gfx_device_t *dev, uint32_t kind, size_t byte_size, const void *initial_data) {
    if (!dev || byte_size == 0u) return NULL;
    twd_gfx_buffer_t *buf = (twd_gfx_buffer_t *)calloc(1, sizeof(*buf));
    if (!buf) return NULL;
    buf->staging = malloc(byte_size);
    if (!buf->staging) {
        free(buf);
        return NULL;
    }
    if (initial_data) {
        memcpy(buf->staging, initial_data, byte_size);
    } else {
        memset(buf->staging, 0, byte_size);
    }
    buf->dev = dev;
    buf->kind = kind;
    buf->byte_size = byte_size;
    if (dev->egl_surface && dev->gl_gen_buffers && dev->gl_bind_buffer && dev->gl_buffer_data) {
        uint32_t tgt = buf_kind_to_gl_target(kind);
        dev->gl_gen_buffers(1, &buf->gl_handle);
        if (buf->gl_handle) {
            dev->gl_bind_buffer(tgt, buf->gl_handle);
            dev->gl_buffer_data(tgt, (ptrdiff_t)byte_size, buf->staging, 0x88E8u); /* GL_DYNAMIC_DRAW */
        }
    }
    dev->active_buffers++;
    return buf;
}

int twd_gfx_buffer_update(twd_gfx_buffer_t *buf, const void *data, size_t byte_size) {
    if (!buf || !data || byte_size > buf->byte_size) return -1;
    memcpy(buf->staging, data, byte_size);
    sync_gl_buffer(buf);
    return 0;
}

const void *twd_gfx_buffer_data(const twd_gfx_buffer_t *buf, size_t *out_size) {
    if (!buf) {
        if (out_size) *out_size = 0u;
        return NULL;
    }
    if (out_size) *out_size = buf->byte_size;
    return buf->staging;
}

void twd_gfx_buffer_destroy(twd_gfx_buffer_t *buf) {
    if (!buf) return;
    if (buf->gl_handle && buf->dev && buf->dev->gl_delete_buffers) {
        buf->dev->gl_delete_buffers(1, &buf->gl_handle);
        buf->gl_handle = 0u;
    }
    if (buf->dev && buf->dev->active_buffers > 0u) {
        buf->dev->active_buffers--;
    }
    free(buf->staging);
    free(buf);
}

twd_gfx_texture_t *twd_gfx_texture_create(
    twd_gfx_device_t *dev, uint32_t width, uint32_t height, uint32_t format) {
    if (!dev || width == 0u || height == 0u) return NULL;
    if ((size_t)width > ((size_t)-1) / 4u / (size_t)height) return NULL;
    twd_gfx_texture_t *tex = (twd_gfx_texture_t *)calloc(1, sizeof(*tex));
    if (!tex) return NULL;
    tex->row_pitch = width * 4u;
    tex->byte_size = (size_t)tex->row_pitch * (size_t)height;
    tex->pixels = calloc(1, tex->byte_size ? tex->byte_size : 4u);
    if (!tex->pixels) {
        free(tex);
        return NULL;
    }
    tex->dev = dev;
    tex->width = width;
    tex->height = height;
    tex->format = format;
    if (dev->egl_surface && dev->gl_gen_textures && dev->gl_bind_texture && dev->gl_tex_image_2d) {
        dev->gl_gen_textures(1, &tex->gl_handle);
        if (tex->gl_handle) {
            dev->gl_bind_texture(0x0DE1u, tex->gl_handle);
            if (dev->gl_tex_parameter_i) {
                dev->gl_tex_parameter_i(0x0DE1u, 0x2801u, 0x2601); /* MIN_FILTER = LINEAR */
                dev->gl_tex_parameter_i(0x0DE1u, 0x2800u, 0x2601); /* MAG_FILTER = LINEAR */
            }
            dev->gl_tex_image_2d(0x0DE1u, 0, 0x1908, (int32_t)width, (int32_t)height,
                                 0, 0x1908u, 0x1401u, tex->pixels);
        }
    }
    dev->active_textures++;
    return tex;
}

int twd_gfx_texture_update(twd_gfx_texture_t *tex, const void *pixels, size_t byte_size) {
    if (!tex || !pixels || !tex->pixels || byte_size == 0u) return -1;
    size_t copy_bytes = byte_size;
    if (copy_bytes > tex->byte_size) copy_bytes = tex->byte_size;
    memcpy(tex->pixels, pixels, copy_bytes);
    sync_gl_texture(tex);
    return 0;
}

const void *twd_gfx_texture_data(const twd_gfx_texture_t *tex, size_t *out_size) {
    if (!tex) {
        if (out_size) *out_size = 0u;
        return NULL;
    }
    if (out_size) *out_size = tex->byte_size;
    return tex->pixels;
}

void twd_gfx_texture_get_size(const twd_gfx_texture_t *tex, uint32_t *out_w, uint32_t *out_h) {
    if (out_w) *out_w = tex ? tex->width : 0u;
    if (out_h) *out_h = tex ? tex->height : 0u;
}

void twd_gfx_texture_destroy(twd_gfx_texture_t *tex) {
    if (!tex) return;
    if (tex->gl_handle && tex->dev && tex->dev->gl_delete_textures) {
        tex->dev->gl_delete_textures(1, &tex->gl_handle);
        tex->gl_handle = 0u;
    }
    if (tex->dev && tex->dev->active_textures > 0u) {
        tex->dev->active_textures--;
    }
    free(tex->pixels);
    free(tex);
}

void twd_gfx_begin_frame(twd_gfx_device_t *dev) {
    (void)dev;
}

static uint8_t clamp_f_u8(float v) {
    if (v <= 0.0f) return 0u;
    if (v >= 1.0f) return 255u;
    return (uint8_t)(v * 255.0f + 0.5f);
}

void twd_gfx_clear(twd_gfx_device_t *dev, const float rgba[4], float depth, uint8_t stencil) {
    if (!dev) return;
    if (rgba) {
        memcpy(dev->last_clear_rgba, rgba, sizeof(dev->last_clear_rgba));
        if (dev->backbuffer && dev->backbuffer_size >= (size_t)dev->width * (size_t)dev->height * 4u) {
            uint8_t r = clamp_f_u8(rgba[0]);
            uint8_t g = clamp_f_u8(rgba[1]);
            uint8_t b = clamp_f_u8(rgba[2]);
            uint8_t a = clamp_f_u8(rgba[3]);
            size_t total = (size_t)dev->width * (size_t)dev->height;
            for (size_t i = 0u; i < total; ++i) {
                dev->backbuffer[i * 4u + 0u] = r;
                dev->backbuffer[i * 4u + 1u] = g;
                dev->backbuffer[i * 4u + 2u] = b;
                dev->backbuffer[i * 4u + 3u] = a;
            }
        }
        if (dev->gl_clear_color && dev->gl_clear && dev->egl_surface) {
            dev->gl_clear_color(rgba[0], rgba[1], rgba[2], rgba[3]);
            dev->gl_clear(0x00004000u); /* GL_COLOR_BUFFER_BIT */
        }
    } else if (dev->gl_clear && dev->egl_surface) {
        dev->gl_clear(0x00000100u | 0x00000400u); /* GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT */
    }
    dev->last_depth = depth;
    dev->last_stencil = stencil;
    dev->clear_calls++;
}

const uint8_t *twd_gfx_backbuffer_data(
    const twd_gfx_device_t *dev, uint32_t *out_w, uint32_t *out_h, size_t *out_size) {
    if (!dev || !dev->backbuffer) {
        if (out_w) *out_w = 0u;
        if (out_h) *out_h = 0u;
        if (out_size) *out_size = 0u;
        return NULL;
    }
    if (out_w) *out_w = dev->width;
    if (out_h) *out_h = dev->height;
    if (out_size) *out_size = dev->backbuffer_size;
    return dev->backbuffer;
}

static void blend_pixel_rgba8(uint8_t *dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0u) return;
    if (a == 255u) {
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = 255u;
        return;
    }
    uint32_t inv = 255u - (uint32_t)a;
    dst[0] = (uint8_t)(((uint32_t)r * (uint32_t)a + (uint32_t)dst[0] * inv) / 255u);
    dst[1] = (uint8_t)(((uint32_t)g * (uint32_t)a + (uint32_t)dst[1] * inv) / 255u);
    dst[2] = (uint8_t)(((uint32_t)b * (uint32_t)a + (uint32_t)dst[2] * inv) / 255u);
    uint32_t out_a = (uint32_t)a + ((uint32_t)dst[3] * inv) / 255u;
    dst[3] = (uint8_t)(out_a > 255u ? 255u : out_a);
}

int twd_gfx_draw_rect(
    twd_gfx_device_t *dev, int32_t dx, int32_t dy, int32_t dw, int32_t dh, const uint8_t rgba[4]) {
    if (!dev || !dev->backbuffer || !rgba || dw <= 0 || dh <= 0) return -1;
    int32_t x0 = (dx < 0) ? 0 : dx;
    int32_t y0 = (dy < 0) ? 0 : dy;
    int32_t x1 = dx + dw;
    int32_t y1 = dy + dh;
    if (x1 > (int32_t)dev->width) x1 = (int32_t)dev->width;
    if (y1 > (int32_t)dev->height) y1 = (int32_t)dev->height;
    for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
            uint8_t *p = dev->backbuffer + ((size_t)y * (size_t)dev->width + (size_t)x) * 4u;
            blend_pixel_rgba8(p, rgba[0], rgba[1], rgba[2], rgba[3]);
        }
    }
    dev->draw_calls++;
    return 0;
}

int twd_gfx_draw_sprite(
    twd_gfx_device_t *dev,
    const twd_gfx_texture_t *tex,
    int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    int32_t dx, int32_t dy, int32_t dw, int32_t dh,
    const uint8_t rgba_tint[4]) {
    if (!dev || !dev->backbuffer || !tex || !tex->pixels ||
        tex->width == 0u || tex->height == 0u || dw <= 0 || dh <= 0) {
        return -1;
    }
    if (sw <= 0) sw = (int32_t)tex->width - sx;
    if (sh <= 0) sh = (int32_t)tex->height - sy;
    if (sw <= 0 || sh <= 0) return -1;

    uint8_t tr = rgba_tint ? rgba_tint[0] : 255u;
    uint8_t tg = rgba_tint ? rgba_tint[1] : 255u;
    uint8_t tb = rgba_tint ? rgba_tint[2] : 255u;
    uint8_t ta = rgba_tint ? rgba_tint[3] : 255u;
    const uint8_t *src_px = (const uint8_t *)tex->pixels;

    int32_t x0 = (dx < 0) ? 0 : dx;
    int32_t y0 = (dy < 0) ? 0 : dy;
    int32_t x1 = dx + dw;
    int32_t y1 = dy + dh;
    if (x1 > (int32_t)dev->width) x1 = (int32_t)dev->width;
    if (y1 > (int32_t)dev->height) y1 = (int32_t)dev->height;

    for (int32_t y = y0; y < y1; ++y) {
        int32_t src_y = sy + (int32_t)(((int64_t)(y - dy) * (int64_t)sh) / (int64_t)dh);
        if (src_y < 0) src_y = 0;
        if (src_y >= (int32_t)tex->height) src_y = (int32_t)tex->height - 1;
        for (int32_t x = x0; x < x1; ++x) {
            int32_t src_x = sx + (int32_t)(((int64_t)(x - dx) * (int64_t)sw) / (int64_t)dw);
            if (src_x < 0) src_x = 0;
            if (src_x >= (int32_t)tex->width) src_x = (int32_t)tex->width - 1;
            const uint8_t *sp = src_px + (size_t)src_y * (size_t)tex->row_pitch + (size_t)src_x * 4u;
            uint8_t sr = (uint8_t)(((uint32_t)sp[0] * (uint32_t)tr) / 255u);
            uint8_t sg = (uint8_t)(((uint32_t)sp[1] * (uint32_t)tg) / 255u);
            uint8_t sb = (uint8_t)(((uint32_t)sp[2] * (uint32_t)tb) / 255u);
            uint8_t sa = (uint8_t)(((uint32_t)sp[3] * (uint32_t)ta) / 255u);
            uint8_t *dp = dev->backbuffer + ((size_t)y * (size_t)dev->width + (size_t)x) * 4u;
            blend_pixel_rgba8(dp, sr, sg, sb, sa);
        }
    }
    dev->draw_calls++;
    return 0;
}

void twd_gfx_set_viewport(
    twd_gfx_device_t *dev, float x, float y, float w, float h, float min_d, float max_d) {
    if (!dev) return;
    dev->viewport.top_left_x = x;
    dev->viewport.top_left_y = y;
    dev->viewport.width = w;
    dev->viewport.height = h;
    dev->viewport.min_depth = min_d;
    dev->viewport.max_depth = max_d;
    if (dev->gl_viewport && dev->egl_surface) {
        dev->gl_viewport((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h);
    }
}

static uint32_t d3d_topo_to_gl_mode(uint32_t topo) {
    if (topo == 1u) return 0x0000u; /* POINTLIST -> GL_POINTS */
    if (topo == 2u) return 0x0001u; /* LINELIST -> GL_LINES */
    if (topo == 3u) return 0x0003u; /* LINESTRIP -> GL_LINE_STRIP */
    if (topo == 5u) return 0x0005u; /* TRIANGLESTRIP -> GL_TRIANGLE_STRIP */
    return 0x0004u;                 /* TRIANGLELIST -> GL_TRIANGLES */
}

static void bind_gl_pipeline(twd_gfx_device_t *dev) {
    if (!dev || !dev->egl_surface) return;
    if (dev->bound_vs_hash && dev->bound_ps_hash &&
        (dev->bound_vs_hash != dev->linked_vs_hash || dev->bound_ps_hash != dev->linked_ps_hash) &&
        dev->gl_create_program && dev->gl_attach_shader && dev->gl_link_program &&
        dev->gl_create_shader && dev->gl_delete_shader) {
        twd_shader_cache_entry_t vs_ent, ps_ent;
        if (twd_shader_cache_lookup(dev->shader_cache, dev->bound_vs_hash, &vs_ent) &&
            twd_shader_cache_lookup(dev->shader_cache, dev->bound_ps_hash, &ps_ent) &&
            vs_ent.glsl_source && ps_ent.glsl_source) {
            uint32_t vs_gl = twd_gfx_gl_compile_glsl(dev, TWD_SHADER_STAGE_VS, vs_ent.glsl_source);
            uint32_t ps_gl = twd_gfx_gl_compile_glsl(dev, TWD_SHADER_STAGE_PS, ps_ent.glsl_source);
            if (vs_gl && ps_gl) {
                uint32_t prog = dev->gl_create_program();
                if (prog) {
                    dev->gl_attach_shader(prog, vs_gl);
                    dev->gl_attach_shader(prog, ps_gl);
                    dev->gl_link_program(prog);
                    if (dev->gl_active_program && dev->gl_delete_program) {
                        dev->gl_delete_program(dev->gl_active_program);
                    }
                    dev->gl_active_program = prog;
                    dev->linked_vs_hash = dev->bound_vs_hash;
                    dev->linked_ps_hash = dev->bound_ps_hash;
                }
            }
            if (vs_gl) dev->gl_delete_shader(vs_gl);
            if (ps_gl) dev->gl_delete_shader(ps_gl);
        }
    }
    if (dev->gl_active_program && dev->gl_use_program) {
        dev->gl_use_program(dev->gl_active_program);
    }
    if (dev->gl_bind_buffer) {
        if (dev->bound_vb_gl) dev->gl_bind_buffer(0x8892u, dev->bound_vb_gl);
        if (dev->bound_ib_gl) dev->gl_bind_buffer(0x8893u, dev->bound_ib_gl);
    }
    if (dev->gl_bind_texture && dev->bound_ps_srv_gl[0]) {
        if (dev->gl_active_texture) dev->gl_active_texture(0x84C0u); /* GL_TEXTURE0 */
        dev->gl_bind_texture(0x0DE1u, dev->bound_ps_srv_gl[0]);
    }
}

void twd_gfx_draw(twd_gfx_device_t *dev, uint32_t vertex_count, uint32_t start_vertex) {
    if (!dev) return;
    bind_gl_pipeline(dev);
    if (dev->gl_draw_arrays && dev->egl_surface) {
        dev->gl_draw_arrays(d3d_topo_to_gl_mode(dev->topology), (int32_t)start_vertex, (int32_t)vertex_count);
    }
    dev->draw_calls++;
}

void twd_gfx_draw_indexed(
    twd_gfx_device_t *dev, uint32_t index_count, uint32_t start_index, int32_t base_vertex) {
    (void)base_vertex;
    if (!dev) return;
    bind_gl_pipeline(dev);
    if (dev->gl_draw_elements && dev->egl_surface) {
        const void *off_ptr = (const void *)(uintptr_t)((size_t)start_index * sizeof(uint16_t));
        dev->gl_draw_elements(d3d_topo_to_gl_mode(dev->topology), (int32_t)index_count, 0x1403u, off_ptr);
    }
    dev->draw_calls++;
}

int twd_gfx_present(twd_gfx_device_t *dev, uint32_t sync_interval) {
    (void)sync_interval;
    if (!dev) return -1;
    if (dev->egl_display && dev->egl_surface && dev->egl_swap_buffers) {
        dev->egl_swap_buffers(dev->egl_display, dev->egl_surface);
    }
    dev->frame_index++;
    return 0;
}

/* ---------- Binary-Compatible D3D11 / DXGI COM Shim ---------- */

#define TWD_OBJ_MAGIC 0x44334431u
#define TWD_KIND_DEVICE   1u
#define TWD_KIND_CONTEXT  2u
#define TWD_KIND_SWAP     3u
#define TWD_KIND_BUFFER      4u
#define TWD_KIND_TEX2D       5u
#define TWD_KIND_VIEW        6u
#define TWD_KIND_GENERIC     7u
#define TWD_KIND_DXGI_DEV    8u
#define TWD_KIND_DXGI_ADAPT  9u
#define TWD_KIND_SHADER      10u

typedef struct shim_com_obj {
    const void *const *lpVtbl;
    uint32_t magic;
    uint32_t kind;
    int32_t refcount;
    twd_gfx_device_t *gfx;
    twd_gfx_buffer_t *buf;
    twd_gfx_texture_t *tex;
    uint64_t shader_hash;
    uint32_t shader_stage;
    struct shim_com_obj *parent;
} shim_com_obj_t;

typedef const void *twd_vfn_t;

static twd_vfn_t g_dev_vtbl[43];
static twd_vfn_t g_ctx_vtbl[115];
static twd_vfn_t g_swap_vtbl[18];
static twd_vfn_t g_child_vtbl[16];
static twd_vfn_t g_dxgi_dev_vtbl[12];
static twd_vfn_t g_dxgi_adapter_vtbl[11];
static int g_vtbls_init = 0;

static const uint8_t k_iid_dxgi_device[16] = {
    0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44,
    0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x82, 0xc0, 0x12
};

static shim_com_obj_t *alloc_shim_obj(
    const void *const *vtbl, uint32_t kind, twd_gfx_device_t *gfx, shim_com_obj_t *parent) {
    shim_com_obj_t *o = (shim_com_obj_t *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->lpVtbl = vtbl;
    o->magic = TWD_OBJ_MAGIC;
    o->kind = kind;
    o->refcount = 1;
    o->gfx = gfx;
    if (gfx) gfx->refs++;
    o->parent = parent;
    return o;
}

static int32_t shim_query_interface(void *self, const void *riid, void **ppv) {
    if (!self || !ppv) return (int32_t)0x80070057u; /* E_INVALIDARG */
    shim_com_obj_t *o = (shim_com_obj_t *)self;
    if (riid && o->kind == TWD_KIND_DEVICE &&
        memcmp(riid, k_iid_dxgi_device, sizeof(k_iid_dxgi_device)) == 0) {
        shim_com_obj_t *dxgi = alloc_shim_obj(g_dxgi_dev_vtbl, TWD_KIND_DXGI_DEV, o->gfx, NULL);
        if (!dxgi) return (int32_t)0x8007000Eu;
        *ppv = dxgi;
        return 0;
    }
    o->refcount++;
    *ppv = self;
    return 0; /* S_OK */
}

static uint32_t shim_add_ref(void *self) {
    if (!self) return 0u;
    shim_com_obj_t *o = (shim_com_obj_t *)self;
    return (uint32_t)(++o->refcount);
}

static uint32_t shim_release(void *self) {
    if (!self) return 0u;
    shim_com_obj_t *o = (shim_com_obj_t *)self;
    int32_t rc = --o->refcount;
    if (rc > 0) return (uint32_t)rc;
    if (o->kind == TWD_KIND_BUFFER && o->buf) {
        twd_gfx_buffer_destroy(o->buf);
    } else if (o->kind == TWD_KIND_TEX2D && o->tex) {
        twd_gfx_texture_destroy(o->tex);
    } else if ((o->kind == TWD_KIND_DEVICE || o->kind == TWD_KIND_VIEW) && o->parent) {
        shim_release(o->parent);
    }
    if (o->gfx) {
        twd_gfx_device_destroy(o->gfx);
    }
    o->magic = 0u;
    free(o);
    return 0u;
}

static void shim_noop_void(void *self) {
    (void)self;
}

static int32_t shim_noop_ok(void *self) {
    (void)self;
    return 0;
}

static uint32_t shim_dev_get_feature_level(void *self) {
    (void)self;
    return TWD_D3D11_FEATURE_LEVEL_11_0;
}

static void shim_dev_get_immediate_context(void *self, void **pp_ctx) {
    if (!pp_ctx) return;
    *pp_ctx = NULL;
    shim_com_obj_t *dev = (shim_com_obj_t *)self;
    if (!dev || dev->magic != TWD_OBJ_MAGIC) return;
    if (dev->parent) {
        shim_add_ref(dev->parent);
        *pp_ctx = dev->parent;
    }
}

static int32_t shim_dev_create_buffer(
    void *self, const twd_d3d11_buffer_desc_t *desc,
    const twd_d3d11_subresource_data_t *init_data, void **pp_buf) {
    shim_com_obj_t *dev = (shim_com_obj_t *)self;
    if (!dev || !desc) return (int32_t)0x80070057u;
    uint32_t kind = (desc->bind_flags & 4u) ? TWD_GFX_BUF_CONSTANT :
                    (desc->bind_flags & 2u) ? TWD_GFX_BUF_INDEX : TWD_GFX_BUF_VERTEX;
    twd_gfx_buffer_t *gb = twd_gfx_buffer_create(
        dev->gfx, kind, desc->byte_width ? desc->byte_width : 64u,
        init_data ? init_data->sys_mem : NULL);
    if (!gb) return (int32_t)0x8007000Eu; /* E_OUTOFMEMORY */
    if (pp_buf) {
        shim_com_obj_t *bo = alloc_shim_obj(g_child_vtbl, TWD_KIND_BUFFER, dev->gfx, NULL);
        if (!bo) {
            twd_gfx_buffer_destroy(gb);
            return (int32_t)0x8007000Eu;
        }
        bo->buf = gb;
        *pp_buf = bo;
    } else {
        twd_gfx_buffer_destroy(gb);
    }
    return 0;
}

static int32_t shim_dev_create_tex2d(
    void *self, const twd_d3d11_tex2d_desc_t *desc,
    const twd_d3d11_subresource_data_t *init_data, void **pp_tex) {
    shim_com_obj_t *dev = (shim_com_obj_t *)self;
    if (!dev || !desc) return (int32_t)0x80070057u;
    twd_gfx_texture_t *gt = twd_gfx_texture_create(
        dev->gfx, desc->width ? desc->width : 1u, desc->height ? desc->height : 1u, desc->format);
    if (!gt) return (int32_t)0x8007000Eu;
    if (init_data && init_data->sys_mem && gt->pixels) {
        size_t copy_bytes = gt->byte_size;
        if (init_data->sys_mem_pitch > 0u && init_data->sys_mem_pitch < gt->row_pitch) {
            copy_bytes = (size_t)init_data->sys_mem_pitch * (size_t)gt->height;
        }
        if (copy_bytes > gt->byte_size) copy_bytes = gt->byte_size;
        memcpy(gt->pixels, init_data->sys_mem, copy_bytes);
        sync_gl_texture(gt);
    }
    if (pp_tex) {
        shim_com_obj_t *to = alloc_shim_obj(g_child_vtbl, TWD_KIND_TEX2D, dev->gfx, NULL);
        if (!to) {
            twd_gfx_texture_destroy(gt);
            return (int32_t)0x8007000Eu;
        }
        to->tex = gt;
        *pp_tex = to;
    } else {
        twd_gfx_texture_destroy(gt);
    }
    return 0;
}

static int32_t shim_dev_create_srv(
    void *self, void *resource, const void *desc, void **pp_out) {
    (void)desc;
    shim_com_obj_t *dev = (shim_com_obj_t *)self;
    if (!dev) return (int32_t)0x80070057u;
    if (pp_out) {
        shim_com_obj_t *res = (shim_com_obj_t *)resource;
        shim_com_obj_t *parent_ref = (res && res->magic == TWD_OBJ_MAGIC) ? res : NULL;
        shim_com_obj_t *o = alloc_shim_obj(g_child_vtbl, TWD_KIND_VIEW, dev->gfx, parent_ref);
        if (!o) return (int32_t)0x8007000Eu;
        if (parent_ref) {
            shim_add_ref(parent_ref);
            o->tex = parent_ref->tex;
            o->buf = parent_ref->buf;
        }
        *pp_out = o;
    }
    return 0;
}

static int32_t shim_dev_create_generic_obj(void *self, const void *a, const void *b, void **pp_out) {
    (void)a;
    (void)b;
    shim_com_obj_t *dev = (shim_com_obj_t *)self;
    if (!dev) return (int32_t)0x80070057u;
    if (pp_out) {
        shim_com_obj_t *o = alloc_shim_obj(g_child_vtbl, TWD_KIND_VIEW, dev->gfx, NULL);
        if (!o) return (int32_t)0x8007000Eu;
        *pp_out = o;
    }
    return 0;
}

static int32_t shim_dev_create_shader_stage(
    void *self, const void *bytecode, size_t len, const void *linkage,
    uint32_t stage, void **pp_out) {
    (void)linkage;
    shim_com_obj_t *dev = (shim_com_obj_t *)self;
    if (!dev || !dev->gfx) return (int32_t)0x80070057u;
    uint64_t sh_hash = 0ULL;
    if (dev->gfx->shader_cache) {
        twd_shader_cache_register_dxbc(dev->gfx->shader_cache, bytecode, len, stage, &sh_hash);
    }
    if (pp_out) {
        shim_com_obj_t *o = alloc_shim_obj(g_child_vtbl, TWD_KIND_SHADER, dev->gfx, NULL);
        if (!o) return (int32_t)0x8007000Eu;
        o->shader_hash = sh_hash;
        o->shader_stage = stage;
        *pp_out = o;
    }
    return 0;
}

static int32_t shim_dev_create_vs(
    void *self, const void *bytecode, size_t len, const void *linkage, void **pp_out) {
    return shim_dev_create_shader_stage(self, bytecode, len, linkage, TWD_SHADER_STAGE_VS, pp_out);
}

static int32_t shim_dev_create_ps(
    void *self, const void *bytecode, size_t len, const void *linkage, void **pp_out) {
    return shim_dev_create_shader_stage(self, bytecode, len, linkage, TWD_SHADER_STAGE_PS, pp_out);
}

static int32_t shim_dev_create_input_layout(
    void *self, const void *descs, uint32_t num, const void *bc, size_t bc_len, void **pp_out) {
    (void)descs;
    (void)num;
    (void)bc;
    (void)bc_len;
    return shim_dev_create_generic_obj(self, NULL, NULL, pp_out);
}

static void shim_ctx_draw_indexed(
    void *self, uint32_t idx_count, uint32_t start_idx, int32_t base_vtx) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) twd_gfx_draw_indexed(ctx->gfx, idx_count, start_idx, base_vtx);
}

static void shim_ctx_draw(void *self, uint32_t vtx_count, uint32_t start_vtx) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) twd_gfx_draw(ctx->gfx, vtx_count, start_vtx);
}

static void shim_ctx_rs_set_viewports(
    void *self, uint32_t num, const twd_d3d11_viewport_t *vps) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx && num > 0u && vps) {
        twd_gfx_set_viewport(ctx->gfx, vps[0].top_left_x, vps[0].top_left_y,
                             vps[0].width, vps[0].height, vps[0].min_depth, vps[0].max_depth);
    }
}

static void shim_ctx_clear_rtv(void *self, void *rtv, const float rgba[4]) {
    (void)rtv;
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) twd_gfx_clear(ctx->gfx, rgba, 1.0f, 0u);
}

static void shim_ctx_clear_dsv(
    void *self, void *dsv, uint32_t flags, float depth, uint8_t stencil) {
    (void)dsv;
    (void)flags;
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) twd_gfx_clear(ctx->gfx, NULL, depth, stencil);
}

static int32_t shim_ctx_map(
    void *self, void *resource, uint32_t subresource, uint32_t map_type,
    uint32_t map_flags, twd_d3d11_mapped_subresource_t *mapped) {
    (void)subresource;
    (void)map_type;
    (void)map_flags;
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    shim_com_obj_t *res = (shim_com_obj_t *)resource;
    if (!ctx || !res || res->magic != TWD_OBJ_MAGIC || !mapped) {
        return (int32_t)0x80070057u;
    }
    memset(mapped, 0, sizeof(*mapped));
    if (res->kind == TWD_KIND_BUFFER && res->buf) {
        mapped->data = res->buf->staging;
        mapped->row_pitch = (uint32_t)res->buf->byte_size;
        mapped->depth_pitch = mapped->row_pitch;
    } else if (res->kind == TWD_KIND_TEX2D && res->tex) {
        mapped->data = res->tex->pixels;
        mapped->row_pitch = res->tex->row_pitch;
        mapped->depth_pitch = (uint32_t)res->tex->byte_size;
    } else {
        return (int32_t)0x80070057u;
    }
    if (ctx->gfx) ctx->gfx->map_calls++;
    return 0;
}

static void shim_ctx_unmap(void *self, void *resource, uint32_t subresource) {
    (void)self;
    (void)subresource;
    shim_com_obj_t *res = (shim_com_obj_t *)resource;
    if (!res || res->magic != TWD_OBJ_MAGIC) return;
    if (res->kind == TWD_KIND_BUFFER && res->buf) sync_gl_buffer(res->buf);
    else if (res->kind == TWD_KIND_TEX2D && res->tex) sync_gl_texture(res->tex);
}

static void shim_ctx_update_subresource(
    void *self, void *dst_resource, uint32_t dst_subresource,
    const twd_d3d11_box_t *dst_box, const void *src_data,
    uint32_t src_row_pitch, uint32_t src_depth_pitch) {
    (void)dst_subresource;
    (void)dst_box;
    (void)src_depth_pitch;
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    shim_com_obj_t *res = (shim_com_obj_t *)dst_resource;
    if (!ctx || !res || res->magic != TWD_OBJ_MAGIC || !src_data) return;
    if (res->kind == TWD_KIND_BUFFER && res->buf) {
        size_t n = (src_row_pitch > 0u && (size_t)src_row_pitch < res->buf->byte_size)
                   ? (size_t)src_row_pitch : res->buf->byte_size;
        memcpy(res->buf->staging, src_data, n);
    } else if (res->kind == TWD_KIND_TEX2D && res->tex && res->tex->pixels) {
        uint32_t row_bytes = src_row_pitch ? src_row_pitch : res->tex->row_pitch;
        if (row_bytes > res->tex->row_pitch) row_bytes = res->tex->row_pitch;
        for (uint32_t y = 0; y < res->tex->height; y++) {
            memcpy((uint8_t *)res->tex->pixels + (size_t)y * res->tex->row_pitch,
                   (const uint8_t *)src_data + (size_t)y * (src_row_pitch ? src_row_pitch : res->tex->row_pitch),
                   row_bytes);
        }
        sync_gl_texture(res->tex);
    } else if (res->kind == TWD_KIND_BUFFER && res->buf) {
        sync_gl_buffer(res->buf);
    }
    if (ctx->gfx) ctx->gfx->update_calls++;
}

static void shim_ctx_copy_resource(void *self, void *dst_resource, void *src_resource) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    shim_com_obj_t *dst = (shim_com_obj_t *)dst_resource;
    shim_com_obj_t *src = (shim_com_obj_t *)src_resource;
    if (!dst || !src || dst->magic != TWD_OBJ_MAGIC || src->magic != TWD_OBJ_MAGIC) return;
    if (dst->kind == TWD_KIND_BUFFER && src->kind == TWD_KIND_BUFFER && dst->buf && src->buf) {
        size_t n = (dst->buf->byte_size < src->buf->byte_size) ? dst->buf->byte_size : src->buf->byte_size;
        memcpy(dst->buf->staging, src->buf->staging, n);
    } else if (dst->kind == TWD_KIND_TEX2D && src->kind == TWD_KIND_TEX2D &&
               dst->tex && src->tex && dst->tex->pixels && src->tex->pixels) {
        size_t n = (dst->tex->byte_size < src->tex->byte_size) ? dst->tex->byte_size : src->tex->byte_size;
        memcpy(dst->tex->pixels, src->tex->pixels, n);
    }
    if (ctx && ctx->gfx) ctx->gfx->update_calls++;
}

static void shim_ctx_ps_set_shader_resources(
    void *self, uint32_t start_slot, uint32_t num_views, void *const *srvs) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (!ctx || !ctx->gfx || start_slot >= 8u) return;
    for (uint32_t i = 0u; i < num_views && (start_slot + i) < 8u; ++i) {
        void *v = srvs ? srvs[i] : NULL;
        ctx->gfx->bound_ps_srvs[start_slot + i] = v;
        shim_com_obj_t *vo = (shim_com_obj_t *)v;
        ctx->gfx->bound_ps_srv_gl[start_slot + i] =
            (vo && vo->magic == TWD_OBJ_MAGIC && vo->tex) ? vo->tex->gl_handle : 0u;
    }
}

static void shim_ctx_vs_set_constant_buffers(
    void *self, uint32_t start_slot, uint32_t num_bufs, void *const *bufs) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (!ctx || !ctx->gfx || start_slot >= 8u) return;
    for (uint32_t i = 0u; i < num_bufs && (start_slot + i) < 8u; ++i) {
        ctx->gfx->bound_vs_cbs[start_slot + i] = bufs ? bufs[i] : NULL;
    }
}

static void shim_ctx_ps_set_constant_buffers(
    void *self, uint32_t start_slot, uint32_t num_bufs, void *const *bufs) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (!ctx || !ctx->gfx || start_slot >= 8u) return;
    for (uint32_t i = 0u; i < num_bufs && (start_slot + i) < 8u; ++i) {
        ctx->gfx->bound_ps_cbs[start_slot + i] = bufs ? bufs[i] : NULL;
    }
}

static void shim_ctx_ps_set_shader(void *self, void *ps, const void *const *cls, uint32_t num) {
    (void)cls;
    (void)num;
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) {
        ctx->gfx->bound_ps = ps;
        shim_com_obj_t *so = (shim_com_obj_t *)ps;
        ctx->gfx->bound_ps_hash = (so && so->magic == TWD_OBJ_MAGIC) ? so->shader_hash : 0ULL;
    }
}

static void shim_ctx_vs_set_shader(void *self, void *vs, const void *const *cls, uint32_t num) {
    (void)cls;
    (void)num;
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) {
        ctx->gfx->bound_vs = vs;
        shim_com_obj_t *so = (shim_com_obj_t *)vs;
        ctx->gfx->bound_vs_hash = (so && so->magic == TWD_OBJ_MAGIC) ? so->shader_hash : 0ULL;
    }
}

static void shim_ctx_ia_set_input_layout(void *self, void *layout) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) ctx->gfx->bound_input_layout = layout;
}

static void shim_ctx_ia_set_vertex_buffers(
    void *self, uint32_t start, uint32_t num, void *const *vbs, const uint32_t *strides, const uint32_t *offsets) {
    (void)start;
    (void)strides;
    (void)offsets;
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx && num > 0u && vbs) {
        ctx->gfx->bound_vb = vbs[0];
        shim_com_obj_t *bo = (shim_com_obj_t *)vbs[0];
        ctx->gfx->bound_vb_gl = (bo && bo->magic == TWD_OBJ_MAGIC && bo->buf) ? bo->buf->gl_handle : 0u;
    }
}

static void shim_ctx_ia_set_index_buffer(void *self, void *ib, uint32_t fmt, uint32_t offset) {
    (void)fmt;
    (void)offset;
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) {
        ctx->gfx->bound_ib = ib;
        shim_com_obj_t *bo = (shim_com_obj_t *)ib;
        ctx->gfx->bound_ib_gl = (bo && bo->magic == TWD_OBJ_MAGIC && bo->buf) ? bo->buf->gl_handle : 0u;
    }
}

static void shim_ctx_ia_set_primitive_topology(void *self, uint32_t topology) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx) ctx->gfx->topology = topology;
}

static void shim_ctx_om_set_render_targets(
    void *self, uint32_t num_views, void *const *rtvs, void *dsv) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (!ctx || !ctx->gfx) return;
    uint32_t n = (num_views > 8u) ? 8u : num_views;
    ctx->gfx->bound_rtv_count = n;
    for (uint32_t i = 0; i < n; i++) {
        ctx->gfx->bound_rtvs[i] = rtvs ? rtvs[i] : NULL;
    }
    ctx->gfx->bound_dsv = dsv;
}

static void shim_ctx_rs_set_scissor_rects(
    void *self, uint32_t num, const twd_d3d11_rect_t *rects) {
    shim_com_obj_t *ctx = (shim_com_obj_t *)self;
    if (ctx && ctx->gfx && num > 0u && rects) {
        ctx->gfx->scissor = rects[0];
    }
}

static int32_t shim_swap_present(void *self, uint32_t sync_interval, uint32_t flags) {
    (void)flags;
    shim_com_obj_t *sw = (shim_com_obj_t *)self;
    if (!sw || !sw->gfx) return (int32_t)0x80070057u;
    return (twd_gfx_present(sw->gfx, sync_interval) == 0) ? 0 : (int32_t)0x80004005u;
}

static int32_t shim_swap_get_buffer(void *self, uint32_t buffer, const void *riid, void **pp_surface) {
    (void)buffer;
    (void)riid;
    shim_com_obj_t *sw = (shim_com_obj_t *)self;
    if (!sw || !sw->gfx || !pp_surface) return (int32_t)0x80070057u;
    twd_gfx_texture_t *bb = twd_gfx_texture_create(sw->gfx, sw->gfx->width, sw->gfx->height, 28u);
    if (!bb) return (int32_t)0x8007000Eu;
    shim_com_obj_t *to = alloc_shim_obj(g_child_vtbl, TWD_KIND_TEX2D, sw->gfx, NULL);
    if (!to) {
        twd_gfx_texture_destroy(bb);
        return (int32_t)0x8007000Eu;
    }
    to->tex = bb;
    *pp_surface = to;
    return 0;
}

static int32_t shim_swap_resize_buffers(
    void *self, uint32_t count, uint32_t width, uint32_t height, uint32_t fmt, uint32_t flags) {
    (void)count;
    (void)fmt;
    (void)flags;
    shim_com_obj_t *sw = (shim_com_obj_t *)self;
    if (!sw || !sw->gfx) return (int32_t)0x80070057u;
    return (twd_gfx_resize(sw->gfx, width ? width : 1280u, height ? height : 720u) == 0)
           ? 0 : (int32_t)0x80070057u;
}

static int32_t shim_dxgi_get_adapter(void *self, void **pp_adapter) {
    shim_com_obj_t *dx = (shim_com_obj_t *)self;
    if (!dx || !pp_adapter) return (int32_t)0x80070057u;
    shim_com_obj_t *ad = alloc_shim_obj(g_dxgi_adapter_vtbl, TWD_KIND_DXGI_ADAPT, dx->gfx, NULL);
    if (!ad) return (int32_t)0x8007000Eu;
    *pp_adapter = ad;
    return 0;
}

static int32_t shim_dxgi_adapter_get_desc1(void *self, twd_dxgi_adapter_desc1_t *desc) {
    (void)self;
    if (!desc) return (int32_t)0x80070057u;
    memset(desc, 0, sizeof(*desc));
    twd_utf8_to_utf16("TWD-Final Vulkan/GLES", -1, desc->description, 128);
    desc->vendor_id = 0x13B5u;
    desc->device_id = 0x5457u;
    desc->dedicated_video_mem = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    desc->shared_system_mem = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    return 0;
}

static void init_shim_vtbls(void) {
    if (g_vtbls_init) return;
    size_t i;
    for (i = 0; i < sizeof(g_child_vtbl) / sizeof(g_child_vtbl[0]); i++) {
        g_child_vtbl[i] = (twd_vfn_t)&shim_noop_ok;
    }
    g_child_vtbl[0] = (twd_vfn_t)&shim_query_interface;
    g_child_vtbl[1] = (twd_vfn_t)&shim_add_ref;
    g_child_vtbl[2] = (twd_vfn_t)&shim_release;

    for (i = 0; i < sizeof(g_dev_vtbl) / sizeof(g_dev_vtbl[0]); i++) {
        g_dev_vtbl[i] = (twd_vfn_t)&shim_noop_ok;
    }
    g_dev_vtbl[0]  = (twd_vfn_t)&shim_query_interface;
    g_dev_vtbl[1]  = (twd_vfn_t)&shim_add_ref;
    g_dev_vtbl[2]  = (twd_vfn_t)&shim_release;
    g_dev_vtbl[3]  = (twd_vfn_t)&shim_dev_create_buffer;
    g_dev_vtbl[5]  = (twd_vfn_t)&shim_dev_create_tex2d;
    g_dev_vtbl[7]  = (twd_vfn_t)&shim_dev_create_srv;         /* CreateShaderResourceView */
    g_dev_vtbl[9]  = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateRenderTargetView */
    g_dev_vtbl[10] = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateDepthStencilView */
    g_dev_vtbl[11] = (twd_vfn_t)&shim_dev_create_input_layout;
    g_dev_vtbl[12] = (twd_vfn_t)&shim_dev_create_vs;           /* CreateVertexShader */
    g_dev_vtbl[15] = (twd_vfn_t)&shim_dev_create_ps;           /* CreatePixelShader */
    g_dev_vtbl[20] = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateBlendState */
    g_dev_vtbl[21] = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateDepthStencilState */
    g_dev_vtbl[22] = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateRasterizerState */
    g_dev_vtbl[23] = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateSamplerState */
    g_dev_vtbl[37] = (twd_vfn_t)&shim_dev_get_feature_level;
    g_dev_vtbl[40] = (twd_vfn_t)&shim_dev_get_immediate_context;

    for (i = 0; i < sizeof(g_ctx_vtbl) / sizeof(g_ctx_vtbl[0]); i++) {
        g_ctx_vtbl[i] = (twd_vfn_t)&shim_noop_void;
    }
    g_ctx_vtbl[0]  = (twd_vfn_t)&shim_query_interface;
    g_ctx_vtbl[1]  = (twd_vfn_t)&shim_add_ref;
    g_ctx_vtbl[2]  = (twd_vfn_t)&shim_release;
    g_ctx_vtbl[7]  = (twd_vfn_t)&shim_ctx_vs_set_constant_buffers;
    g_ctx_vtbl[8]  = (twd_vfn_t)&shim_ctx_ps_set_shader_resources;
    g_ctx_vtbl[9]  = (twd_vfn_t)&shim_ctx_ps_set_shader;
    g_ctx_vtbl[11] = (twd_vfn_t)&shim_ctx_vs_set_shader;
    g_ctx_vtbl[12] = (twd_vfn_t)&shim_ctx_draw_indexed;
    g_ctx_vtbl[13] = (twd_vfn_t)&shim_ctx_draw;
    g_ctx_vtbl[14] = (twd_vfn_t)&shim_ctx_map;
    g_ctx_vtbl[15] = (twd_vfn_t)&shim_ctx_unmap;
    g_ctx_vtbl[16] = (twd_vfn_t)&shim_ctx_ps_set_constant_buffers;
    g_ctx_vtbl[17] = (twd_vfn_t)&shim_ctx_ia_set_input_layout;
    g_ctx_vtbl[18] = (twd_vfn_t)&shim_ctx_ia_set_vertex_buffers;
    g_ctx_vtbl[19] = (twd_vfn_t)&shim_ctx_ia_set_index_buffer;
    g_ctx_vtbl[24] = (twd_vfn_t)&shim_ctx_ia_set_primitive_topology;
    g_ctx_vtbl[33] = (twd_vfn_t)&shim_ctx_om_set_render_targets;
    g_ctx_vtbl[44] = (twd_vfn_t)&shim_ctx_rs_set_viewports;
    g_ctx_vtbl[45] = (twd_vfn_t)&shim_ctx_rs_set_scissor_rects;
    g_ctx_vtbl[47] = (twd_vfn_t)&shim_ctx_copy_resource;
    g_ctx_vtbl[48] = (twd_vfn_t)&shim_ctx_update_subresource;
    g_ctx_vtbl[50] = (twd_vfn_t)&shim_ctx_clear_rtv;
    g_ctx_vtbl[53] = (twd_vfn_t)&shim_ctx_clear_dsv;

    for (i = 0; i < sizeof(g_swap_vtbl) / sizeof(g_swap_vtbl[0]); i++) {
        g_swap_vtbl[i] = (twd_vfn_t)&shim_noop_ok;
    }
    g_swap_vtbl[0]  = (twd_vfn_t)&shim_query_interface;
    g_swap_vtbl[1]  = (twd_vfn_t)&shim_add_ref;
    g_swap_vtbl[2]  = (twd_vfn_t)&shim_release;
    g_swap_vtbl[8]  = (twd_vfn_t)&shim_swap_present;
    g_swap_vtbl[9]  = (twd_vfn_t)&shim_swap_get_buffer;
    g_swap_vtbl[13] = (twd_vfn_t)&shim_swap_resize_buffers;

    for (i = 0; i < sizeof(g_dxgi_dev_vtbl) / sizeof(g_dxgi_dev_vtbl[0]); i++) {
        g_dxgi_dev_vtbl[i] = (twd_vfn_t)&shim_noop_ok;
    }
    g_dxgi_dev_vtbl[0] = (twd_vfn_t)&shim_query_interface;
    g_dxgi_dev_vtbl[1] = (twd_vfn_t)&shim_add_ref;
    g_dxgi_dev_vtbl[2] = (twd_vfn_t)&shim_release;
    g_dxgi_dev_vtbl[7] = (twd_vfn_t)&shim_dxgi_get_adapter;

    for (i = 0; i < sizeof(g_dxgi_adapter_vtbl) / sizeof(g_dxgi_adapter_vtbl[0]); i++) {
        g_dxgi_adapter_vtbl[i] = (twd_vfn_t)&shim_noop_ok;
    }
    g_dxgi_adapter_vtbl[0]  = (twd_vfn_t)&shim_query_interface;
    g_dxgi_adapter_vtbl[1]  = (twd_vfn_t)&shim_add_ref;
    g_dxgi_adapter_vtbl[2]  = (twd_vfn_t)&shim_release;
    g_dxgi_adapter_vtbl[8]  = (twd_vfn_t)&shim_dxgi_adapter_get_desc1;
    g_dxgi_adapter_vtbl[10] = (twd_vfn_t)&shim_dxgi_adapter_get_desc1;

    g_vtbls_init = 1;
}

int32_t twd_d3d11_create_device_shim(
    twd_window_t *window,
    uint32_t width,
    uint32_t height,
    uint32_t *out_feature_level,
    void **out_device,
    void **out_context,
    void **out_swapchain) {
    init_shim_vtbls();
    int w = width ? (int)width : 1280;
    int h = height ? (int)height : 720;
    if (twd_window_is_valid(window)) {
        twd_window_get_client_rect(window, &w, &h);
        if (w <= 0) w = 1280;
        if (h <= 0) h = 720;
    }
    twd_gfx_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.native_window = window;
    desc.width = (uint32_t)w;
    desc.height = (uint32_t)h;
    desc.preferred_backend = -1;
    desc.vsync = 1;

    twd_gfx_device_t *gfx = twd_gfx_device_create(&desc);
    if (!gfx) return (int32_t)0x8007000Eu;

    shim_com_obj_t *ctx = alloc_shim_obj(g_ctx_vtbl, TWD_KIND_CONTEXT, gfx, NULL);
    shim_com_obj_t *dev = alloc_shim_obj(g_dev_vtbl, TWD_KIND_DEVICE, gfx, ctx);
    shim_com_obj_t *sw  = out_swapchain
                        ? alloc_shim_obj(g_swap_vtbl, TWD_KIND_SWAP, gfx, NULL)
                        : NULL;
    twd_gfx_device_destroy(gfx); /* transfer ownership to COM wrappers */
    if (!ctx || !dev || (out_swapchain && !sw)) {
        if (dev) shim_release(dev);
        else if (ctx) shim_release(ctx);
        if (sw) shim_release(sw);
        return (int32_t)0x8007000Eu;
    }
    if (out_feature_level) *out_feature_level = TWD_D3D11_FEATURE_LEVEL_11_0;
    if (out_context) {
        shim_add_ref(ctx);
        *out_context = ctx;
    }
    if (out_device) *out_device = dev;
    else shim_release(dev);
    if (out_swapchain) *out_swapchain = sw;
    return 0;
}

twd_gfx_device_t *twd_d3d11_get_gfx_device(void *d3d11_device) {
    shim_com_obj_t *o = (shim_com_obj_t *)d3d11_device;
    if (!o || o->magic != TWD_OBJ_MAGIC) return NULL;
    return o->gfx;
}
