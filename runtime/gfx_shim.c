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
    uint32_t width;
    uint32_t height;
    int vsync;
    uint64_t frame_index;
    uint64_t draw_calls;
    uint64_t clear_calls;
    uint32_t active_buffers;
    uint32_t active_textures;
    float last_clear_rgba[4];
    float last_depth;
    uint8_t last_stencil;
    twd_d3d11_viewport_t viewport;
};

struct twd_gfx_buffer {
    twd_gfx_device_t *dev;
    uint32_t kind;
    size_t byte_size;
    void *staging;
};

struct twd_gfx_texture {
    twd_gfx_device_t *dev;
    uint32_t width;
    uint32_t height;
    uint32_t format;
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

#ifdef __ANDROID__
    if (dev->backend == TWD_GFX_BACKEND_VULKAN) {
        dev->backend_lib = twd_dynlib_open("libvulkan.so");
        if (dev->backend_lib &&
            twd_dynlib_sym(dev->backend_lib, "vkCreateInstance") &&
            twd_dynlib_sym(dev->backend_lib, "vkEnumeratePhysicalDevices") &&
            twd_dynlib_sym(dev->backend_lib, "vkCreateDevice")) {
            dev->driver_caps |= 1;
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
        if (dev->backend_lib &&
            twd_dynlib_sym(dev->backend_lib, "eglGetDisplay") &&
            twd_dynlib_sym(dev->backend_lib, "eglInitialize") &&
            twd_dynlib_sym(dev->backend_lib, "eglSwapBuffers")) {
            dev->driver_caps |= 2;
        }
    }
#endif
    return dev;
}

int twd_gfx_has_driver_symbols(const twd_gfx_device_t *dev) {
    return dev ? dev->driver_caps : 0;
}

void twd_gfx_device_destroy(twd_gfx_device_t *dev) {
    if (!dev) return;
    if (--dev->refs > 0) return;
    if (dev->backend_lib) {
        twd_dynlib_close(dev->backend_lib);
        dev->backend_lib = NULL;
    }
    free(dev);
}

int twd_gfx_resize(twd_gfx_device_t *dev, uint32_t width, uint32_t height) {
    if (!dev || width == 0u || height == 0u) return -1;
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
    out->active_buffers = dev->active_buffers;
    out->active_textures = dev->active_textures;
    memcpy(out->last_clear_rgba, dev->last_clear_rgba, sizeof(out->last_clear_rgba));
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
    dev->active_buffers++;
    return buf;
}

int twd_gfx_buffer_update(twd_gfx_buffer_t *buf, const void *data, size_t byte_size) {
    if (!buf || !data || byte_size > buf->byte_size) return -1;
    memcpy(buf->staging, data, byte_size);
    return 0;
}

void twd_gfx_buffer_destroy(twd_gfx_buffer_t *buf) {
    if (!buf) return;
    if (buf->dev && buf->dev->active_buffers > 0u) {
        buf->dev->active_buffers--;
    }
    free(buf->staging);
    free(buf);
}

twd_gfx_texture_t *twd_gfx_texture_create(
    twd_gfx_device_t *dev, uint32_t width, uint32_t height, uint32_t format) {
    if (!dev || width == 0u || height == 0u) return NULL;
    twd_gfx_texture_t *tex = (twd_gfx_texture_t *)calloc(1, sizeof(*tex));
    if (!tex) return NULL;
    tex->dev = dev;
    tex->width = width;
    tex->height = height;
    tex->format = format;
    dev->active_textures++;
    return tex;
}

void twd_gfx_texture_destroy(twd_gfx_texture_t *tex) {
    if (!tex) return;
    if (tex->dev && tex->dev->active_textures > 0u) {
        tex->dev->active_textures--;
    }
    free(tex);
}

void twd_gfx_begin_frame(twd_gfx_device_t *dev) {
    (void)dev;
}

void twd_gfx_clear(twd_gfx_device_t *dev, const float rgba[4], float depth, uint8_t stencil) {
    if (!dev) return;
    if (rgba) {
        memcpy(dev->last_clear_rgba, rgba, sizeof(dev->last_clear_rgba));
    }
    dev->last_depth = depth;
    dev->last_stencil = stencil;
    dev->clear_calls++;
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
}

void twd_gfx_draw(twd_gfx_device_t *dev, uint32_t vertex_count, uint32_t start_vertex) {
    (void)vertex_count;
    (void)start_vertex;
    if (dev) dev->draw_calls++;
}

void twd_gfx_draw_indexed(
    twd_gfx_device_t *dev, uint32_t index_count, uint32_t start_index, int32_t base_vertex) {
    (void)index_count;
    (void)start_index;
    (void)base_vertex;
    if (dev) dev->draw_calls++;
}

int twd_gfx_present(twd_gfx_device_t *dev, uint32_t sync_interval) {
    (void)sync_interval;
    if (!dev) return -1;
    dev->frame_index++;
    return 0;
}

/* ---------- Binary-Compatible D3D11 / DXGI COM Shim ---------- */

#define TWD_OBJ_MAGIC 0x44334431u
#define TWD_KIND_DEVICE   1u
#define TWD_KIND_CONTEXT  2u
#define TWD_KIND_SWAP     3u
#define TWD_KIND_BUFFER   4u
#define TWD_KIND_TEX2D    5u
#define TWD_KIND_VIEW     6u
#define TWD_KIND_GENERIC  7u

typedef struct shim_com_obj {
    const void *const *lpVtbl;
    uint32_t magic;
    uint32_t kind;
    int32_t refcount;
    twd_gfx_device_t *gfx;
    twd_gfx_buffer_t *buf;
    twd_gfx_texture_t *tex;
    struct shim_com_obj *parent;
} shim_com_obj_t;

typedef const void *twd_vfn_t;

static twd_vfn_t g_dev_vtbl[43];
static twd_vfn_t g_ctx_vtbl[115];
static twd_vfn_t g_swap_vtbl[18];
static twd_vfn_t g_child_vtbl[16];
static int g_vtbls_init = 0;

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
    (void)riid;
    if (!self || !ppv) return (int32_t)0x80070057u; /* E_INVALIDARG */
    shim_com_obj_t *o = (shim_com_obj_t *)self;
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
    } else if (o->kind == TWD_KIND_DEVICE && o->parent) {
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
    (void)init_data;
    shim_com_obj_t *dev = (shim_com_obj_t *)self;
    if (!dev || !desc) return (int32_t)0x80070057u;
    twd_gfx_texture_t *gt = twd_gfx_texture_create(
        dev->gfx, desc->width ? desc->width : 1u, desc->height ? desc->height : 1u, desc->format);
    if (!gt) return (int32_t)0x8007000Eu;
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

static int32_t shim_dev_create_generic4(
    void *self, const void *a, size_t b, const void *c, void **pp_out) {
    (void)a;
    (void)b;
    (void)c;
    return shim_dev_create_generic_obj(self, NULL, NULL, pp_out);
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
    g_dev_vtbl[7]  = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateShaderResourceView */
    g_dev_vtbl[9]  = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateRenderTargetView */
    g_dev_vtbl[10] = (twd_vfn_t)&shim_dev_create_generic_obj; /* CreateDepthStencilView */
    g_dev_vtbl[11] = (twd_vfn_t)&shim_dev_create_input_layout;
    g_dev_vtbl[12] = (twd_vfn_t)&shim_dev_create_generic4;    /* CreateVertexShader */
    g_dev_vtbl[15] = (twd_vfn_t)&shim_dev_create_generic4;    /* CreatePixelShader */
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
    g_ctx_vtbl[12] = (twd_vfn_t)&shim_ctx_draw_indexed;
    g_ctx_vtbl[13] = (twd_vfn_t)&shim_ctx_draw;
    g_ctx_vtbl[14] = (twd_vfn_t)&shim_noop_ok;                /* Map */
    g_ctx_vtbl[44] = (twd_vfn_t)&shim_ctx_rs_set_viewports;
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
