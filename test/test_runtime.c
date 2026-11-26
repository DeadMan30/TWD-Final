/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TWD_CHECK(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "test_runtime failed at %s:%d: %s\n",           \
                    __FILE__, __LINE__, #cond);                             \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static int dummy_fn(void) {
    return 42;
}

typedef struct thread_test_ctx {
    twd_sem_t *sem;
    int value;
} thread_test_ctx_t;

typedef struct loop_test_ctx {
    int inited;
    int ticks;
    int renders;
    int shutdown;
} loop_test_ctx_t;

static int test_loop_init(void *user) {
    ((loop_test_ctx_t *)user)->inited = 1;
    return 0;
}

static void test_loop_tick(void *user, uint64_t dt_ns) {
    (void)dt_ns;
    ((loop_test_ctx_t *)user)->ticks++;
}

static void test_loop_render(void *user, twd_gfx_device_t *gfx) {
    (void)gfx;
    ((loop_test_ctx_t *)user)->renders++;
}

static void test_loop_shutdown(void *user) {
    ((loop_test_ctx_t *)user)->shutdown = 1;
}

static int g_attach_hook_ran = 0;
static void test_attach_hook_cb(void) {
    g_attach_hook_ran = 1;
}

static int worker_thread_fn(void *arg) {
    thread_test_ctx_t *ctx = (thread_test_ctx_t *)arg;
    twd_sem_post(ctx->sem, 1);
    return ctx->value + 8;
}

static void ttarch_wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void ttarch_wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void ttarch_wr_u64(uint8_t *p, uint64_t v) {
    ttarch_wr_u32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    ttarch_wr_u32(p + 4u, (uint32_t)((v >> 32) & 0xFFFFFFFFULL));
}

static int synthetic_xor_decrypt(uint32_t chunk_index, void *buf, size_t len, void *user) {
    uint8_t key = (uint8_t)((uintptr_t)user ^ (uintptr_t)chunk_index);
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0u; i < len; ++i) p[i] ^= key;
    return 0;
}

int main(void) {
    TWD_CHECK(twd_platform_init() == 0);
    uint64_t t0 = twd_time_now_ns();
    uint64_t t1 = twd_time_now_ns();
    TWD_CHECK(t1 >= t0);

    twd_register_hook(0x14036f0d0ULL, "engine_qpc_scale", (void *)&dummy_fn);
    TWD_CHECK(twd_hook_count() == 1);
    const twd_hook_entry_t *e = twd_find_hook(0x14036f0d0ULL);
    TWD_CHECK(e != NULL);
    TWD_CHECK(e->replacement == (void *)&dummy_fn);
    TWD_CHECK(twd_find_hook(0x140000000ULL) == NULL);

    TWD_CHECK(twd_qpc_freq() > 0);
    uint64_t q0 = twd_qpc_now();
    uint32_t ms0 = twd_ticks_ms();
    twd_sleep_ms(1);
    twd_thread_yield();
    uint64_t q1 = twd_qpc_now();
    TWD_CHECK(q1 >= q0);
    (void)ms0;

    TWD_CHECK(twd_pid() != 0);
    TWD_CHECK(twd_tid() != 0);

    twd_mutex_t mtx;
    TWD_CHECK(twd_mutex_init(&mtx) == 0);
    twd_mutex_lock(&mtx);
    twd_mutex_lock(&mtx); /* verify recursive acquisition */
    twd_mutex_unlock(&mtx);
    twd_mutex_unlock(&mtx);
    twd_mutex_destroy(&mtx);

    uint32_t tls_key = twd_tls_alloc();
    TWD_CHECK(tls_key != TWD_TLS_INVALID);
    TWD_CHECK(twd_tls_get(tls_key) == NULL);
    int sentinel = 12345;
    TWD_CHECK(twd_tls_set(tls_key, &sentinel) == 0);
    TWD_CHECK(twd_tls_get(tls_key) == &sentinel);
    TWD_CHECK(twd_tls_free(tls_key) == 0);

    twd_debug_output("twd runtime selftest\n");

    twd_set_last_error(0x1234u);
    TWD_CHECK(twd_last_error() == 0x1234u);

    uint8_t *vm = (uint8_t *)twd_vm_alloc(4096, 0);
    TWD_CHECK(vm != NULL);
    vm[0] = 0x42u;
    vm[4095] = 0x99u;
    TWD_CHECK(twd_vm_protect(vm, 4096, 0, 0) == 0);
    TWD_CHECK(vm[0] == 0x42u);
    twd_vm_free(vm, 4096);

    char *hp = (char *)twd_heap_alloc(16);
    TWD_CHECK(hp != NULL);
    memcpy(hp, "twd", 4);
    hp = (char *)twd_heap_realloc(hp, 64);
    TWD_CHECK(hp != NULL && strcmp(hp, "twd") == 0);
    twd_heap_free(hp);

    twd_event_t ev;
    TWD_CHECK(twd_event_init(&ev, 0, 0) == 0);
    TWD_CHECK(twd_event_wait(&ev, 0) == TWD_WAIT_TIMEOUT);
    TWD_CHECK(twd_event_set(&ev) == 0);
    TWD_CHECK(twd_event_wait(&ev, 100) == TWD_WAIT_OK);
    TWD_CHECK(twd_event_wait(&ev, 0) == TWD_WAIT_TIMEOUT); /* auto-reset check */
    TWD_CHECK(twd_event_set(&ev) == 0);
    TWD_CHECK(twd_event_reset(&ev) == 0);
    TWD_CHECK(twd_event_wait(&ev, 0) == TWD_WAIT_TIMEOUT);
    twd_event_destroy(&ev);

    const char *tmp_file = "twd_pal_test.tmp";
    twd_file_t tf;
    TWD_CHECK(twd_file_open(tmp_file, TWD_FILE_OPEN_READ | TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &tf) == 0);
    TWD_CHECK(twd_file_write(&tf, "TWD4", 4) == 4);
    TWD_CHECK(twd_file_flush(&tf) == 0);
    TWD_CHECK(twd_file_seek(&tf, 0, TWD_SEEK_SET) == 0);
    char rbuf[5] = {0};
    TWD_CHECK(twd_file_read(&tf, rbuf, 4) == 4);
    TWD_CHECK(memcmp(rbuf, "TWD4", 4) == 0);
    TWD_CHECK(twd_file_close(&tf) == 0);
    TWD_CHECK(twd_file_delete(tmp_file) == 0);

    uint16_t u16[16];
    char u8[32];
    int n16 = twd_utf8_to_utf16("TWD\xE2\x9C\x93", -1, u16, 16);
    TWD_CHECK(n16 == 5 && u16[3] == 0x2713u && u16[4] == 0u);
    int n8 = twd_utf16_to_utf8(u16, -1, u8, 32);
    TWD_CHECK(n8 == 7 && strcmp(u8, "TWD\xE2\x9C\x93") == 0);

    char exe_buf[512];
    TWD_CHECK(twd_exe_path(exe_buf, sizeof(exe_buf)) > 0);

    twd_sem_t sem;
    TWD_CHECK(twd_sem_init(&sem, 0, 4) == 0);
    TWD_CHECK(twd_sem_wait(&sem, 0) == TWD_WAIT_TIMEOUT);

    thread_test_ctx_t tctx;
    tctx.sem = &sem;
    tctx.value = 34;
    twd_thread_t th;
    TWD_CHECK(twd_thread_create(&th, worker_thread_fn, &tctx) == 0);
    TWD_CHECK(twd_sem_wait(&sem, 5000) == TWD_WAIT_OK);
    int th_rc = 0;
    TWD_CHECK(twd_thread_join(&th, &th_rc) == 0);
    TWD_CHECK(th_rc == 42);
    twd_sem_destroy(&sem);

    void *self_mod = twd_dynlib_open(NULL);
    TWD_CHECK(self_mod != NULL);
    twd_dynlib_close(self_mod);

    TWD_CHECK(twd_alert_box("TWD-Final", "alert test", 0u) == TWD_ALERT_OK);
    TWD_CHECK(twd_alert_box("TWD-Final", "yesno test", 4u) == TWD_ALERT_YES);

    twd_window_t *win = twd_window_create("TWD4 Test Window", 10, 20, 1280, 720, 0u);
    TWD_CHECK(win != NULL && twd_window_is_valid(win) == 1);
    twd_window_show(win, 1);
    TWD_CHECK(win->visible == 1);
    twd_window_set_rect(win, 0, 0, 1920, 1080);
    int cw = 0, ch = 0;
    twd_window_get_client_rect(win, &cw, &ch);
    TWD_CHECK(cw == 1920 && ch == 1080);
    char wtitle[64];
    TWD_CHECK(twd_window_get_title(win, wtitle, sizeof(wtitle)) > 0);
    TWD_CHECK(strcmp(wtitle, "TWD4 Test Window") == 0);

    twd_win_event_t ev_in;
    ev_in.kind = TWD_EVT_KEY;
    ev_in.window_id = (uintptr_t)win;
    ev_in.param1 = 0x41u;
    ev_in.param2 = 1;
    twd_window_push_event(&ev_in);
    twd_win_event_t ev_out;
    TWD_CHECK(twd_window_poll_event(&ev_out) == 1);
    TWD_CHECK(ev_out.kind == TWD_EVT_KEY && ev_out.param1 == 0x41u);
    twd_post_quit(7);
    TWD_CHECK(twd_window_poll_event(&ev_out) == 1 && ev_out.kind == TWD_EVT_QUIT);
    int qcode = 0;
    TWD_CHECK(twd_quit_requested(&qcode) == 1 && qcode == 7);
    twd_window_destroy(win);
    TWD_CHECK(twd_window_is_valid(win) == 0);

    TWD_CHECK(twd_system_metric(TWD_METRIC_SCREEN_W) > 0);
    TWD_CHECK(twd_system_metric(TWD_METRIC_SCREEN_H) > 0);
    TWD_CHECK(twd_cpu_count() >= 1u);
    TWD_CHECK(twd_page_size() >= 4096u);
    TWD_CHECK(twd_unix_time_ns() > 1000000000000000000ULL);
    TWD_CHECK(twd_timer_begin_period(1u) == 0);
    TWD_CHECK(twd_timer_end_period(1u) == 0);

    uint8_t ic_buf[16];
    memset(ic_buf, 0x90, sizeof(ic_buf));
    twd_icache_flush(ic_buf, sizeof(ic_buf));

    char cwd_buf[512];
    TWD_CHECK(twd_getcwd(cwd_buf, sizeof(cwd_buf)) > 0);
    TWD_CHECK(twd_setenv("TWD_PAL_ENV_TEST", "ok42") == 0);
    char env_buf[32];
    TWD_CHECK(twd_getenv("TWD_PAL_ENV_TEST", env_buf, sizeof(env_buf)) == 4);
    TWD_CHECK(strcmp(env_buf, "ok42") == 0);

    const uint16_t u16_a[] = {'A', 'b', 'c', 0};
    const uint16_t u16_b[] = {'a', 'B', 'C', 0};
    TWD_CHECK(twd_utf16_len(u16_a) == 3u);
    TWD_CHECK(twd_utf16_cmp(u16_a, u16_b, 0) != 0);
    TWD_CHECK(twd_utf16_cmp(u16_a, u16_b, 1) == 0);

    TWD_CHECK(twd_mkdir("twd_pal_dir_tmp") == 0);
    TWD_CHECK(twd_path_is_dir("twd_pal_dir_tmp") == 1);
    twd_file_t df;
    TWD_CHECK(twd_file_open("twd_pal_dir_tmp/a.bin", TWD_FILE_OPEN_READ | TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &df) == 0);
    TWD_CHECK(twd_file_write(&df, "ABCDEFGH", 8) == 8);
    TWD_CHECK(twd_file_truncate(&df, 4) == 0);
    TWD_CHECK(twd_file_close(&df) == 0);
    TWD_CHECK(twd_file_rename("twd_pal_dir_tmp/a.bin", "twd_pal_dir_tmp/b.bin") == 0);
    TWD_CHECK(twd_file_exists("twd_pal_dir_tmp/b.bin") == 1);
    twd_dir_t dir;
    TWD_CHECK(twd_dir_open("twd_pal_dir_tmp", &dir) == 0);
    twd_dir_entry_t dent;
    int saw_b = 0;
    while (twd_dir_next(&dir, &dent) > 0) {
        if (strcmp(dent.name, "b.bin") == 0) saw_b = 1;
    }
    twd_dir_close(&dir);
    TWD_CHECK(saw_b == 1);
    TWD_CHECK(twd_file_delete("twd_pal_dir_tmp/b.bin") == 0);
    TWD_CHECK(twd_rmdir("twd_pal_dir_tmp") == 0);

    twd_alert("TWD-Final", "runtime alert check");

    /* Graphics PAL + D3D11 -> Vulkan/GLES COM shim */
    TWD_CHECK(twd_gfx_select_backend(TWD_GFX_BACKEND_VULKAN) == TWD_GFX_BACKEND_VULKAN);
    twd_window_t *gwin = twd_window_create("TWD4 Gfx", 0, 0, 1280, 720, 0u);
    TWD_CHECK(gwin != NULL);
    uint32_t feat = 0u;
    void *d3d_dev = NULL;
    void *d3d_ctx = NULL;
    void *dxgi_sw = NULL;
    TWD_CHECK(twd_d3d11_create_device_shim(gwin, 0u, 0u, &feat, &d3d_dev, &d3d_ctx, &dxgi_sw) == 0);
    TWD_CHECK(feat == TWD_D3D11_FEATURE_LEVEL_11_0);
    TWD_CHECK(d3d_dev != NULL && d3d_ctx != NULL && dxgi_sw != NULL);

    void *const *dev_vtbl = *(void *const **)d3d_dev;
    void *const *ctx_vtbl = *(void *const **)d3d_ctx;
    void *const *sw_vtbl  = *(void *const **)dxgi_sw;

    typedef uint32_t (*fn_get_fl_t)(void *);
    typedef void (*fn_get_ctx_t)(void *, void **);
    typedef int32_t (*fn_create_buf_t)(void *, const twd_d3d11_buffer_desc_t *,
                                       const twd_d3d11_subresource_data_t *, void **);
    typedef int32_t (*fn_create_tex_t)(void *, const twd_d3d11_tex2d_desc_t *,
                                       const twd_d3d11_subresource_data_t *, void **);
    typedef int32_t (*fn_create_rtv_t)(void *, const void *, const void *, void **);
    typedef void (*fn_clear_rtv_t)(void *, void *, const float[4]);
    typedef void (*fn_set_vp_t)(void *, uint32_t, const twd_d3d11_viewport_t *);
    typedef void (*fn_draw_t)(void *, uint32_t, uint32_t);
    typedef int32_t (*fn_present_t)(void *, uint32_t, uint32_t);
    typedef int32_t (*fn_resize_t)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    typedef uint32_t (*fn_release_t)(void *);

    TWD_CHECK(((fn_get_fl_t)dev_vtbl[37])(d3d_dev) == TWD_D3D11_FEATURE_LEVEL_11_0);
    void *ctx2 = NULL;
    ((fn_get_ctx_t)dev_vtbl[40])(d3d_dev, &ctx2);
    TWD_CHECK(ctx2 == d3d_ctx);
    TWD_CHECK(((fn_release_t)ctx_vtbl[2])(ctx2) >= 1u);

    twd_d3d11_buffer_desc_t bdesc;
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.byte_width = 256u;
    bdesc.bind_flags = 1u;
    void *vbuf = NULL;
    TWD_CHECK(((fn_create_buf_t)dev_vtbl[3])(d3d_dev, &bdesc, NULL, &vbuf) == 0 && vbuf != NULL);

    twd_d3d11_tex2d_desc_t tdesc;
    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = 64u;
    tdesc.height = 64u;
    tdesc.format = 28u;
    void *tex = NULL;
    void *tex2 = NULL;
    void *rtv = NULL;
    TWD_CHECK(((fn_create_tex_t)dev_vtbl[5])(d3d_dev, &tdesc, NULL, &tex) == 0 && tex != NULL);
    TWD_CHECK(((fn_create_tex_t)dev_vtbl[5])(d3d_dev, &tdesc, NULL, &tex2) == 0 && tex2 != NULL);
    TWD_CHECK(((fn_create_rtv_t)dev_vtbl[9])(d3d_dev, tex, NULL, &rtv) == 0 && rtv != NULL);

    typedef int32_t (*fn_map_t)(void *, void *, uint32_t, uint32_t, uint32_t, twd_d3d11_mapped_subresource_t *);
    typedef void (*fn_unmap_t)(void *, void *, uint32_t);
    typedef void (*fn_update_sub_t)(void *, void *, uint32_t, const twd_d3d11_box_t *, const void *, uint32_t, uint32_t);
    typedef void (*fn_copy_res_t)(void *, void *, void *);
    typedef void (*fn_set_topo_t)(void *, uint32_t);
    typedef void (*fn_set_rtvs_t)(void *, uint32_t, void *const *, void *);
    typedef void (*fn_set_scissor_t)(void *, uint32_t, const twd_d3d11_rect_t *);
    typedef void (*fn_draw_idx_t)(void *, uint32_t, uint32_t, int32_t);

    twd_d3d11_mapped_subresource_t mres;
    TWD_CHECK(((fn_map_t)ctx_vtbl[14])(d3d_ctx, vbuf, 0u, 4u, 0u, &mres) == 0 && mres.data != NULL);
    memcpy(mres.data, "VTX0", 4u);
    ((fn_unmap_t)ctx_vtbl[15])(d3d_ctx, vbuf, 0u);

    uint32_t row0[64];
    for (uint32_t i = 0; i < 64u; i++) row0[i] = 0xdeadbeefu;
    uint32_t *pix_upload = (uint32_t *)malloc(64u * 64u * 4u);
    TWD_CHECK(pix_upload != NULL);
    for (uint32_t y = 0; y < 64u; y++) memcpy(pix_upload + (size_t)y * 64u, row0, sizeof(row0));
    ((fn_update_sub_t)ctx_vtbl[48])(d3d_ctx, tex, 0u, NULL, pix_upload, 64u * 4u, 0u);
    free(pix_upload);
    ((fn_copy_res_t)ctx_vtbl[47])(d3d_ctx, tex2, tex);
    TWD_CHECK(((fn_map_t)ctx_vtbl[14])(d3d_ctx, tex2, 0u, 1u, 0u, &mres) == 0 && mres.data != NULL);
    TWD_CHECK(((const uint32_t *)mres.data)[0] == 0xdeadbeefu && mres.row_pitch == 64u * 4u);
    ((fn_unmap_t)ctx_vtbl[15])(d3d_ctx, tex2, 0u);

    typedef int32_t (*fn_create_sh_t)(void *, const void *, size_t, const void *, void **);
    typedef void (*fn_set_sh_t)(void *, void *, const void *const *, uint32_t);
    /* Minimal valid DXBC container: magic 'DXBC', 16B checksum, version 1, total=56, chunks=1, off[0]=36, chunk 'SHDR' len=12, tok=VS SM5.0 (0x00010050) */
    static const uint8_t sample_dxbc_vs[56] = {
        'D','X','B','C', 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        1,0,0,0, 56,0,0,0, 1,0,0,0, 36,0,0,0,
        'S','H','D','R', 12,0,0,0, 0x50,0x00,0x01,0x00, 3,0,0,0, 0x0e,0x00,0x00,0x3e
    };
    twd_dxbc_info_t dinfo;
    TWD_CHECK(twd_dxbc_parse(sample_dxbc_vs, sizeof(sample_dxbc_vs), &dinfo) == 1);
    TWD_CHECK(dinfo.valid_dxbc == 1 && dinfo.stage == TWD_SHADER_STAGE_VS && dinfo.version_major == 5u);

    void *vs_obj = NULL, *vs_dup = NULL, *ps_obj = NULL;
    TWD_CHECK(((fn_create_sh_t)dev_vtbl[12])(d3d_dev, sample_dxbc_vs, sizeof(sample_dxbc_vs), NULL, &vs_obj) == 0 && vs_obj != NULL);
    TWD_CHECK(((fn_create_sh_t)dev_vtbl[12])(d3d_dev, sample_dxbc_vs, sizeof(sample_dxbc_vs), NULL, &vs_dup) == 0 && vs_dup != NULL);
    static const uint8_t sample_ps_blob[8] = {'P','S','_','T','E','S','T',0};
    TWD_CHECK(((fn_create_sh_t)dev_vtbl[15])(d3d_dev, sample_ps_blob, sizeof(sample_ps_blob), NULL, &ps_obj) == 0 && ps_obj != NULL);
    ((fn_set_sh_t)ctx_vtbl[11])(d3d_ctx, vs_obj, NULL, 0u);
    ((fn_set_sh_t)ctx_vtbl[9])(d3d_ctx, ps_obj, NULL, 0u);

    twd_shader_cache_t *scache = twd_gfx_get_shader_cache(twd_d3d11_get_gfx_device(d3d_dev));
    TWD_CHECK(scache != NULL);
    twd_shader_cache_entry_t sentry;
    TWD_CHECK(twd_shader_cache_lookup(scache, dinfo.hash, &sentry) == 1 && sentry.glsl_source != NULL);
    static const uint32_t fake_spv[5] = {0x07230203u, 0x00010000u, 0u, 1u, 0u};
    TWD_CHECK(twd_shader_cache_put_spirv(scache, dinfo.hash, TWD_SHADER_STAGE_VS, fake_spv, 5u) == 0);
    TWD_CHECK(twd_shader_cache_lookup(scache, dinfo.hash, &sentry) == 1 && sentry.spirv_word_count == 5u);

    twd_d3d11_viewport_t vp = {0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f};
    twd_d3d11_rect_t sc = {0, 0, 1280, 720};
    void *rtvs[1];
    rtvs[0] = rtv;
    const float rgba[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    ((fn_set_topo_t)ctx_vtbl[24])(d3d_ctx, 4u);
    ((fn_set_rtvs_t)ctx_vtbl[33])(d3d_ctx, 1u, rtvs, NULL);
    ((fn_set_vp_t)ctx_vtbl[44])(d3d_ctx, 1u, &vp);
    ((fn_set_scissor_t)ctx_vtbl[45])(d3d_ctx, 1u, &sc);
    ((fn_clear_rtv_t)ctx_vtbl[50])(d3d_ctx, rtv, rgba);
    ((fn_draw_t)ctx_vtbl[13])(d3d_ctx, 3u, 0u);
    ((fn_draw_idx_t)ctx_vtbl[12])(d3d_ctx, 6u, 0u, 0);
    TWD_CHECK(((fn_present_t)sw_vtbl[8])(dxgi_sw, 1u, 0u) == 0);
    TWD_CHECK(((fn_resize_t)sw_vtbl[13])(dxgi_sw, 2u, 1920u, 1080u, 28u, 0u) == 0);

    twd_gfx_stats_t gstats;
    twd_gfx_get_stats(twd_d3d11_get_gfx_device(d3d_dev), &gstats);
    TWD_CHECK(gstats.width == 1920u && gstats.height == 1080u);
    TWD_CHECK(gstats.frame_index == 1u && gstats.draw_calls == 2u && gstats.clear_calls == 1u);
    TWD_CHECK(gstats.map_calls == 2u && gstats.update_calls == 2u);
    TWD_CHECK(gstats.topology == 4u && gstats.bound_rtv_count == 1u && gstats.scissor_right == 1280);
    TWD_CHECK(gstats.cached_shaders == 2u && gstats.shader_cache_hits == 1ULL && gstats.shader_cache_misses == 2ULL);
    TWD_CHECK(gstats.active_buffers == 1u && gstats.active_textures == 2u);

    TWD_CHECK(((fn_release_t)(*(void *const **)ps_obj)[2])(ps_obj) == 0u);
    TWD_CHECK(((fn_release_t)(*(void *const **)vs_dup)[2])(vs_dup) == 0u);
    TWD_CHECK(((fn_release_t)(*(void *const **)vs_obj)[2])(vs_obj) == 0u);

    TWD_CHECK(((fn_release_t)(*(void *const **)rtv)[2])(rtv) == 0u);
    TWD_CHECK(((fn_release_t)(*(void *const **)tex2)[2])(tex2) == 0u);
    TWD_CHECK(((fn_release_t)(*(void *const **)tex)[2])(tex) == 0u);
    TWD_CHECK(((fn_release_t)(*(void *const **)vbuf)[2])(vbuf) == 0u);
    TWD_CHECK(((fn_release_t)dev_vtbl[2])(d3d_dev) == 0u);
    TWD_CHECK(((fn_release_t)ctx_vtbl[2])(d3d_ctx) == 0u);
    TWD_CHECK(((fn_release_t)sw_vtbl[2])(dxgi_sw) == 0u);
    twd_window_destroy(gwin);

    /* Input PAL */
    twd_gamepad_state_t pad_in, pad_out;
    memset(&pad_in, 0, sizeof(pad_in));
    TWD_CHECK(twd_input_get_gamepad_state(0u, &pad_out) != 0u);
    pad_in.buttons = TWD_PAD_A | TWD_PAD_START;
    pad_in.thumb_lx = 12345;
    twd_input_push_gamepad_state(0u, &pad_in);
    TWD_CHECK(twd_input_get_gamepad_state(0u, &pad_out) == 0u);
    TWD_CHECK(pad_out.buttons == (TWD_PAD_A | TWD_PAD_START) && pad_out.thumb_lx == 12345);
    TWD_CHECK(twd_input_set_rumble(0u, 1000u, 2000u) == 0u);
    uint16_t rl = 0u, rr = 0u;
    twd_input_get_rumble(0u, &rl, &rr);
    TWD_CHECK(rl == 1000u && rr == 2000u);
    twd_input_set_pointer(640.0f, 360.0f, 1u);
    twd_pointer_state_t pstate;
    twd_input_get_pointer(&pstate);
    TWD_CHECK(pstate.x == 640.0f && pstate.y == 360.0f && pstate.buttons == 1u);
    twd_input_set_key(0x20u, 1);
    TWD_CHECK(twd_input_key_down(0x20u) == 1);
    void *di8 = NULL;
    TWD_CHECK(twd_dinput8_create_stub(&di8) == 0 && di8 != NULL);
    TWD_CHECK(((fn_release_t)(*(void *const **)di8)[2])(di8) == 0u);

    /* Media, Audio Focus, Save Dir, COM/KV & Telemetry Stubs */
    TWD_CHECK(twd_media_startup(0x20070u, 0u) == 0 && twd_media_is_initialized() == 1);
    TWD_CHECK(twd_media_shutdown() == 0 && twd_media_is_initialized() == 0);
    twd_audio_set_focus(0);
    TWD_CHECK(twd_audio_has_focus() == 0);
    twd_audio_set_focus(1);
    TWD_CHECK(twd_audio_has_focus() == 1);

    char appdir[512];
    TWD_CHECK(twd_app_data_dir(TWD_DIR_SAVES, appdir, sizeof(appdir)) > 0);
    TWD_CHECK(twd_com_init(0u) == 0 && twd_com_is_initialized() == 1);
    void *cmem = twd_com_alloc(32u);
    TWD_CHECK(cmem != NULL);
    cmem = twd_com_realloc(cmem, 64u);
    TWD_CHECK(cmem != NULL);
    twd_com_free(cmem);
    twd_com_uninit();
    TWD_CHECK(twd_com_is_initialized() == 0);

    TWD_CHECK(twd_kv_set("Graphics", "Fullscreen", "0") == 0);
    char kv_val[32];
    TWD_CHECK(twd_kv_get("Graphics", "Fullscreen", kv_val, sizeof(kv_val)) == 1 && strcmp(kv_val, "0") == 0);
    TWD_CHECK(twd_kv_delete("Graphics", "Fullscreen") == 0);
    TWD_CHECK(twd_kv_get("Graphics", "Fullscreen", kv_val, sizeof(kv_val)) < 0);
    TWD_CHECK(twd_telemetry_enabled() == 0);
    TWD_CHECK(twd_wininet_offline_status() != 0u);
    twd_eventlog_stub("TWD4", "noop");

    /* GUID, COM IStream, Memory Status, Multi-Wait, Sockets & 2D Surface PAL */
    twd_guid_t g1, g2;
    TWD_CHECK(twd_guid_generate(&g1) == 0);
    char gstr[48];
    TWD_CHECK(twd_guid_to_string(&g1, gstr, sizeof(gstr)) == 38);
    TWD_CHECK(twd_guid_from_string(gstr, &g2) == 0);
    TWD_CHECK(memcmp(&g1, &g2, sizeof(twd_guid_t)) == 0);
    void *com_obj = (void *)&sentinel;
    TWD_CHECK(twd_com_create_instance_stub(&g1, &com_obj) != 0 && com_obj == NULL);

    twd_stream_t *st = twd_stream_create("AB", 2u);
    TWD_CHECK(st != NULL && twd_stream_size(st) == 2u);
    TWD_CHECK(twd_stream_seek(st, 0, TWD_SEEK_END) == 2);
    TWD_CHECK(twd_stream_write(st, "CD", 2u) == 2u && twd_stream_size(st) == 4u);
    TWD_CHECK(twd_stream_seek(st, 1, TWD_SEEK_SET) == 1);
    char sbuf[4] = {0};
    TWD_CHECK(twd_stream_read(st, sbuf, 3u) == 3u && memcmp(sbuf, "BCD", 3) == 0);
    TWD_CHECK(twd_stream_release(st) == 0u);

    twd_memory_status_t mst;
    TWD_CHECK(twd_memory_status(&mst) == 0 && mst.total_phys > 0ULL);

    twd_event_t ev_a, ev_b;
    TWD_CHECK(twd_event_init(&ev_a, 0, 0) == 0);
    TWD_CHECK(twd_event_init(&ev_b, 0, 0) == 0);
    twd_event_t *ev_list[2];
    ev_list[0] = &ev_a;
    ev_list[1] = &ev_b;
    TWD_CHECK(twd_wait_any(ev_list, 2u, 0u) == TWD_WAIT_TIMEOUT);
    TWD_CHECK(twd_event_set(&ev_b) == 0);
    TWD_CHECK(twd_wait_any(ev_list, 2u, 100u) == 1);
    twd_event_destroy(&ev_a);
    twd_event_destroy(&ev_b);

    TWD_CHECK(twd_lang_id() == 0x0409u);
    TWD_CHECK(twd_net_init() == 0 && twd_net_is_initialized() == 1 && twd_net_last_error() == 0);
    twd_net_shutdown();
    TWD_CHECK(twd_net_is_initialized() == 0);

    twd_surface_t *s_src = twd_surface_create(4u, 4u);
    twd_surface_t *s_dst = twd_surface_create(8u, 8u);
    TWD_CHECK(s_src != NULL && s_dst != NULL);
    twd_surface_fill(s_src, 0xff112233u);
    twd_surface_fill(s_dst, 0u);
    TWD_CHECK(twd_surface_blit(s_dst, 2u, 3u, s_src, 0u, 0u, 4u, 4u) == 0);
    TWD_CHECK(s_dst->pixels[3u * 8u + 2u] == 0xff112233u);
    twd_surface_destroy(s_src);
    twd_surface_destroy(s_dst);

    int dpix = 0, dpiy = 0;
    twd_display_dpi(&dpix, &dpiy);
    TWD_CHECK(dpix == 96 && dpiy == 96);

    twd_window_t *pwin = twd_window_create("Paint", 0, 0, 640, 480, 0u);
    TWD_CHECK(pwin != NULL && twd_window_needs_paint(pwin) == 0);
    twd_window_invalidate(pwin);
    TWD_CHECK(twd_window_needs_paint(pwin) == 1);
    twd_window_clear_paint(pwin);
    TWD_CHECK(twd_window_needs_paint(pwin) == 0);
    twd_window_destroy(pwin);

    /* Android GameActivity lifecycle, surface, dirs & touch bridge */
    int dummy_anw = 0x1234, dummy_mgr = 0x5678;
    twd_android_set_native_window(&dummy_anw);
    twd_android_set_asset_manager(&dummy_mgr);
    TWD_CHECK(twd_android_get_native_window() == &dummy_anw);
    TWD_CHECK(twd_android_get_asset_manager() == &dummy_mgr);

    twd_android_set_data_dirs("/data/user/0/twd/files", "/sdcard/Android/data/twd/files");
    char adir[256];
    TWD_CHECK(twd_android_get_data_dir(0, adir, sizeof(adir)) > 0 &&
              strcmp(adir, "/data/user/0/twd/files") == 0);
    TWD_CHECK(twd_app_data_dir(TWD_DIR_SAVES, adir, sizeof(adir)) > 0 &&
              strcmp(adir, "/sdcard/Android/data/twd/files") == 0);

    twd_android_on_touch(TWD_TOUCH_DOWN, 0, 320.0f, 180.0f);
    twd_input_get_pointer(&pstate);
    TWD_CHECK(pstate.x == 320.0f && pstate.y == 180.0f && pstate.buttons == 1u);
    TWD_CHECK(twd_window_poll_event(&ev_out) == 1 && ev_out.kind == TWD_EVT_POINTER);
    twd_android_on_touch(TWD_TOUCH_UP, 0, 320.0f, 180.0f);
    TWD_CHECK(twd_window_poll_event(&ev_out) == 1 && ev_out.kind == TWD_EVT_POINTER);

    twd_android_on_key(0x57, 1);
    TWD_CHECK(twd_input_key_down(0x57u) == 1);
    TWD_CHECK(twd_window_poll_event(&ev_out) == 1 && ev_out.kind == TWD_EVT_KEY);

    twd_android_on_focus(0);
    TWD_CHECK(twd_android_has_focus() == 0 && twd_audio_has_focus() == 0);
    twd_android_on_focus(1);
    TWD_CHECK(twd_android_has_focus() == 1 && twd_audio_has_focus() == 1);

    twd_window_t *rwin = twd_window_create("Resize", 0, 0, 800, 600, 0u);
    void *rd3d = NULL;
    TWD_CHECK(twd_d3d11_create_device_shim(rwin, 800u, 600u, NULL, &rd3d, NULL, NULL) == 0);
    twd_gfx_device_t *rgfx = twd_d3d11_get_gfx_device(rd3d);
    TWD_CHECK(twd_gfx_has_driver_symbols(rgfx) == 0);
    TWD_CHECK(twd_gfx_gpu_count(rgfx) == 0u);
    twd_android_attach_native_activity(NULL, test_attach_hook_cb);
    TWD_CHECK(g_attach_hook_ran == 1);
    char drv_name[32];
    TWD_CHECK(twd_gfx_probe_driver(rgfx, drv_name, sizeof(drv_name)) == TWD_GFX_BACKEND_NONE);
    TWD_CHECK(strcmp(drv_name, "Headless") == 0);

    static const uint8_t iid_dxgi_dev[16] = {
        0xfa, 0x77, 0xec, 0x54, 0x77, 0x13, 0xe6, 0x44,
        0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x82, 0xc0, 0x12
    };
    void *dxgi_dev = NULL;
    typedef int32_t (*fn_qi_t)(void *, const void *, void **);
    typedef int32_t (*fn_get_ad_t)(void *, void **);
    typedef int32_t (*fn_get_desc1_t)(void *, twd_dxgi_adapter_desc1_t *);
    TWD_CHECK(((fn_qi_t)(*(void *const **)rd3d)[0])(rd3d, iid_dxgi_dev, &dxgi_dev) == 0 && dxgi_dev != NULL);
    void *dxgi_ad = NULL;
    TWD_CHECK(((fn_get_ad_t)(*(void *const **)dxgi_dev)[7])(dxgi_dev, &dxgi_ad) == 0 && dxgi_ad != NULL);
    twd_dxgi_adapter_desc1_t adesc;
    TWD_CHECK(((fn_get_desc1_t)(*(void *const **)dxgi_ad)[10])(dxgi_ad, &adesc) == 0);
    char ad_utf8[64];
    TWD_CHECK(twd_utf16_to_utf8(adesc.description, -1, ad_utf8, sizeof(ad_utf8)) > 0);
    TWD_CHECK(strcmp(ad_utf8, "TWD-Final Vulkan/GLES") == 0);
    TWD_CHECK(((uint32_t (*)(void *))(*(void *const **)dxgi_ad)[2])(dxgi_ad) == 0u);
    TWD_CHECK(((uint32_t (*)(void *))(*(void *const **)dxgi_dev)[2])(dxgi_dev) == 0u);

    twd_android_on_resize(rwin, rgfx, 2400u, 1080u);
    twd_gfx_stats_t rstats;
    twd_gfx_get_stats(rgfx, &rstats);
    TWD_CHECK(rstats.width == 2400u && rstats.height == 1080u && twd_window_needs_paint(rwin) == 1);
    TWD_CHECK(twd_window_poll_event(&ev_out) == 1 && ev_out.kind == TWD_EVT_RESIZE);

    loop_test_ctx_t lctx;
    memset(&lctx, 0, sizeof(lctx));
    twd_app_callbacks_t lcb;
    lcb.on_init = test_loop_init;
    lcb.on_tick = test_loop_tick;
    lcb.on_render = test_loop_render;
    lcb.on_shutdown = test_loop_shutdown;
    lcb.user = &lctx;
    lcb.max_frames = 3u;
    twd_clear_quit();
    twd_android_on_motion_axis(0u, 0.5f, -1.0f, 0.0f, 1.0f, 0.5f, 1.0f, TWD_PAD_A);
    TWD_CHECK(twd_input_get_gamepad_state(0u, &pad_out) == 0u);
    TWD_CHECK(pad_out.buttons == TWD_PAD_A && pad_out.thumb_ly == -32767 && pad_out.right_trigger == 255u);

    twd_file_t af;
    TWD_CHECK(twd_file_open("twd_asset_test.tmp", TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &af) == 0);
    TWD_CHECK(twd_file_write(&af, "LUA", 3) == 3);
    TWD_CHECK(twd_file_close(&af) == 0);
    size_t asz = 0u;
    char *adata = (char *)twd_asset_read_alloc("twd_asset_test.tmp", &asz);
    TWD_CHECK(adata != NULL && asz == 3u && strcmp(adata, "LUA") == 0);
    twd_asset_free(adata);
    TWD_CHECK(twd_file_delete("twd_asset_test.tmp") == 0);

    char raw_oodle[5] = {0};
    TWD_CHECK(twd_oodle_is_available() == 0);
    TWD_CHECK(twd_oodle_decompress("TWD!", 4u, raw_oodle, 4u) == 4 && strcmp(raw_oodle, "TWD!") == 0);
    twd_oodle_shutdown();

    /* TTArchive2 container header, NCTT/ZCTT/ECTT TOC reader, mount fallback & Video PAL */
    static const uint8_t nctt_hdr[12] = {
        'N','C','T','T', 0x00,0x10,0x00,0x00, 0,0,0,0
    };
    twd_ttarch2_header_t tah;
    TWD_CHECK(twd_ttarch2_parse_header(nctt_hdr, sizeof(nctt_hdr), &tah) == 0);
    TWD_CHECK(tah.container_kind == TWD_TTARCH2_NCTT && tah.encrypted == 0 &&
              tah.compressed == 0 && tah.payload_size == 0x1000ULL && tah.header_size == 12u);
    static const uint8_t zctt_hdr[28] = {
        'Z','C','T','T', 0x00,0x00,0x01,0x00, 1,0,0,0,
        0x1C,0,0,0, 0,0,0,0, 0x3C,0,0,0, 0,0,0,0
    };
    TWD_CHECK(twd_ttarch2_parse_header(zctt_hdr, sizeof(zctt_hdr), &tah) == 0);
    TWD_CHECK(tah.container_kind == TWD_TTARCH2_ZCTT && tah.encrypted == 0 &&
              tah.compressed == 1 && tah.chunk_size == 0x10000u &&
              tah.chunk_count == 1u && tah.payload_size == 0x20ULL);
    static const uint8_t ectt_hdr[16] = {
        'e','C','T','T', 1,0,0,0, 0x20,0,0,0, 0,0,0,0
    };
    TWD_CHECK(twd_ttarch2_parse_header(ectt_hdr, sizeof(ectt_hdr), &tah) == 0);
    TWD_CHECK(tah.container_kind == TWD_TTARCH2_ECTT && tah.encrypted == 1 &&
              tah.compressed == 1 && tah.compress_mode == 1u && tah.chunk_size == 0x20u);

    /* Build synthetic TTA4 logical payload: 2 entries ("scripts/_boot.lua", "config\\prefs.prop") */
    static const char s_boot_lua[] = "print('boot_ok')";
    static const char s_prefs[] = "volume=0.8";
    static const char n_tab[] = "scripts/_boot.lua\0config\\prefs.prop\0";
    enum {
        TOC_ENTRIES = 2,
        NAMES_SZ = sizeof(n_tab) - 1,
        DATA_BASE = 12 + TOC_ENTRIES * 28 + NAMES_SZ,
        BOOT_LEN = sizeof(s_boot_lua) - 1,
        PREFS_LEN = sizeof(s_prefs) - 1,
        LOGICAL_SZ = DATA_BASE + BOOT_LEN + PREFS_LEN
    };
    uint8_t logical_tt[LOGICAL_SZ];
    memset(logical_tt, 0, sizeof(logical_tt));
    ttarch_wr_u32(logical_tt + 0u, TWD_TTARCH2_TOC_TTA4);
    ttarch_wr_u32(logical_tt + 4u, (uint32_t)NAMES_SZ);
    ttarch_wr_u32(logical_tt + 8u, (uint32_t)TOC_ENTRIES);
    /* Entry 0 */
    ttarch_wr_u64(logical_tt + 12u + 0u, 0x1111222233334444ULL);
    ttarch_wr_u64(logical_tt + 12u + 8u, 0ULL);
    ttarch_wr_u32(logical_tt + 12u + 16u, (uint32_t)BOOT_LEN);
    ttarch_wr_u32(logical_tt + 12u + 20u, 0u);
    ttarch_wr_u16(logical_tt + 12u + 24u, 0u);
    ttarch_wr_u16(logical_tt + 12u + 26u, 0u);
    /* Entry 1 */
    ttarch_wr_u64(logical_tt + 40u + 0u, 0x5555666677778888ULL);
    ttarch_wr_u64(logical_tt + 40u + 8u, (uint64_t)BOOT_LEN);
    ttarch_wr_u32(logical_tt + 40u + 16u, (uint32_t)PREFS_LEN);
    ttarch_wr_u32(logical_tt + 40u + 20u, 0u);
    ttarch_wr_u16(logical_tt + 40u + 24u, 0u);
    ttarch_wr_u16(logical_tt + 40u + 26u, 18u);
    memcpy(logical_tt + 12u + 56u, n_tab, NAMES_SZ);
    memcpy(logical_tt + DATA_BASE, s_boot_lua, BOOT_LEN);
    memcpy(logical_tt + DATA_BASE + BOOT_LEN, s_prefs, PREFS_LEN);

    /* 1. NCTT archive file + mount fallback */
    uint8_t nctt_blob[12 + LOGICAL_SZ];
    memcpy(nctt_blob, "NCTT", 4u);
    ttarch_wr_u64(nctt_blob + 4u, (uint64_t)LOGICAL_SZ);
    memcpy(nctt_blob + 12u, logical_tt, LOGICAL_SZ);

    twd_file_t ttf;
    TWD_CHECK(twd_file_open("twd_synth_test.ttarch2", TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &ttf) == 0);
    TWD_CHECK(twd_file_write(&ttf, nctt_blob, sizeof(nctt_blob)) == (int64_t)sizeof(nctt_blob));
    TWD_CHECK(twd_file_close(&ttf) == 0);

    twd_ttarch2_archive_t *arch = twd_ttarch2_open("twd_synth_test.ttarch2");
    TWD_CHECK(arch != NULL);
    TWD_CHECK(twd_ttarch2_container_kind(arch) == TWD_TTARCH2_NCTT);
    TWD_CHECK(twd_ttarch2_toc_magic(arch) == TWD_TTARCH2_TOC_TTA4);
    TWD_CHECK(twd_ttarch2_entry_count(arch) == 2u);
    uint32_t found_idx = 99u;
    twd_ttarch2_entry_t te;
    TWD_CHECK(twd_ttarch2_find(arch, "CONFIG/PREFS.PROP", &found_idx, &te) == 0 &&
              found_idx == 1u && te.size == (uint32_t)PREFS_LEN);
    TWD_CHECK(twd_ttarch2_find_crc64(arch, 0x1111222233334444ULL, &found_idx, NULL) == 0 &&
              found_idx == 0u);

    TWD_CHECK(twd_asset_mount_ttarch2(arch) == 0);
    size_t boot_sz = 0u;
    char *boot_txt = (char *)twd_asset_read_alloc("SCRIPTS\\_BOOT.LUA", &boot_sz);
    TWD_CHECK(boot_txt != NULL && boot_sz == (size_t)BOOT_LEN &&
              strcmp(boot_txt, s_boot_lua) == 0);
    twd_asset_free(boot_txt);
    twd_ttarch2_close(arch);
    TWD_CHECK(twd_file_delete("twd_synth_test.ttarch2") == 0);

    /* 2. ZCTT multi-chunk container spanning TOC + payloads across 32-byte chunks */
    enum {
        TEST_CHUNK_SZ = 32u,
        TEST_N_CHUNKS = (LOGICAL_SZ + TEST_CHUNK_SZ - 1u) / TEST_CHUNK_SZ,
        ZCTT_HDR_SZ = 12u + (TEST_N_CHUNKS + 1u) * 8u
    };
    uint8_t zctt_blob[ZCTT_HDR_SZ + LOGICAL_SZ];
    memcpy(zctt_blob, "ZCTT", 4u);
    ttarch_wr_u32(zctt_blob + 4u, TEST_CHUNK_SZ);
    ttarch_wr_u32(zctt_blob + 8u, TEST_N_CHUNKS);
    for (uint32_t ci = 0u; ci < TEST_N_CHUNKS; ++ci) {
        uint64_t off = (uint64_t)ZCTT_HDR_SZ + (uint64_t)ci * TEST_CHUNK_SZ;
        ttarch_wr_u64(zctt_blob + 12u + (size_t)ci * 8u, off);
    }
    ttarch_wr_u64(zctt_blob + 12u + (size_t)TEST_N_CHUNKS * 8u, (uint64_t)(ZCTT_HDR_SZ + LOGICAL_SZ));
    memcpy(zctt_blob + ZCTT_HDR_SZ, logical_tt, LOGICAL_SZ);

    twd_ttarch2_archive_t *zarch = twd_ttarch2_open_memory(zctt_blob, sizeof(zctt_blob));
    TWD_CHECK(zarch != NULL && twd_ttarch2_container_kind(zarch) == TWD_TTARCH2_ZCTT);
    TWD_CHECK(twd_ttarch2_find(zarch, "config/prefs.prop", &found_idx, NULL) == 0);
    size_t pr_sz = 0u;
    char *pr_txt = (char *)twd_ttarch2_read_entry_alloc(zarch, found_idx, &pr_sz);
    TWD_CHECK(pr_txt != NULL && pr_sz == (size_t)PREFS_LEN && strcmp(pr_txt, s_prefs) == 0);
    twd_ttarch2_free(pr_txt);
    twd_ttarch2_close(zarch);

    /* 3. ECTT encrypted multi-chunk container via decrypt callback */
    uint8_t ectt_blob[ZCTT_HDR_SZ + LOGICAL_SZ];
    memcpy(ectt_blob, zctt_blob, sizeof(ectt_blob));
    memcpy(ectt_blob, "ECTT", 4u);
    for (uint32_t ci = 0u; ci < TEST_N_CHUNKS; ++ci) {
        size_t pos = (size_t)ZCTT_HDR_SZ + (size_t)ci * TEST_CHUNK_SZ;
        size_t clen = (pos + TEST_CHUNK_SZ <= sizeof(ectt_blob))
                          ? TEST_CHUNK_SZ
                          : (sizeof(ectt_blob) - pos);
        synthetic_xor_decrypt(ci, ectt_blob + pos, clen, (void *)(uintptr_t)0x5Au);
    }
    TWD_CHECK(twd_ttarch2_open_memory(ectt_blob, sizeof(ectt_blob)) == NULL);
    twd_ttarch2_set_decrypt_callback(synthetic_xor_decrypt, (void *)(uintptr_t)0x5Au);
    twd_ttarch2_archive_t *earch = twd_ttarch2_open_memory(ectt_blob, sizeof(ectt_blob));
    TWD_CHECK(earch != NULL && twd_ttarch2_container_kind(earch) == TWD_TTARCH2_ECTT);
    char *eb_txt = (char *)twd_ttarch2_read_entry_alloc(earch, 0u, &boot_sz);
    TWD_CHECK(eb_txt != NULL && boot_sz == (size_t)BOOT_LEN && strcmp(eb_txt, s_boot_lua) == 0);
    twd_ttarch2_free(eb_txt);
    twd_ttarch2_close(earch);
    twd_ttarch2_set_decrypt_callback(NULL, NULL);

    /* 4. Directory auto-mount helper (twd_ttarch2_mount_dir + unmount_all cleanup) */
    TWD_CHECK(twd_mkdir("twd_synth_dir") == 0);
    twd_file_t dir_tf;
    TWD_CHECK(twd_file_open("twd_synth_dir/pack0.ttarch2", TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &dir_tf) == 0);
    TWD_CHECK(twd_file_write(&dir_tf, nctt_blob, sizeof(nctt_blob)) == (int64_t)sizeof(nctt_blob));
    TWD_CHECK(twd_file_close(&dir_tf) == 0);
    TWD_CHECK(twd_ttarch2_mount_dir("twd_synth_dir") == 1);
    boot_txt = (char *)twd_asset_read_alloc("scripts/_boot.lua", &boot_sz);
    TWD_CHECK(boot_txt != NULL && boot_sz == (size_t)BOOT_LEN && strcmp(boot_txt, s_boot_lua) == 0);
    twd_asset_free(boot_txt);
    twd_asset_unmount_all_ttarch2();
    TWD_CHECK(twd_file_delete("twd_synth_dir/pack0.ttarch2") == 0);
    TWD_CHECK(twd_rmdir("twd_synth_dir") == 0);

    /* 5. Audio & FMOD Studio / Core PAL */
    TWD_CHECK((twd_audio_init(NULL, NULL) & TWD_AUDIO_CAP_MIXER) != 0);
    twd_audio_system_t *asys = twd_audio_system_create();
    TWD_CHECK(asys != NULL);
    TWD_CHECK(twd_audio_system_initialize(asys, 64u, 48000u, 0u) == 0);
    static const uint8_t fake_bank[16] = {'R','I','F','F', 8,0,0,0, 'F','E','V',' ', 1,0,0,0};
    twd_audio_bank_t *abank = twd_audio_load_bank_memory(asys, "Master.bank", fake_bank, sizeof(fake_bank));
    TWD_CHECK(abank != NULL && twd_audio_bank_is_valid(abank) == 1 && twd_audio_bank_count(asys) == 1u);
    twd_audio_bus_t *mbus = twd_audio_get_bus(asys, "bus:/Music");
    TWD_CHECK(mbus != NULL && twd_audio_bus_set_volume(mbus, 0.5f) == 0);
    twd_audio_event_desc_t *ed = twd_audio_register_event(asys, "event:/Menu/Theme", 200u, 1, "bus:/Music");
    TWD_CHECK(ed != NULL);
    twd_audio_event_instance_t *einst = twd_audio_create_instance(asys, ed);
    TWD_CHECK(einst != NULL && twd_audio_instance_start(einst) == 0);
    TWD_CHECK(twd_audio_instance_set_parameter(einst, "Tension", 0.75f) == 0);
    float pval = 0.0f;
    TWD_CHECK(twd_audio_instance_get_parameter(einst, "Tension", &pval) == 0 && pval == 0.75f);
    TWD_CHECK(twd_audio_system_update(asys, 100u) == 0);
    TWD_CHECK(twd_audio_instance_get_timeline_ms(einst) == 100u &&
              twd_audio_instance_get_state(einst) == TWD_AUDIO_PLAYING);
    TWD_CHECK(twd_audio_system_update(asys, 100u) == 0);
    TWD_CHECK(twd_audio_instance_get_timeline_ms(einst) == 200u &&
              twd_audio_instance_get_state(einst) == TWD_AUDIO_STOPPED);
    twd_audio_instance_release(einst);

    static const int16_t mono_pcm[4] = {1000, -1000, 2000, -2000};
    twd_audio_sound_t *asnd = twd_audio_create_sound_pcm16(asys, mono_pcm, 4u, 1u, 48000u, 0);
    TWD_CHECK(asnd != NULL);
    twd_audio_channel_t *ach = twd_audio_play_sound(asys, asnd, mbus, 0);
    TWD_CHECK(ach != NULL && twd_audio_channel_is_playing(ach) == 1);
    int16_t mix_out[8] = {0};
    TWD_CHECK(twd_audio_mix_pcm_i16(asys, mix_out, 4u) == 4u);
    TWD_CHECK(mix_out[0] == 500 && mix_out[1] == 500 && mix_out[4] == 1000 && mix_out[5] == 1000);
    TWD_CHECK(twd_audio_system_mixer_suspend(asys) == 0 && twd_audio_system_is_suspended(asys) == 1);
    TWD_CHECK(twd_audio_system_mixer_resume(asys) == 0 && twd_audio_system_is_suspended(asys) == 0);
    TWD_CHECK(twd_audio_bank_unload(abank) == 0 && twd_audio_bank_count(asys) == 0u);
    twd_audio_sound_release(asnd);
    twd_audio_system_release(asys);
    twd_audio_shutdown();

    TWD_CHECK(twd_gfx_has_gl_dispatch(rgfx) == 0);
    TWD_CHECK(twd_gfx_gl_compile_glsl(rgfx, TWD_SHADER_STAGE_VS, "void main(){}") == 0u);
    twd_video_player_t *vp_pl = twd_video_create(rgfx);
    TWD_CHECK(vp_pl != NULL);
    TWD_CHECK(twd_video_set_source(vp_pl, "intro.mp4") == 0);
    TWD_CHECK(twd_video_play(vp_pl) == 0);
    TWD_CHECK(twd_video_set_volume(vp_pl, 0.5) == 0);
    int64_t pts_ns = 0;
    TWD_CHECK(twd_video_tick(vp_pl, &pts_ns) == 0);
    uint32_t vw = 0u, vh = 0u;
    twd_video_get_size(vp_pl, &vw, &vh);
    TWD_CHECK(vw == 1920u && vh == 1080u && twd_video_get_duration(vp_pl) == 0.0);
    TWD_CHECK(twd_video_transfer_frame(vp_pl, NULL) == 0);
    TWD_CHECK(twd_video_pause(vp_pl) == 0);
    twd_video_destroy(vp_pl);

    TWD_CHECK(twd_android_run_frame_loop(rwin, rgfx, &lcb) == 3ULL);
    TWD_CHECK(lctx.inited == 1 && lctx.ticks == 3 && lctx.renders == 3 && lctx.shutdown == 1);

    TWD_CHECK(((uint32_t (*)(void *))(*(void *const **)rd3d)[2])(rd3d) == 0u);
    twd_window_destroy(rwin);

    twd_platform_shutdown();
    printf("test_runtime: ok\n");
    return 0;
}
