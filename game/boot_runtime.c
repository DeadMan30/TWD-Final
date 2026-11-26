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
    twd_gfx_texture_t *splash_tex;
    twd_font_t boot_font;
    int has_boot_font;
    void *d3d_dev;
    void *d3d_ctx;
    void *d3d_swap;
    int owns_gfx_direct;
    uint64_t ticks;
    uint64_t renders;
    uint64_t elapsed_ns;
    int clean_shutdown;
} twd_boot_loop_state_t;

static int lua_scene_open(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        (void)twd_kv_set("lua.scene", "active", argv[0].u.str);
        (void)twd_kv_set("lua.scene.state", argv[0].u.str, "1");
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = 1; }
    return 0;
}

static int lua_scene_is_active(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user;
    int active = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        char buf[8];
        if (twd_kv_get("lua.scene.state", argv[0].u.str, buf, sizeof(buf)) > 0 && strcmp(buf, "1") == 0) {
            active = 1;
        }
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = active; }
    return 0;
}

static int lua_input_is_key_down(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user;
    int down = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_NUM) {
        down = twd_input_key_down((uint32_t)argv[0].u.num);
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = down; }
    return 0;
}

static int lua_cursor_get_pos(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user; (void)argc; (void)argv;
    twd_pointer_state_t ptr;
    twd_input_get_pointer(&ptr);
    if (ret) { ret->kind = TWD_LUA_T_NUM; ret->u.num = ptr.x; }
    return 0;
}

static int lua_engine_quit(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user; (void)argc; (void)argv;
    twd_post_quit(0);
    if (ret) ret->kind = TWD_LUA_T_NIL;
    return 0;
}

static void release_com_obj(void *obj) {
    if (!obj) return;
    ((uint32_t (*)(void *))(*(void *const **)obj)[2])(obj);
}

static void boot_kv_set_u32(const char *key, uint32_t v) {
    char buf[16];
    size_t i = sizeof(buf);
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && i > 0u);
    (void)twd_kv_set("prefs", key, &buf[i]);
}

static int boot_load_prefs_prop(const char *rel_path,
                                twd_boot_stats_t *stats,
                                uint32_t *io_w,
                                uint32_t *io_h) {
    size_t sz = 0u;
    void *buf = twd_asset_read_alloc(rel_path, &sz);
    if (!buf) return 0;

    twd_metastream_header_t mh;
    memset(&mh, 0, sizeof(mh));
    if (twd_metastream_parse_header(buf, sz, &mh) == 0) {
        stats->prefs_meta_version = mh.version;
    }

    twd_propset_t ps;
    if (twd_propset_parse_memory(buf, sz, &ps) == 0) {
        stats->prefs_property_count = ps.entry_count;
        boot_kv_set_u32("entry_count", ps.entry_count);

        int windowed = twd_propset_get_bool(&ps, "Windowed", 1);
        (void)twd_kv_set("prefs", "windowed", windowed ? "1" : "0");

        float rx = 0.0f;
        float ry = 0.0f;
        int got_res = 0;
        if (!windowed) {
            got_res = (twd_propset_get_vec2(&ps, "Fullscreen Size", &rx, &ry) == 0);
        }
        if (!got_res) {
            got_res = (twd_propset_get_vec2(&ps, "Window Size", &rx, &ry) == 0);
        }
        if (got_res &&
            rx >= 320.0f && rx <= 16384.0f &&
            ry >= 200.0f && ry <= 16384.0f) {
            *io_w = (uint32_t)rx;
            *io_h = (uint32_t)ry;
            boot_kv_set_u32("width", *io_w);
            boot_kv_set_u32("height", *io_h);
        }
        const twd_prop_entry_t *brush = twd_propset_find(&ps, "FX Brush Quality");
        if (brush && brush->kind == TWD_PROP_I32 && brush->val.i32 >= 0) {
            boot_kv_set_u32("fx_brush_quality", (uint32_t)brush->val.i32);
        }
        twd_propset_free(&ps);
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
    if (st->splash_tex) {
        uint32_t sw = 0u, sh = 0u;
        twd_gfx_texture_get_size(st->splash_tex, &sw, &sh);
        if (sw > 0u && sh > 0u) {
            uint32_t dw = st->cfg.width ? st->cfg.width : 1280u;
            uint32_t dh = st->cfg.height ? st->cfg.height : 720u;
            (void)twd_gfx_draw_sprite(
                gfx, st->splash_tex, 0, 0, (int32_t)sw, (int32_t)sh,
                0, 0, (int32_t)dw, (int32_t)dh, NULL);
        }
    }
    if (st->has_boot_font) {
        static const uint8_t k_white[4] = {255u, 255u, 255u, 255u};
        const char *label = (st->stats && st->stats->active_scene[0]) ? st->stats->active_scene : "Boot";
        (void)twd_font_draw_text(gfx, &st->boot_font, 16.0f, 16.0f, 1.0f, k_white, label);
    }
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
        if (m > 0) local_stats.mounted_archives += m;
        size_t gl = strlen(game_dir);
        if (gl + sizeof("/Archives") + 1u < sizeof(dir_buf)) {
            char arch_dir[540];
            memcpy(arch_dir, game_dir, gl);
            memcpy(arch_dir + gl, "/Archives", sizeof("/Archives"));
            if (twd_path_is_dir(arch_dir)) {
                int ma = twd_ttarch2_mount_dir(arch_dir);
                if (ma > 0) local_stats.mounted_archives += ma;
            }
        }
    }
    local_stats.found_prefs_prop =
        boot_load_prefs_prop("prefs.prop", &local_stats, &w, &h) ||
        boot_load_prefs_prop("config/prefs.prop", &local_stats, &w, &h);
    {
        const char *boot_path = NULL;
        size_t boot_sz = 0u;
        void *boot_buf = twd_asset_read_alloc("_boot.lua", &boot_sz);
        if (boot_buf) {
            boot_path = "_boot.lua";
        } else {
            boot_buf = twd_asset_read_alloc("scripts/_boot.lua", &boot_sz);
            if (boot_buf) boot_path = "scripts/_boot.lua";
        }
        if (boot_buf && boot_sz > 0u) {
            local_stats.found_boot_lua = 1;
            twd_lua_vm_t *lvm = twd_lua_vm_create();
            if (lvm) {
                (void)twd_lua_vm_register(lvm, "SceneOpen", lua_scene_open, NULL);
                (void)twd_lua_vm_register(lvm, "SceneAdd", lua_scene_open, NULL);
                (void)twd_lua_vm_register(lvm, "SceneIsActive", lua_scene_is_active, NULL);
                (void)twd_lua_vm_register(lvm, "Input_IsKeyDown", lua_input_is_key_down, NULL);
                (void)twd_lua_vm_register(lvm, "CursorGetPos", lua_cursor_get_pos, NULL);
                (void)twd_lua_vm_register(lvm, "GameEngine_Quit", lua_engine_quit, NULL);
                if (twd_lua_vm_exec_buffer(lvm, boot_buf, boot_sz, boot_path) == 0) {
                    local_stats.boot_script_ran = 1;
                    local_stats.boot_scripts_executed = twd_lua_vm_executed_count(lvm);
                }
                twd_lua_vm_destroy(lvm);
            }
        }
        if (boot_buf) twd_asset_free(boot_buf);
    }
    (void)twd_kv_get("lua.scene", "active", local_stats.active_scene, sizeof(local_stats.active_scene));

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
        {
            static const char *const k_boot_tex[] = {"splash.d3dtx", "ui_boot.d3dtx"};
            for (size_t ti = 0u; ti < sizeof(k_boot_tex) / sizeof(k_boot_tex[0]); ++ti) {
                size_t tsz = 0u;
                void *tbuf = twd_asset_read_alloc(k_boot_tex[ti], &tsz);
                if (tbuf && tsz > 0u) {
                    twd_gfx_texture_t *gtex = twd_d3dtx_upload_gfx_texture(st.gfx, tbuf, tsz, NULL);
                    if (gtex) {
                        local_stats.boot_textures_loaded++;
                        if (!st.splash_tex) {
                            st.splash_tex = gtex;
                        } else {
                            twd_gfx_texture_destroy(gtex);
                        }
                    }
                }
                if (tbuf) twd_asset_free(tbuf);
            }
            size_t fsz = 0u;
            void *fbuf = twd_asset_read_alloc("ui_boot.font", &fsz);
            if (fbuf && fsz > 0u) {
                if (twd_font_parse_memory(fbuf, fsz, &st.boot_font) == 0) {
                    local_stats.boot_fonts_loaded++;
                    st.has_boot_font = 1;
                    if (st.splash_tex) {
                        (void)twd_font_attach_page(&st.boot_font, 0u, st.splash_tex);
                    }
                }
            }
            if (fbuf) twd_asset_free(fbuf);
        }
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
    if (st.gfx) {
        twd_gfx_stats_t gstats_end;
        twd_gfx_get_stats(st.gfx, &gstats_end);
        local_stats.draw_calls = gstats_end.draw_calls;
    }

    /* 6. Ordered shutdown */
    if (st.has_boot_font) twd_font_free(&st.boot_font);
    if (st.splash_tex) twd_gfx_texture_destroy(st.splash_tex);
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
