/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TWD_RUNTIME_H
#define TWD_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#if !defined(__cplusplus) && defined(_MSC_VER) && _MSC_VER < 1928
#define _TWD_CAT2(a, b) a##b
#define _TWD_CAT(a, b) _TWD_CAT2(a, b)
#ifndef _Static_assert
#define _Static_assert(cond, msg) typedef char _TWD_CAT(_twd_sa_, __COUNTER__)[(cond) ? 1 : -1]
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TWD_TLS_INVALID 0xffffffffu

typedef struct twd_hook_entry {
    uint64_t address;
    const char *name;
    void *replacement;
} twd_hook_entry_t;

typedef union twd_mutex {
    uint64_t align;
    uint8_t bytes[64];
} twd_mutex_t;

typedef union twd_event {
    uint64_t align;
    uint8_t bytes[128];
} twd_event_t;

typedef union twd_sem {
    uint64_t align;
    uint8_t bytes[128];
} twd_sem_t;

typedef struct twd_thread {
    uintptr_t handle;
    void *ctx;
} twd_thread_t;

typedef int (*twd_thread_fn)(void *arg);

typedef struct twd_file {
    intptr_t handle;
} twd_file_t;

typedef struct twd_dir {
    uintptr_t handle;
    int first_pending;
    char pending_name[260];
} twd_dir_t;

typedef struct twd_dir_entry {
    char name[260];
    int is_dir;
    uint64_t size;
} twd_dir_entry_t;

typedef struct twd_window {
    uint32_t magic;
    int x;
    int y;
    int width;
    int height;
    uint32_t style;
    int visible;
    char title[128];
} twd_window_t;

typedef struct twd_win_event {
    uint32_t kind;
    uintptr_t window_id;
    uint64_t param1;
    int64_t param2;
} twd_win_event_t;

#define TWD_EVT_NONE    0u
#define TWD_EVT_CLOSE   1u
#define TWD_EVT_RESIZE  2u
#define TWD_EVT_KEY     3u
#define TWD_EVT_POINTER 4u
#define TWD_EVT_QUIT    5u

#define TWD_ALERT_OK     1
#define TWD_ALERT_CANCEL 2
#define TWD_ALERT_YES    6

#define TWD_METRIC_SCREEN_W      0
#define TWD_METRIC_SCREEN_H      1
#define TWD_METRIC_MOUSE_PRESENT 19

#define TWD_FILE_OPEN_READ   1u
#define TWD_FILE_OPEN_WRITE  2u
#define TWD_FILE_CREATE      4u

#define TWD_SEEK_SET 0
#define TWD_SEEK_CUR 1
#define TWD_SEEK_END 2

#define TWD_WAIT_OK      0
#define TWD_WAIT_TIMEOUT 1
#define TWD_WAIT_ERROR   -1
#define TWD_WAIT_INFINITE 0xffffffffu

int twd_platform_init(void);
void twd_platform_shutdown(void);
uint64_t twd_time_now_ns(void);
uint64_t twd_qpc_now(void);
uint64_t twd_qpc_freq(void);
uint32_t twd_ticks_ms(void);
void twd_sleep_ms(uint32_t ms);
void twd_thread_yield(void);
uint32_t twd_pid(void);
uint32_t twd_tid(void);
uint32_t twd_last_error(void);
void twd_set_last_error(uint32_t err);
void twd_debug_output(const char *msg);
void twd_log(const char *tag, const char *fmt, ...);

void *twd_vm_alloc(size_t size, int exec);
int twd_vm_protect(void *ptr, size_t size, int read_write, int exec);
void twd_vm_free(void *ptr, size_t size);
void *twd_heap_alloc(size_t size);
void *twd_heap_realloc(void *ptr, size_t size);
void twd_heap_free(void *ptr);

int twd_mutex_init(twd_mutex_t *m);
void twd_mutex_destroy(twd_mutex_t *m);
void twd_mutex_lock(twd_mutex_t *m);
void twd_mutex_unlock(twd_mutex_t *m);

int twd_event_init(twd_event_t *ev, int manual_reset, int initial_state);
void twd_event_destroy(twd_event_t *ev);
int twd_event_set(twd_event_t *ev);
int twd_event_reset(twd_event_t *ev);
int twd_event_wait(twd_event_t *ev, uint32_t timeout_ms);

uint32_t twd_tls_alloc(void);
int twd_tls_free(uint32_t key);
void *twd_tls_get(uint32_t key);
int twd_tls_set(uint32_t key, void *value);

int twd_file_open(const char *path, uint32_t flags, twd_file_t *out);
int64_t twd_file_read(twd_file_t *f, void *buf, size_t len);
int64_t twd_file_write(twd_file_t *f, const void *buf, size_t len);
int64_t twd_file_seek(twd_file_t *f, int64_t offset, int whence);
int twd_file_flush(twd_file_t *f);
int twd_file_close(twd_file_t *f);
int twd_file_delete(const char *path);
int twd_mkdir(const char *path);
int twd_rmdir(const char *path);

int twd_utf8_to_utf16(const char *src, int src_len, uint16_t *dst, int dst_cap);
int twd_utf16_to_utf8(const uint16_t *src, int src_len, char *dst, int dst_cap);
size_t twd_utf16_len(const uint16_t *s);
int twd_utf16_cmp(const uint16_t *a, const uint16_t *b, int ignore_case);

int twd_exe_path(char *buf, size_t cap);
int twd_getcwd(char *buf, size_t cap);
int twd_chdir(const char *path);
int twd_getenv(const char *name, char *buf, size_t cap);
int twd_setenv(const char *name, const char *val);

int twd_file_exists(const char *path);
int twd_path_is_dir(const char *path);
int twd_file_rename(const char *src, const char *dst);
int twd_file_truncate(twd_file_t *f, int64_t size);
int twd_dir_open(const char *path, twd_dir_t *out);
int twd_dir_next(twd_dir_t *d, twd_dir_entry_t *out);
void twd_dir_close(twd_dir_t *d);

uint32_t twd_cpu_count(void);
uint32_t twd_page_size(void);
uint64_t twd_unix_time_ns(void);
void twd_icache_flush(void *addr, size_t size);
int twd_timer_begin_period(uint32_t ms);
int twd_timer_end_period(uint32_t ms);

int twd_sem_init(twd_sem_t *s, uint32_t initial, uint32_t max_count);
void twd_sem_destroy(twd_sem_t *s);
int twd_sem_post(twd_sem_t *s, uint32_t count);
int twd_sem_wait(twd_sem_t *s, uint32_t timeout_ms);

int twd_thread_create(twd_thread_t *th, twd_thread_fn fn, void *arg);
int twd_thread_join(twd_thread_t *th, int *out_rc);

void *twd_dynlib_open(const char *path_or_null);
void *twd_dynlib_sym(void *lib, const char *name);
void twd_dynlib_close(void *lib);

void twd_alert(const char *title, const char *msg);
int twd_alert_box(const char *title, const char *msg, uint32_t flags);

twd_window_t *twd_window_create(const char *title, int x, int y, int width, int height, uint32_t style);
void twd_window_destroy(twd_window_t *win);
int twd_window_is_valid(const twd_window_t *win);
void twd_window_show(twd_window_t *win, int visible);
void twd_window_set_title(twd_window_t *win, const char *title);
int twd_window_get_title(const twd_window_t *win, char *buf, size_t cap);
void twd_window_set_rect(twd_window_t *win, int x, int y, int width, int height);
void twd_window_get_client_rect(const twd_window_t *win, int *out_w, int *out_h);
void twd_window_push_event(const twd_win_event_t *evt);
int twd_window_poll_event(twd_win_event_t *out_evt);
void twd_post_quit(int exit_code);
int twd_quit_requested(int *out_code);
void twd_clear_quit(void);
int twd_system_metric(int metric_id);

/* Graphics Backend PAL + D3D11 -> Vulkan / GLES Shim */
#define TWD_GFX_BACKEND_NONE   0
#define TWD_GFX_BACKEND_VULKAN 1
#define TWD_GFX_BACKEND_GLES3  2

#define TWD_GFX_BUF_VERTEX   1u
#define TWD_GFX_BUF_INDEX    2u
#define TWD_GFX_BUF_CONSTANT 3u

#define TWD_D3D11_FEATURE_LEVEL_11_0 0xb000u

typedef struct twd_gfx_desc {
    void *native_window;
    uint32_t width;
    uint32_t height;
    int preferred_backend;
    int vsync;
} twd_gfx_desc_t;

typedef struct twd_gfx_stats {
    int backend;
    uint32_t width;
    uint32_t height;
    uint64_t frame_index;
    uint64_t draw_calls;
    uint64_t clear_calls;
    uint64_t map_calls;
    uint64_t update_calls;
    uint32_t active_buffers;
    uint32_t active_textures;
    uint32_t topology;
    uint32_t bound_rtv_count;
    int32_t scissor_right;
    int32_t scissor_bottom;
    uint32_t cached_shaders;
    uint64_t shader_cache_hits;
    uint64_t shader_cache_misses;
    float last_clear_rgba[4];
} twd_gfx_stats_t;

typedef struct twd_gfx_device twd_gfx_device_t;
typedef struct twd_gfx_buffer twd_gfx_buffer_t;
typedef struct twd_gfx_texture twd_gfx_texture_t;

int twd_gfx_select_backend(int preferred);
twd_gfx_device_t *twd_gfx_device_create(const twd_gfx_desc_t *desc);
void twd_gfx_device_destroy(twd_gfx_device_t *dev);
int twd_gfx_resize(twd_gfx_device_t *dev, uint32_t width, uint32_t height);
void twd_gfx_get_stats(const twd_gfx_device_t *dev, twd_gfx_stats_t *out);

twd_gfx_buffer_t *twd_gfx_buffer_create(twd_gfx_device_t *dev, uint32_t kind, size_t byte_size, const void *initial_data);
int twd_gfx_buffer_update(twd_gfx_buffer_t *buf, const void *data, size_t byte_size);
const void *twd_gfx_buffer_data(const twd_gfx_buffer_t *buf, size_t *out_size);
void twd_gfx_buffer_destroy(twd_gfx_buffer_t *buf);

twd_gfx_texture_t *twd_gfx_texture_create(twd_gfx_device_t *dev, uint32_t width, uint32_t height, uint32_t format);
int twd_gfx_texture_update(twd_gfx_texture_t *tex, const void *pixels, size_t byte_size);
const void *twd_gfx_texture_data(const twd_gfx_texture_t *tex, size_t *out_size);
void twd_gfx_texture_get_size(const twd_gfx_texture_t *tex, uint32_t *out_w, uint32_t *out_h);
void twd_gfx_texture_destroy(twd_gfx_texture_t *tex);

/* Telltale T3Texture (.d3dtx) parser, BC/DXT software decoder, and GPU upload */
#define TWD_T3_FMT_ARGB8      0x00u
#define TWD_T3_FMT_RGB565     0x02u
#define TWD_T3_FMT_ARGB1555   0x03u
#define TWD_T3_FMT_ARGB4      0x04u
#define TWD_T3_FMT_R16        0x06u
#define TWD_T3_FMT_RG8        0x09u
#define TWD_T3_FMT_RGBA8      0x0au
#define TWD_T3_FMT_A8         0x10u
#define TWD_T3_FMT_L8         0x11u
#define TWD_T3_FMT_AL8        0x12u
#define TWD_T3_FMT_L16        0x13u
#define TWD_T3_FMT_DXT1       0x40u
#define TWD_T3_FMT_DXT3       0x41u
#define TWD_T3_FMT_DXT5       0x42u
#define TWD_T3_FMT_DXT5A      0x43u
#define TWD_T3_FMT_DXN        0x44u
#define TWD_T3_FMT_CTX1       0x45u
#define TWD_T3_FMT_BC6        0x46u
#define TWD_T3_FMT_BC7        0x47u
#define TWD_T3_FMT_R8         0x51u

typedef struct twd_d3dtx_image {
    int32_t version;
    int32_t platform;
    char name[128];
    uint32_t sampler_state;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t mip_levels;
    uint32_t surface_format;
    uint32_t texture_layout;
    uint32_t surface_gamma;
    uint32_t multisample;
    uint32_t resource_usage;
    uint32_t tex_type;
    uint8_t swizzle[4];
    int32_t alpha_mode;
    int32_t color_mode;
    uint32_t region_count;
    uint32_t total_data_size;
    uint32_t mip0_pitch;
    size_t mip0_size;
    uint8_t *mip0_data;
} twd_d3dtx_image_t;

int twd_d3dtx_parse_memory(const void *data, size_t size, twd_d3dtx_image_t *out);
int twd_d3dtx_decode_rgba8(const twd_d3dtx_image_t *img, uint8_t **out_rgba, size_t *out_size);
void twd_d3dtx_free(twd_d3dtx_image_t *img);
twd_gfx_texture_t *twd_d3dtx_upload_gfx_texture(
    twd_gfx_device_t *dev, const void *d3dtx_bytes, size_t d3dtx_size, twd_d3dtx_image_t *out_info);

/* Telltale Font (.font) parser & bitmap glyph blitter */
#define TWD_FONT_MAX_PAGES 8u

typedef struct twd_font_glyph {
    uint32_t codepoint;
    int32_t texture_page;
    int32_t channel;
    float u0, u1, v0, v1;
    float width, height;
    float x_offset, y_offset;
    float x_advance;
} twd_font_glyph_t;

typedef struct twd_font {
    char name[128];
    int is_unicode;
    int is_distance_field;
    int is_filtered;
    int has_ttf_data;
    float height;
    float base;
    float base_point_size;
    uint32_t glyph_count;
    twd_font_glyph_t *glyphs;
    uint32_t page_count;
    uint64_t page_hashes[TWD_FONT_MAX_PAGES];
    const twd_gfx_texture_t *pages[TWD_FONT_MAX_PAGES];
} twd_font_t;

int twd_font_parse_memory(const void *data, size_t size, twd_font_t *out);
void twd_font_free(twd_font_t *font);
const twd_font_glyph_t *twd_font_find_glyph(const twd_font_t *font, uint32_t codepoint);
int twd_font_attach_page(twd_font_t *font, uint32_t page_index, const twd_gfx_texture_t *tex);
float twd_font_draw_text(
    twd_gfx_device_t *dev,
    const twd_font_t *font,
    float x,
    float y,
    float scale,
    const uint8_t rgba[4],
    const char *utf8_text);

void twd_gfx_begin_frame(twd_gfx_device_t *dev);
void twd_gfx_clear(twd_gfx_device_t *dev, const float rgba[4], float depth, uint8_t stencil);
void twd_gfx_set_viewport(twd_gfx_device_t *dev, float x, float y, float w, float h, float min_d, float max_d);
void twd_gfx_draw(twd_gfx_device_t *dev, uint32_t vertex_count, uint32_t start_vertex);
void twd_gfx_draw_indexed(twd_gfx_device_t *dev, uint32_t index_count, uint32_t start_index, int32_t base_vertex);
int twd_gfx_present(twd_gfx_device_t *dev, uint32_t sync_interval);
const uint8_t *twd_gfx_backbuffer_data(
    const twd_gfx_device_t *dev, uint32_t *out_w, uint32_t *out_h, size_t *out_size);
int twd_gfx_draw_rect(
    twd_gfx_device_t *dev, int32_t dx, int32_t dy, int32_t dw, int32_t dh, const uint8_t rgba[4]);
int twd_gfx_draw_line(
    twd_gfx_device_t *dev, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t rgba[4]);
int twd_gfx_draw_sprite(
    twd_gfx_device_t *dev,
    const twd_gfx_texture_t *tex,
    int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    int32_t dx, int32_t dy, int32_t dw, int32_t dh,
    const uint8_t rgba_tint[4]);

typedef struct twd_gfx_vertex2d {
    float x, y, z;
    float u, v;
    uint8_t rgba[4];
} twd_gfx_vertex2d_t;

int twd_gfx_draw_triangle(
    twd_gfx_device_t *dev,
    const twd_gfx_texture_t *tex_or_null,
    const twd_gfx_vertex2d_t v[3]);

/* DXBC -> GLSL ES 3.00 / SPIR-V Shader Translation & Cache */
#define TWD_SHADER_STAGE_VS 0u
#define TWD_SHADER_STAGE_PS 1u
#define TWD_SHADER_STAGE_CS 2u

typedef struct twd_dxbc_info {
    uint64_t hash;
    uint32_t total_bytes;
    uint32_t chunk_count;
    uint32_t stage;
    uint32_t version_major;
    uint32_t version_minor;
    int has_isgn;
    int has_osgn;
    int valid_dxbc;
} twd_dxbc_info_t;

typedef struct twd_shader_cache_entry {
    uint64_t hash;
    uint32_t stage;
    const uint32_t *spirv_words;
    size_t spirv_word_count;
    const char *glsl_source;
} twd_shader_cache_entry_t;

typedef struct twd_shader_cache twd_shader_cache_t;

uint64_t twd_dxbc_hash(const void *bytecode, size_t len);
int twd_dxbc_parse(const void *bytecode, size_t len, twd_dxbc_info_t *out_info);
twd_shader_cache_t *twd_shader_cache_create(void);
void twd_shader_cache_destroy(twd_shader_cache_t *cache);
int twd_shader_cache_put_spirv(
    twd_shader_cache_t *cache, uint64_t hash, uint32_t stage,
    const uint32_t *words, size_t word_count);
int twd_shader_cache_put_glsl(
    twd_shader_cache_t *cache, uint64_t hash, uint32_t stage, const char *glsl);
int twd_shader_cache_register_dxbc(
    twd_shader_cache_t *cache, const void *bytecode, size_t len,
    uint32_t fallback_stage, uint64_t *out_hash);
int twd_shader_cache_lookup(
    const twd_shader_cache_t *cache, uint64_t hash, twd_shader_cache_entry_t *out_entry);
uint32_t twd_shader_cache_count(const twd_shader_cache_t *cache);
void twd_shader_cache_stats(const twd_shader_cache_t *cache, uint64_t *out_hits, uint64_t *out_misses);
twd_shader_cache_t *twd_gfx_get_shader_cache(twd_gfx_device_t *dev);

typedef struct twd_d3d11_buffer_desc {
    uint32_t byte_width;
    uint32_t usage;
    uint32_t bind_flags;
    uint32_t cpu_access_flags;
    uint32_t misc_flags;
    uint32_t structure_byte_stride;
} twd_d3d11_buffer_desc_t;

typedef struct twd_d3d11_tex2d_desc {
    uint32_t width;
    uint32_t height;
    uint32_t mip_levels;
    uint32_t array_size;
    uint32_t format;
    uint32_t sample_count;
    uint32_t sample_quality;
    uint32_t usage;
    uint32_t bind_flags;
    uint32_t cpu_access_flags;
    uint32_t misc_flags;
} twd_d3d11_tex2d_desc_t;

typedef struct twd_d3d11_subresource_data {
    const void *sys_mem;
    uint32_t sys_mem_pitch;
    uint32_t sys_mem_slice_pitch;
} twd_d3d11_subresource_data_t;

typedef struct twd_d3d11_viewport {
    float top_left_x;
    float top_left_y;
    float width;
    float height;
    float min_depth;
    float max_depth;
} twd_d3d11_viewport_t;

typedef struct twd_d3d11_mapped_subresource {
    void *data;
    uint32_t row_pitch;
    uint32_t depth_pitch;
} twd_d3d11_mapped_subresource_t;

typedef struct twd_d3d11_box {
    uint32_t left;
    uint32_t top;
    uint32_t front;
    uint32_t right;
    uint32_t bottom;
    uint32_t back;
} twd_d3d11_box_t;

typedef struct twd_d3d11_rect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} twd_d3d11_rect_t;

int32_t twd_d3d11_create_device_shim(
    twd_window_t *window,
    uint32_t width,
    uint32_t height,
    uint32_t *out_feature_level,
    void **out_device,
    void **out_context,
    void **out_swapchain);
twd_gfx_device_t *twd_d3d11_get_gfx_device(void *d3d11_device);

/* Input / Gamepad / Touch PAL (XInput + DInput8 + RawInput + Pointer) */
#define TWD_MAX_GAMEPADS 4u
#define TWD_PAD_DPAD_UP    0x0001u
#define TWD_PAD_DPAD_DOWN  0x0002u
#define TWD_PAD_DPAD_LEFT  0x0004u
#define TWD_PAD_DPAD_RIGHT 0x0008u
#define TWD_PAD_START      0x0010u
#define TWD_PAD_BACK       0x0020u
#define TWD_PAD_LB         0x0100u
#define TWD_PAD_RB         0x0200u
#define TWD_PAD_A          0x1000u
#define TWD_PAD_B          0x2000u
#define TWD_PAD_X          0x4000u
#define TWD_PAD_Y          0x8000u

typedef struct twd_gamepad_state {
    uint32_t packet_number;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t thumb_lx;
    int16_t thumb_ly;
    int16_t thumb_rx;
    int16_t thumb_ry;
} twd_gamepad_state_t;

typedef struct twd_pointer_state {
    float x;
    float y;
    uint32_t buttons;
    int visible;
} twd_pointer_state_t;

void twd_input_set_gamepad_connected(uint32_t user_index, int connected);
void twd_input_push_gamepad_state(uint32_t user_index, const twd_gamepad_state_t *state);
uint32_t twd_input_get_gamepad_state(uint32_t user_index, twd_gamepad_state_t *out_state);
uint32_t twd_input_set_rumble(uint32_t user_index, uint16_t left_motor, uint16_t right_motor);
void twd_input_get_rumble(uint32_t user_index, uint16_t *out_left, uint16_t *out_right);
uint32_t twd_input_get_gamepad_caps(uint32_t user_index, uint16_t *out_type, uint16_t *out_subtype);
void twd_input_set_pointer(float x, float y, uint32_t buttons);
void twd_input_get_pointer(twd_pointer_state_t *out);
void twd_input_set_key(uint32_t vkey, int down);
int twd_input_key_down(uint32_t vkey);
int32_t twd_dinput8_create_stub(void **out_iface);

/* Audio / Media Foundation Lifecycle PAL (MFPlat + FMOD Focus) */
int32_t twd_media_startup(uint32_t version, uint32_t flags);
int32_t twd_media_shutdown(void);
int twd_media_is_initialized(void);
void twd_audio_set_focus(int active);
int twd_audio_has_focus(void);

/* Save / AppData Directories, COM/KV Registry & Telemetry Stubs */
#define TWD_DIR_SAVES  0
#define TWD_DIR_CONFIG 1
#define TWD_DIR_TEMP   2

int twd_app_data_dir(int kind, char *buf, size_t cap);
int twd_path_is_empty_dir(const char *path);

int32_t twd_com_init(uint32_t coinit_flags);
void twd_com_uninit(void);
int twd_com_is_initialized(void);
void *twd_com_alloc(size_t size);
void *twd_com_realloc(void *ptr, size_t size);
void twd_com_free(void *ptr);

int twd_kv_set(const char *section, const char *key, const char *value);
int twd_kv_get(const char *section, const char *key, char *buf, size_t cap);
int twd_kv_delete(const char *section, const char *key);

int twd_telemetry_enabled(void);
uint32_t twd_wininet_offline_status(void);
void twd_eventlog_stub(const char *source, const char *msg);

/* GUID, COM Memory Stream & Factory Stub (ole32 + OLEAUT32) */
typedef struct twd_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
} twd_guid_t;

typedef struct twd_stream twd_stream_t;

int twd_guid_generate(twd_guid_t *out);
int twd_guid_to_string(const twd_guid_t *g, char *buf, size_t cap);
int twd_guid_from_string(const char *str, twd_guid_t *out);
twd_stream_t *twd_stream_create(const void *init_data, size_t len);
void twd_stream_retain(twd_stream_t *st);
uint32_t twd_stream_release(twd_stream_t *st);
size_t twd_stream_read(twd_stream_t *st, void *dst, size_t len);
size_t twd_stream_write(twd_stream_t *st, const void *src, size_t len);
int64_t twd_stream_seek(twd_stream_t *st, int64_t offset, int whence);
size_t twd_stream_size(const twd_stream_t *st);
int32_t twd_com_create_instance_stub(const twd_guid_t *clsid, void **out_obj);

/* Memory Status, Multi-Wait, File Info & Locale PAL (KERNEL32) */
typedef struct twd_memory_status {
    uint64_t total_phys;
    uint64_t avail_phys;
    uint64_t total_virt;
    uint64_t avail_virt;
} twd_memory_status_t;

int twd_memory_status(twd_memory_status_t *out);
int twd_wait_any(twd_event_t *const *events, uint32_t count, uint32_t timeout_ms);
int twd_file_info(twd_file_t *f, uint64_t *out_size, uint64_t *out_mtime_ns);
uint16_t twd_lang_id(void);

/* Networking / Sockets PAL (WS2_32) */
int twd_net_init(void);
void twd_net_shutdown(void);
int twd_net_last_error(void);
int twd_net_is_initialized(void);

/* 2D Surface, DPI & Paint Invalidation PAL (GDI32 + USER32 + COMCTL32) */
typedef struct twd_surface {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} twd_surface_t;

twd_surface_t *twd_surface_create(uint32_t width, uint32_t height);
void twd_surface_destroy(twd_surface_t *surf);
void twd_surface_fill(twd_surface_t *surf, uint32_t argb);
int twd_surface_blit(twd_surface_t *dst, uint32_t dx, uint32_t dy,
                     const twd_surface_t *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);
void twd_display_dpi(int *out_dpi_x, int *out_dpi_y);
void twd_window_invalidate(twd_window_t *win);
int twd_window_needs_paint(const twd_window_t *win);
void twd_window_clear_paint(twd_window_t *win);

/* Vulkan / EGL / GLES3 dynamic driver availability probe (bitmask: 1=Vulkan, 2=EGL, 4=GLES draw, 8=GLES resources/shaders) */
int twd_gfx_has_driver_symbols(const twd_gfx_device_t *dev);
int twd_gfx_probe_driver(const twd_gfx_device_t *dev, char *out_name, size_t cap);
uint32_t twd_gfx_gpu_count(const twd_gfx_device_t *dev);
int twd_gfx_has_gl_dispatch(const twd_gfx_device_t *dev);
uint32_t twd_gfx_gl_compile_glsl(twd_gfx_device_t *dev, uint32_t stage, const char *glsl_src);

typedef struct twd_dxgi_adapter_desc1 {
    uint16_t description[128];
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t subsys_id;
    uint32_t revision;
    uint64_t dedicated_video_mem;
    uint64_t dedicated_system_mem;
    uint64_t shared_system_mem;
    uint32_t luid_low;
    int32_t luid_high;
    uint32_t flags;
    uint32_t _pad;
} twd_dxgi_adapter_desc1_t;

_Static_assert(sizeof(twd_dxgi_adapter_desc1_t) == 0x138, "DXGI_ADAPTER_DESC1 size");

/* Android GameActivity / NativeActivity Lifecycle, Surface, Assets & Frame Loop */
#define TWD_TOUCH_DOWN   0
#define TWD_TOUCH_UP     1
#define TWD_TOUCH_MOVE   2
#define TWD_TOUCH_CANCEL 3

typedef struct twd_app_callbacks {
    int (*on_init)(void *user);
    void (*on_tick)(void *user, uint64_t dt_ns);
    void (*on_render)(void *user, twd_gfx_device_t *gfx);
    void (*on_shutdown)(void *user);
    void *user;
    uint32_t max_frames;
    void (*on_event)(void *user, const twd_win_event_t *evt);
} twd_app_callbacks_t;

void twd_android_set_native_window(void *native_window);
void *twd_android_get_native_window(void);
void twd_android_set_asset_manager(void *asset_manager);
void *twd_android_get_asset_manager(void);
void twd_android_set_data_dirs(const char *internal_dir, const char *external_dir);
int twd_android_get_data_dir(int external, char *buf, size_t cap);
void twd_android_on_touch(int action, int pointer_id, float x, float y);
void twd_android_on_key(int keycode, int down);
void twd_android_on_motion_axis(uint32_t pad_index, float lx, float ly, float rx, float ry, float lt, float rt, uint16_t buttons);
void twd_android_on_focus(int has_focus);
int twd_android_has_focus(void);
void twd_android_on_resize(twd_window_t *win, twd_gfx_device_t *dev, uint32_t width, uint32_t height);
uint64_t twd_android_run_frame_loop(twd_window_t *win, twd_gfx_device_t *gfx, const twd_app_callbacks_t *cb);
void twd_android_attach_native_activity(void *native_activity, void (*on_bind_hooks)(void));

/* Portable Asset / Archive Loader (save/host FS first, AAssetManager fallback on Android) */
void *twd_asset_read_alloc(const char *rel_path, size_t *out_size);
void twd_asset_free(void *ptr);

/* OodleLZ Compression / Archive Block Loader Bridge (oo2core_5_win64 / liboo2core) */
int twd_oodle_init(const char *library_path_or_null);
void twd_oodle_shutdown(void);
int twd_oodle_is_available(void);
int64_t twd_oodle_decompress(
    const void *comp_buf, size_t comp_len, void *raw_buf, size_t raw_len);

/* TTArchive2 (.ttarch2) Container, Chunk Decompressor & TOC Reader */
#define TWD_TTARCH2_NONE      0u
#define TWD_TTARCH2_NCTT      1u
#define TWD_TTARCH2_ZCTT      2u
#define TWD_TTARCH2_ECTT      3u

#define TWD_TTARCH2_TOC_TTA3  0x54544133u /* "3ATT" LE */
#define TWD_TTARCH2_TOC_TTA4  0x54544134u /* "4ATT" LE */

typedef struct twd_ttarch2_header {
    uint32_t container_kind;
    int encrypted;
    int compressed;
    uint64_t payload_size;
    uint32_t version;
    uint32_t compress_mode;
    uint32_t chunk_size;
    uint32_t chunk_count;
    uint64_t header_size;
} twd_ttarch2_header_t;

typedef struct twd_ttarch2_entry {
    const char *name;
    uint64_t name_crc64;
    uint64_t offset;
    uint32_t size;
    uint32_t flags;
} twd_ttarch2_entry_t;

typedef struct twd_ttarch2_archive twd_ttarch2_archive_t;

typedef int (*twd_ttarch2_decrypt_fn)(
    uint32_t chunk_index, void *buf, size_t len, void *user);

void twd_ttarch2_set_decrypt_callback(twd_ttarch2_decrypt_fn fn, void *user);
int twd_ttarch2_parse_header(const void *data, size_t len, twd_ttarch2_header_t *out);
twd_ttarch2_archive_t *twd_ttarch2_open(const char *path);
twd_ttarch2_archive_t *twd_ttarch2_open_memory(const void *data, size_t len);
void twd_ttarch2_close(twd_ttarch2_archive_t *arch);
uint32_t twd_ttarch2_container_kind(const twd_ttarch2_archive_t *arch);
uint32_t twd_ttarch2_toc_magic(const twd_ttarch2_archive_t *arch);
uint32_t twd_ttarch2_entry_count(const twd_ttarch2_archive_t *arch);
int twd_ttarch2_get_entry(
    const twd_ttarch2_archive_t *arch, uint32_t index, twd_ttarch2_entry_t *out);
int twd_ttarch2_find(
    const twd_ttarch2_archive_t *arch, const char *name, uint32_t *out_index,
    twd_ttarch2_entry_t *out);
int twd_ttarch2_find_crc64(
    const twd_ttarch2_archive_t *arch, uint64_t crc64, uint32_t *out_index,
    twd_ttarch2_entry_t *out);
void *twd_ttarch2_read_entry_alloc(
    twd_ttarch2_archive_t *arch, uint32_t index, size_t *out_size);
void twd_ttarch2_free(void *ptr);

/* Mount/unmount opened .ttarch2 archives as lookup fallback inside twd_asset_read_alloc */
int twd_asset_mount_ttarch2(twd_ttarch2_archive_t *arch);
void twd_asset_unmount_ttarch2(const twd_ttarch2_archive_t *arch);
void twd_asset_unmount_all_ttarch2(void);
int twd_ttarch2_mount_dir(const char *dir_path);
void *twd_ttarch2_lookup_mounted_alloc(const char *rel_path, size_t *out_size);

/* Telltale ECMA-182 CRC64 & MetaStream Header Parser (verified against WalkingDead4.exe) */
#define TWD_METASTREAM_NONE 0u
#define TWD_METASTREAM_MBIN 1u
#define TWD_METASTREAM_MTRE 2u
#define TWD_METASTREAM_MCOM 3u
#define TWD_METASTREAM_MSV4 4u
#define TWD_METASTREAM_MSV5 5u
#define TWD_METASTREAM_MSV6 6u
#define TWD_METASTREAM_MBES 7u

typedef struct twd_metastream_header {
    uint32_t kind;
    uint32_t version;
    uint32_t default_size;
    uint32_t debug_size;
    uint32_t async_size;
    uint32_t type_count;
    uint32_t async_aligned;
    uint64_t payload_offset;
} twd_metastream_header_t;

uint64_t twd_crc64_bytes(uint64_t seed, const void *data, size_t len, int to_lower);
uint64_t twd_crc64(uint64_t seed, const char *str);
int twd_metastream_parse_header(const void *data, size_t len, twd_metastream_header_t *out);

typedef enum twd_prop_kind {
    TWD_PROP_UNKNOWN = 0,
    TWD_PROP_BOOL = 1,
    TWD_PROP_I32 = 2,
    TWD_PROP_U32 = 3,
    TWD_PROP_I64 = 4,
    TWD_PROP_U64 = 5,
    TWD_PROP_F32 = 6,
    TWD_PROP_F64 = 7,
    TWD_PROP_STR = 8,
    TWD_PROP_SYMBOL = 9,
    TWD_PROP_VEC2 = 10,
    TWD_PROP_VEC3 = 11,
    TWD_PROP_VEC4 = 12,
    TWD_PROP_IRECT = 13
} twd_prop_kind_t;

typedef struct twd_prop_entry {
    uint64_t key_crc;
    uint64_t type_crc;
    twd_prop_kind_t kind;
    union {
        int b;
        int32_t i32;
        uint32_t u32;
        int64_t i64;
        uint64_t u64;
        float f32;
        double f64;
        char *str;
        uint64_t sym;
        float vec[4];
        int32_t irect[4];
    } val;
} twd_prop_entry_t;

typedef struct twd_propset {
    int32_t prop_version;
    uint32_t prop_flags;
    uint32_t parent_count;
    uint64_t *parent_crcs;
    uint32_t entry_count;
    twd_prop_entry_t *entries;
    int truncated;
} twd_propset_t;

int twd_propset_parse_stream(const void *data, size_t len, size_t *out_consumed, twd_propset_t *out);
int twd_propset_parse_memory(const void *data, size_t len, twd_propset_t *out);
void twd_propset_free(twd_propset_t *ps);
const twd_prop_entry_t *twd_propset_find(const twd_propset_t *ps, const char *key);
const twd_prop_entry_t *twd_propset_find_crc(const twd_propset_t *ps, uint64_t key_crc);
int twd_propset_get_bool(const twd_propset_t *ps, const char *key, int default_val);
int32_t twd_propset_get_i32(const twd_propset_t *ps, const char *key, int32_t default_val);
uint32_t twd_propset_get_u32(const twd_propset_t *ps, const char *key, uint32_t default_val);
float twd_propset_get_f32(const twd_propset_t *ps, const char *key, float default_val);
const char *twd_propset_get_str(const twd_propset_t *ps, const char *key, const char *default_val);
int twd_propset_get_vec2(const twd_propset_t *ps, const char *key, float *out_x, float *out_y);
int twd_propset_get_vec3(const twd_propset_t *ps, const char *key, float out_xyz[3]);

/* Telltale Scene (.scene) Binary Parser & Active Scene/Agent Manager */
#define TWD_SCENE_MAX_REFS 32u

typedef struct twd_scene_agent {
    char name[128];
    float pos[3];
    int visible;
    char render_mesh[128];
    char ui_text[128];
    twd_propset_t props;
} twd_scene_agent_t;

typedef struct twd_scene {
    char name[128];
    int hidden;
    int active;
    uint32_t ref_scene_count;
    uint64_t ref_scene_hashes[TWD_SCENE_MAX_REFS];
    uint32_t agent_count;
    uint32_t agent_cap;
    twd_scene_agent_t *agents;
} twd_scene_t;

typedef struct twd_scene_mgr twd_scene_mgr_t;

int twd_scene_parse_memory(const void *data, size_t size, twd_scene_t *out);
void twd_scene_free(twd_scene_t *sc);
twd_scene_agent_t *twd_scene_find_agent(twd_scene_t *sc, const char *agent_name);

twd_scene_mgr_t *twd_scene_mgr_create(void);
void twd_scene_mgr_destroy(twd_scene_mgr_t *mgr);
int twd_scene_mgr_open(twd_scene_mgr_t *mgr, const char *scene_name);
int twd_scene_mgr_add(twd_scene_mgr_t *mgr, const char *scene_name);
int twd_scene_mgr_remove(twd_scene_mgr_t *mgr, const char *scene_name);
void twd_scene_mgr_remove_all(twd_scene_mgr_t *mgr);
int twd_scene_mgr_hide(twd_scene_mgr_t *mgr, const char *scene_name, int hidden);
twd_scene_t *twd_scene_mgr_find(twd_scene_mgr_t *mgr, const char *scene_name);
int twd_scene_mgr_add_agent(
    twd_scene_mgr_t *mgr, const char *scene_name, const char *agent_name, const float pos[3]);
twd_scene_agent_t *twd_scene_mgr_find_agent(
    twd_scene_mgr_t *mgr, const char *agent_name, const char *opt_scene_name);
uint32_t twd_scene_mgr_scene_count(const twd_scene_mgr_t *mgr);
uint32_t twd_scene_mgr_total_agent_count(const twd_scene_mgr_t *mgr);
const char *twd_scene_mgr_primary_name(const twd_scene_mgr_t *mgr);

/* Telltale D3DMesh (.d3dmesh / T3MeshData / T3GFXBuffer) parser, VB/IB GPU upload & wire/shaded draw */
#define TWD_D3DMESH_MAX_BATCHES   64u
#define TWD_D3DMESH_MAX_TEXTURES  16u
#define TWD_D3DMESH_MAX_MATERIALS 16u

typedef struct twd_d3dmesh_batch {
    uint32_t base_index;
    uint32_t start_index;
    uint32_t num_primitives;
    uint32_t num_indices;
    int32_t material_index;
} twd_d3dmesh_batch_t;

typedef struct twd_d3dmesh {
    char name[128];
    int32_t version;
    float lightmap_scale;
    uint64_t lod_param_crc;
    uint32_t lod_count;
    uint32_t vertex_count;
    uint32_t ib_or_vb_verts;
    uint32_t vertex_stride;
    uint32_t vertex_stream_mask;
    float bbox_minmax[6];
    float bsphere_xyzr[4];
    float pos_scale[3];
    float pos_offset[3];
    uint32_t batch_count;
    twd_d3dmesh_batch_t *batches;
    uint32_t texture_count;
    char textures[TWD_D3DMESH_MAX_TEXTURES][128];
    uint64_t texture_crcs[TWD_D3DMESH_MAX_TEXTURES];
    uint32_t material_count;
    char materials[TWD_D3DMESH_MAX_MATERIALS][128];
    uint8_t *vertex_data;
    size_t vertex_bytes;
    uint32_t index_count;
    uint32_t index_stride;
    uint8_t *index_data;
    size_t index_bytes;
} twd_d3dmesh_t;

typedef struct twd_d3dmesh_gpu {
    twd_gfx_buffer_t *vb;
    twd_gfx_buffer_t *ib;
    uint32_t vertex_count;
    uint32_t vertex_stride;
    uint32_t index_count;
    uint32_t index_stride;
} twd_d3dmesh_gpu_t;

int twd_d3dmesh_parse_memory(const void *data, size_t size, twd_d3dmesh_t *out);
void twd_d3dmesh_free(twd_d3dmesh_t *mesh);
int twd_d3dmesh_upload_gfx(
    twd_gfx_device_t *dev, const twd_d3dmesh_t *mesh, twd_d3dmesh_gpu_t *out_gpu);
void twd_d3dmesh_gpu_free(twd_d3dmesh_gpu_t *gpu);
int twd_d3dmesh_draw_wire(
    twd_gfx_device_t *dev,
    const twd_d3dmesh_t *mesh,
    float cx, float cy, float scale,
    const uint8_t rgba[4]);
int twd_d3dmesh_draw_shaded(
    twd_gfx_device_t *dev,
    const twd_d3dmesh_t *mesh,
    const twd_gfx_texture_t *tex_or_null,
    float cx, float cy, float scale,
    const uint8_t rgba[4]);

/* --- Lua 5.2 script chunk unwrapper & lightweight boot VM --- */
typedef enum twd_lua_chunk_kind {
    TWD_LUA_CHUNK_UNKNOWN = 0,
    TWD_LUA_CHUNK_TEXT = 1,
    TWD_LUA_CHUNK_BYTECODE = 2,
    TWD_LUA_CHUNK_LEN = 3,
    TWD_LUA_CHUNK_LEO = 4
} twd_lua_chunk_kind_t;

typedef enum twd_lua_val_kind {
    TWD_LUA_T_NIL = 0,
    TWD_LUA_T_BOOL = 1,
    TWD_LUA_T_NUM = 3,
    TWD_LUA_T_STR = 4,
    TWD_LUA_T_TABLE = 5,
    TWD_LUA_T_CFUNC = 6
} twd_lua_val_kind_t;

typedef struct twd_lua_vm twd_lua_vm_t;

typedef struct twd_lua_val {
    twd_lua_val_kind_t kind;
    union {
        int b;
        float num;
        char *str;
        void *ptr;
    } u;
} twd_lua_val_t;

typedef int (*twd_lua_decrypt_fn)(void *buf, size_t len, void *user);
typedef int (*twd_lua_cfunc_t)(twd_lua_vm_t *vm,
                               void *user,
                               int nargs,
                               const twd_lua_val_t *args,
                               twd_lua_val_t *out_ret);

void twd_lua_set_decrypt_callback(twd_lua_decrypt_fn fn, void *user);
int twd_lua_unwrap_chunk(const void *data,
                         size_t len,
                         twd_lua_chunk_kind_t *out_kind,
                         uint8_t **out_buf,
                         size_t *out_len);
void twd_lua_free_buf(void *ptr);

twd_lua_vm_t *twd_lua_vm_create(void);
void twd_lua_vm_destroy(twd_lua_vm_t *vm);
int twd_lua_vm_register(twd_lua_vm_t *vm, const char *name, twd_lua_cfunc_t fn, void *user);
int twd_lua_vm_set_global_str(twd_lua_vm_t *vm, const char *name, const char *val);
int twd_lua_vm_set_global_num(twd_lua_vm_t *vm, const char *name, float val);
int twd_lua_vm_set_global_bool(twd_lua_vm_t *vm, const char *name, int val);
const twd_lua_val_t *twd_lua_vm_get_global(const twd_lua_vm_t *vm, const char *name);
int twd_lua_vm_exec_buffer(twd_lua_vm_t *vm, const void *data, size_t len, const char *chunk_name);
int twd_lua_vm_exec_vfs(twd_lua_vm_t *vm, const char *rel_path);
uint32_t twd_lua_vm_executed_count(const twd_lua_vm_t *vm);
twd_lua_chunk_kind_t twd_lua_vm_last_chunk_kind(const twd_lua_vm_t *vm);

/* Audio & FMOD Studio / Core PAL (libfmodstudio/libfmod bridge + software PCM16 mixer) */
#define TWD_AUDIO_CAP_FMOD_STUDIO 1
#define TWD_AUDIO_CAP_FMOD_CORE   2
#define TWD_AUDIO_CAP_OS_OUTPUT   4
#define TWD_AUDIO_CAP_MIXER       8

#define TWD_AUDIO_STOPPED         0
#define TWD_AUDIO_PLAYING         1
#define TWD_AUDIO_PAUSED          2

#define TWD_AUDIO_LOAD_UNLOADED   0
#define TWD_AUDIO_LOAD_LOADED     3

typedef struct twd_audio_system twd_audio_system_t;
typedef struct twd_audio_bank twd_audio_bank_t;
typedef struct twd_audio_bus twd_audio_bus_t;
typedef struct twd_audio_event_desc twd_audio_event_desc_t;
typedef struct twd_audio_event_instance twd_audio_event_instance_t;
typedef struct twd_audio_sound twd_audio_sound_t;
typedef struct twd_audio_channel twd_audio_channel_t;

int twd_audio_init(const char *studio_lib_or_null, const char *core_lib_or_null);
void twd_audio_shutdown(void);
int twd_audio_backend_caps(void);

twd_audio_system_t *twd_audio_system_create(void);
int twd_audio_system_initialize(
    twd_audio_system_t *sys, uint32_t max_channels, uint32_t sample_rate_hz, uint32_t flags);
void twd_audio_system_release(twd_audio_system_t *sys);
int twd_audio_system_mixer_suspend(twd_audio_system_t *sys);
int twd_audio_system_mixer_resume(twd_audio_system_t *sys);
int twd_audio_system_is_suspended(const twd_audio_system_t *sys);
int twd_audio_system_set_listener(twd_audio_system_t *sys, float x, float y, float z);
int twd_audio_system_update(twd_audio_system_t *sys, uint32_t dt_ms);

twd_audio_bank_t *twd_audio_load_bank_memory(
    twd_audio_system_t *sys, const char *name, const void *data, size_t len);
twd_audio_bank_t *twd_audio_load_bank_vfs(twd_audio_system_t *sys, const char *rel_path);
int twd_audio_bank_unload(twd_audio_bank_t *bank);
int twd_audio_bank_is_valid(const twd_audio_bank_t *bank);
uint32_t twd_audio_bank_count(const twd_audio_system_t *sys);

twd_audio_bus_t *twd_audio_get_bus(twd_audio_system_t *sys, const char *bus_path);
int twd_audio_bus_set_volume(twd_audio_bus_t *bus, float volume);
float twd_audio_bus_get_volume(const twd_audio_bus_t *bus);
int twd_audio_bus_set_mute(twd_audio_bus_t *bus, int muted);
int twd_audio_bus_set_paused(twd_audio_bus_t *bus, int paused);

twd_audio_event_desc_t *twd_audio_register_event(
    twd_audio_system_t *sys, const char *path, uint32_t length_ms, int oneshot,
    const char *bus_path_or_null);
twd_audio_event_desc_t *twd_audio_get_event(twd_audio_system_t *sys, const char *path);
twd_audio_event_instance_t *twd_audio_create_instance(
    twd_audio_system_t *sys, twd_audio_event_desc_t *desc);
void twd_audio_instance_release(twd_audio_event_instance_t *inst);
int twd_audio_instance_start(twd_audio_event_instance_t *inst);
int twd_audio_instance_stop(twd_audio_event_instance_t *inst, int immediate);
int twd_audio_instance_set_paused(twd_audio_event_instance_t *inst, int paused);
int twd_audio_instance_set_volume(twd_audio_event_instance_t *inst, float volume);
int twd_audio_instance_set_timeline_ms(twd_audio_event_instance_t *inst, uint32_t ms);
uint32_t twd_audio_instance_get_timeline_ms(const twd_audio_event_instance_t *inst);
int twd_audio_instance_get_state(const twd_audio_event_instance_t *inst);
int twd_audio_instance_set_parameter(
    twd_audio_event_instance_t *inst, const char *name, float value);
int twd_audio_instance_get_parameter(
    const twd_audio_event_instance_t *inst, const char *name, float *out_val);
void twd_audio_instance_set_3d(
    twd_audio_event_instance_t *inst, float x, float y, float z);
void twd_audio_instance_set_userdata(twd_audio_event_instance_t *inst, void *user);
void *twd_audio_instance_get_userdata(const twd_audio_event_instance_t *inst);

twd_audio_sound_t *twd_audio_create_sound_pcm16(
    twd_audio_system_t *sys, const int16_t *samples, uint32_t num_frames,
    uint32_t channels, uint32_t sample_rate, int loop);
void twd_audio_sound_release(twd_audio_sound_t *sound);
twd_audio_channel_t *twd_audio_play_sound(
    twd_audio_system_t *sys, twd_audio_sound_t *sound, twd_audio_bus_t *bus, int paused);
int twd_audio_channel_stop(twd_audio_channel_t *ch);
int twd_audio_channel_set_paused(twd_audio_channel_t *ch, int paused);
int twd_audio_channel_set_volume(twd_audio_channel_t *ch, float volume);
int twd_audio_channel_set_pan(twd_audio_channel_t *ch, float pan);
int twd_audio_channel_set_pitch(twd_audio_channel_t *ch, float pitch);
int twd_audio_channel_is_playing(const twd_audio_channel_t *ch);
uint32_t twd_audio_mix_pcm_i16(
    twd_audio_system_t *sys, int16_t *out_stereo_i16, uint32_t num_frames);

/* Video Playback PAL (TTMediaEngine64 / TTIMFMediaEngine replacement) */
typedef struct twd_video_player twd_video_player_t;

twd_video_player_t *twd_video_create(void *gfx_device_or_null);
void twd_video_destroy(twd_video_player_t *vp);
int twd_video_set_source(twd_video_player_t *vp, const char *path);
int twd_video_play(twd_video_player_t *vp);
int twd_video_pause(twd_video_player_t *vp);
int twd_video_set_volume(twd_video_player_t *vp, double volume);
int twd_video_set_time(twd_video_player_t *vp, double seconds);
double twd_video_get_time(const twd_video_player_t *vp);
double twd_video_get_duration(const twd_video_player_t *vp);
void twd_video_get_size(const twd_video_player_t *vp, uint32_t *out_w, uint32_t *out_h);
int twd_video_tick(twd_video_player_t *vp, int64_t *out_pts_ns);
int twd_video_transfer_frame(twd_video_player_t *vp, void *dst_surface_or_tex);

void twd_register_hook(uint64_t address, const char *name, void *replacement);
const twd_hook_entry_t *twd_find_hook(uint64_t address);
const twd_hook_entry_t *twd_hook_at(uint32_t index);
uint32_t twd_hook_count(void);

/* Persistent Save / Load Manager (saveloadmanager.cpp @ 0x140222410) */
#define TWD_SAVE_MAX_PROPS 128u

typedef struct twd_save_mgr twd_save_mgr_t;

twd_save_mgr_t *twd_save_mgr_create(const char *save_dir_or_null);
void twd_save_mgr_destroy(twd_save_mgr_t *mgr);
int twd_save_mgr_get_dir(const twd_save_mgr_t *mgr, char *buf, size_t cap);
void twd_save_clear_props(twd_save_mgr_t *mgr);
int twd_save_set_display_name(twd_save_mgr_t *mgr, const char *display_name);
int twd_save_set_active_scene(twd_save_mgr_t *mgr, const char *scene_name);
int twd_save_set_prop_bool(twd_save_mgr_t *mgr, const char *key, int val);
int twd_save_set_prop_i32(twd_save_mgr_t *mgr, const char *key, int32_t val);
int twd_save_set_prop_f32(twd_save_mgr_t *mgr, const char *key, float val);
int twd_save_set_prop_str(twd_save_mgr_t *mgr, const char *key, const char *val);
int twd_save_get_prop_bool(const twd_save_mgr_t *mgr, const char *key, int default_val);
int32_t twd_save_get_prop_i32(const twd_save_mgr_t *mgr, const char *key, int32_t default_val);
float twd_save_get_prop_f32(const twd_save_mgr_t *mgr, const char *key, float default_val);
const char *twd_save_get_prop_str(const twd_save_mgr_t *mgr, const char *key, const char *default_val);
int twd_save_game(twd_save_mgr_t *mgr, const char *slot_or_null);
int twd_load_game(twd_save_mgr_t *mgr, const char *slot_or_null);
int twd_save_exists(const twd_save_mgr_t *mgr, const char *slot_or_null);
int twd_save_is_corrupt(const twd_save_mgr_t *mgr, const char *slot_or_null);
int twd_save_delete(twd_save_mgr_t *mgr, const char *slot_or_null);
int twd_save_new_game(twd_save_mgr_t *mgr, uint32_t slot_index);
int twd_save_reset_game(twd_save_mgr_t *mgr);
int twd_save_is_saving(const twd_save_mgr_t *mgr);
void twd_save_get_metadata(
    const twd_save_mgr_t *mgr,
    char *out_slot, size_t slot_cap,
    char *out_display, size_t disp_cap,
    char *out_scene, size_t scene_cap,
    int32_t *out_seq);

#ifdef __cplusplus
}
#endif

#endif /* TWD_RUNTIME_H */
