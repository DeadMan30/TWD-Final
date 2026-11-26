/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Critical-path engine boot, VFS archive mount, renderer/audio initialization,
 * and frame loop bring-up connecting game hooks to the portable runtime PAL
 * (WinMain 0x14002f7b0 -> EngineInit 0x14006d450 -> ScriptBoot 0x14006d250
 *  -> RenderDeviceInit 0x1405d9f10 -> GameEngine::Loop 0x140067fd0).
 */
#include "twd_game.h"
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

typedef struct twd_boot_loop_state {
    twd_boot_config_t cfg;
    twd_boot_stats_t *stats;
    twd_window_t *win;
    twd_gfx_device_t *gfx;
    twd_audio_system_t *audio;
    twd_video_player_t *video;
    void *d3d_dev;
    void *d3d_ctx;
    void *d3d_swap;
    int owns_gfx_direct;
    uint64_t ticks;
    uint64_t renders;
    uint64_t elapsed_ns;
    int clean_shutdown;
} twd_boot_loop_state_t;

static void release_com_obj(void *obj) {
    if (!obj) return;
    ((uint32_t (*)(void *))(*(void *const **)obj)[2])(obj);
}

static int boot_probe_vfs_flag(const char *rel_path, twd_metastream_header_t *opt_meta) {
    size_t sz = 0u;
    void *buf = twd_asset_read_alloc(rel_path, &sz);
    if (!buf) return 0;
    if (opt_meta) {
        (void)twd_metastream_parse_header(buf, sz, opt_meta);
    }
    twd_asset_free(buf);
    return (sz > 0u) ? 1 : 0;
}

static int boot_on_init(void *user) {
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    if (!st || !st->gfx) return -1;

    uint32_t w = st->cfg.width ? st->cfg.width : 1280u;
    uint32_t h = st->cfg.height ? st->cfg.height : 720u;
    twd_gfx_set_viewport(st->gfx, 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f);

    if (st->audio) {
        (void)twd_audio_load_bank_vfs(st->audio, "Master.bank");
    }
    twd_kv_set("Engine", "BootPhase", "LoopActive");
    return 0;
}

static void boot_on_tick(void *user, uint64_t dt_ns) {
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    if (!st) return;

    st->elapsed_ns += dt_ns;
    st->ticks++;

    twd_gamepad_state_t pad;
    (void)twd_input_get_gamepad_state(0u, &pad);

    if (st->audio) {
        uint32_t dt_ms = (uint32_t)(dt_ns / 1000000ULL);
        if (dt_ms == 0u) dt_ms = 1u;
        (void)twd_audio_system_update(st->audio, dt_ms);
        int16_t mix_quantum[512 * 2];
        (void)twd_audio_mix_pcm_i16(st->audio, mix_quantum, 512u);
    }

    if (st->video) {
        int64_t pts_ns = 0;
        (void)twd_video_tick(st->video, &pts_ns);
    }
}

static void boot_on_render(void *user, twd_gfx_device_t *gfx) {
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    if (!st || !gfx) return;

    static const float boot_clear_rgba[4] = {0.06f, 0.07f, 0.09f, 1.0f};
    twd_gfx_clear(gfx, boot_clear_rgba, 1.0f, 0u);
    st->renders++;
}

static void boot_on_shutdown(void *user) {
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    if (!st) return;
    st->clean_shutdown = 1;
}

TWD_EXPORT
int twd_game_boot_run(const twd_boot_config_t *cfg, twd_boot_stats_t *out_stats) {
    twd_boot_stats_t local_stats;
    memset(&local_stats, 0, sizeof(local_stats));

    twd_boot_config_t def_cfg;
    memset(&def_cfg, 0, sizeof(def_cfg));
    def_cfg.preferred_backend = -1;
    if (cfg) def_cfg = *cfg;

    uint32_t w = def_cfg.width ? def_cfg.width : 1280u;
    uint32_t h = def_cfg.height ? def_cfg.height : 720u;

    /* 1. Register decompiled engine/Lua hooks & bring up host/Android PAL */
    twd_game_register_hooks();
    local_stats.hook_count = twd_game_hook_count();
    if (twd_platform_init() != 0) {
        if (out_stats) *out_stats = local_stats;
        return -1;
    }
    (void)twd_com_init(0u);
    (void)twd_net_init();
    (void)twd_oodle_init(NULL);
    local_stats.audio_caps = twd_audio_init(NULL, NULL);

    /* 2. Mount game .ttarch2 archives & check critical boot assets */
    char dir_buf[512];
    const char *game_dir = def_cfg.game_dir;
    if (!game_dir || game_dir[0] == '\0') {
        if (twd_getenv("TWD_GAME_DIR", dir_buf, sizeof(dir_buf)) > 0) {
            game_dir = dir_buf;
        } else if (twd_path_is_dir("game")) {
            game_dir = "game";
        } else if (twd_app_data_dir(TWD_DIR_SAVES, dir_buf, sizeof(dir_buf)) > 0) {
            game_dir = dir_buf;
        }
    }
    if (game_dir && twd_path_is_dir(game_dir)) {
        int m = twd_ttarch2_mount_dir(game_dir);
        if (m > 0) local_stats.mounted_archives = m;
    }
    twd_metastream_header_t prefs_mh;
    memset(&prefs_mh, 0, sizeof(prefs_mh));
    local_stats.found_prefs_prop = boot_probe_vfs_flag("prefs.prop", &prefs_mh) ||
                                   boot_probe_vfs_flag("config/prefs.prop", &prefs_mh);
    local_stats.prefs_meta_version = prefs_mh.version;
    local_stats.found_boot_lua = boot_probe_vfs_flag("_boot.lua", NULL) ||
                                 boot_probe_vfs_flag("scripts/_boot.lua", NULL);

    /* 3. Create window + graphics device / DXBC shader cache */
    twd_boot_loop_state_t st;
    memset(&st, 0, sizeof(st));
    st.cfg = def_cfg;
    st.cfg.width = w;
    st.cfg.height = h;
    st.stats = &local_stats;

    st.win = twd_window_create("The Walking Dead: The Final Season", 64, 64, (int)w, (int)h, 0u);
    if (def_cfg.preferred_backend >= 0) {
        twd_gfx_desc_t gdesc;
        memset(&gdesc, 0, sizeof(gdesc));
        gdesc.native_window = twd_android_get_native_window() ? twd_android_get_native_window() : (void *)st.win;
        gdesc.width = w;
        gdesc.height = h;
        gdesc.preferred_backend = def_cfg.preferred_backend;
        gdesc.vsync = 1;
        st.gfx = twd_gfx_device_create(&gdesc);
        st.owns_gfx_direct = 1;
    } else {
        uint32_t feat = 0u;
        if (twd_d3d11_create_device_shim(
                st.win, w, h, &feat, &st.d3d_dev, &st.d3d_ctx, &st.d3d_swap) == 0 &&
            st.d3d_dev != NULL) {
            st.gfx = twd_d3d11_get_gfx_device(st.d3d_dev);
        }
    }

    if (st.gfx) {
        twd_gfx_stats_t gstats;
        twd_gfx_get_stats(st.gfx, &gstats);
        local_stats.gfx_backend = gstats.backend;
        local_stats.width = gstats.width ? gstats.width : w;
        local_stats.height = gstats.height ? gstats.height : h;
        (void)twd_gfx_get_shader_cache(st.gfx);
    } else {
        local_stats.width = w;
        local_stats.height = h;
    }

    /* 4. Media / FMOD Audio PAL & focus state */
    (void)twd_media_startup(0x20070u, 0u);
    st.audio = twd_audio_system_create();
    if (st.audio) {
        (void)twd_audio_system_initialize(st.audio, 64u, 48000u, 0u);
    }
    twd_audio_set_focus(1);
    twd_android_on_focus(1);
    st.video = twd_video_create(st.gfx);

    /* 5. Main GameEngine frame loop */
    twd_app_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_init = boot_on_init;
    cb.on_tick = boot_on_tick;
    cb.on_render = boot_on_render;
    cb.on_shutdown = boot_on_shutdown;
    cb.user = &st;
    cb.max_frames = def_cfg.max_frames;

    twd_clear_quit();
    local_stats.frames_rendered = twd_android_run_frame_loop(st.win, st.gfx, &cb);
    local_stats.elapsed_ns = st.elapsed_ns;
    local_stats.clean_shutdown = st.clean_shutdown;

    /* 6. Ordered shutdown */
    if (st.video) twd_video_destroy(st.video);
    if (st.audio) twd_audio_system_release(st.audio);
    if (st.owns_gfx_direct) {
        if (st.gfx) twd_gfx_device_destroy(st.gfx);
    } else {
        release_com_obj(st.d3d_swap);
        release_com_obj(st.d3d_ctx);
        release_com_obj(st.d3d_dev);
    }
    if (st.win) twd_window_destroy(st.win);
    twd_asset_unmount_all_ttarch2();
    (void)twd_media_shutdown();
    twd_audio_shutdown();
    twd_oodle_shutdown();
    twd_net_shutdown();
    twd_com_uninit();
    twd_platform_shutdown();

    if (out_stats) *out_stats = local_stats;
    return st.clean_shutdown ? 0 : -1;
}
