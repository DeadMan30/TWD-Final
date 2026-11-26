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

#define TWD_BOOT_MAX_SCENE_ITEMS 32u

typedef struct twd_boot_scene_item {
    twd_d3dmesh_t mesh;
    twd_d3dmesh_gpu_t gpu;
    twd_gfx_texture_t *tex;
    float world_pos[3];
    float scale[3];
    int is_bg;
    int active;
} twd_boot_scene_item_t;

typedef struct twd_boot_loop_state {
    twd_boot_config_t cfg;
    twd_boot_stats_t *stats;
    twd_window_t *win;
    twd_gfx_device_t *gfx;
    twd_audio_system_t *audio;
    twd_video_player_t *video;
    twd_scene_mgr_t *scenes;
    twd_save_mgr_t *save_mgr;
    twd_gfx_texture_t *splash_tex;
    twd_gfx_texture_t *boot_mesh_tex;
    twd_font_t boot_font;
    int has_boot_font;
    twd_d3dmesh_t boot_mesh;
    twd_d3dmesh_gpu_t boot_mesh_gpu;
    int has_boot_mesh;
    twd_boot_scene_item_t scene_items[TWD_BOOT_MAX_SCENE_ITEMS];
    uint32_t scene_item_count;
    void *d3d_dev;
    void *d3d_ctx;
    void *d3d_swap;
    int owns_gfx_direct;
    uint64_t ticks;
    uint64_t renders;
    uint64_t elapsed_ns;
    uint32_t input_events;
    int clean_shutdown;
} twd_boot_loop_state_t;

static int lua_scene_open(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    int ok = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        if (mgr && twd_scene_mgr_open(mgr, argv[0].u.str) == 0) ok = 1;
        (void)twd_kv_set("lua.scene", "active", argv[0].u.str);
        (void)twd_kv_set("lua.scene.state", argv[0].u.str, "1");
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_scene_add(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    int ok = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        if (mgr && twd_scene_mgr_add(mgr, argv[0].u.str) == 0) ok = 1;
        (void)twd_kv_set("lua.scene.state", argv[0].u.str, "1");
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_scene_is_active(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    int active = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        twd_scene_t *sc = mgr ? twd_scene_mgr_find(mgr, argv[0].u.str) : NULL;
        if (sc) {
            active = sc->active ? 1 : 0;
        } else {
            char buf[8];
            if (twd_kv_get("lua.scene.state", argv[0].u.str, buf, sizeof(buf)) > 0 && strcmp(buf, "1") == 0) {
                active = 1;
            }
        }
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = active; }
    return 0;
}

static int lua_scene_hide(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    int ok = 0;
    if (mgr && argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        int h = (argc >= 2 && argv[1].kind == TWD_LUA_T_BOOL) ? argv[1].u.b : 1;
        if (twd_scene_mgr_hide(mgr, argv[0].u.str, h) == 0) ok = 1;
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_scene_remove(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    int ok = 0;
    if (mgr && argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        if (twd_scene_mgr_remove(mgr, argv[0].u.str) == 0) ok = 1;
        (void)twd_kv_set("lua.scene.state", argv[0].u.str, "0");
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_scene_find(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    int found = (mgr && argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str &&
                 twd_scene_mgr_find(mgr, argv[0].u.str) != NULL) ? 1 : 0;
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = found; }
    return 0;
}

static int lua_scene_get_agents(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    uint32_t cnt = 0u;
    if (mgr && argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        twd_scene_t *sc = twd_scene_mgr_find(mgr, argv[0].u.str);
        if (sc) cnt = sc->agent_count;
    } else if (mgr) {
        cnt = twd_scene_mgr_total_agent_count(mgr);
    }
    if (ret) { ret->kind = TWD_LUA_T_NUM; ret->u.num = (float)cnt; }
    return 0;
}

static int lua_scene_add_agent(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    int ok = 0;
    if (mgr && argc >= 2 && argv[0].kind == TWD_LUA_T_STR && argv[1].kind == TWD_LUA_T_STR) {
        float pos[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 3 && (2 + i) < argc; ++i) {
            if (argv[2 + i].kind == TWD_LUA_T_NUM) pos[i] = argv[2 + i].u.num;
        }
        if (twd_scene_mgr_add_agent(mgr, argv[0].u.str, argv[1].u.str, pos) == 0) ok = 1;
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_agent_find(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    const char *sc = (argc >= 2 && argv[1].kind == TWD_LUA_T_STR) ? argv[1].u.str : NULL;
    int found = (mgr && argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str &&
                 twd_scene_mgr_find_agent(mgr, argv[0].u.str, sc) != NULL) ? 1 : 0;
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = found; }
    return 0;
}

static int lua_agent_set_pos(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_scene_mgr_t *mgr = (twd_scene_mgr_t *)user;
    int ok = 0;
    if (mgr && argc >= 4 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        twd_scene_agent_t *ag = twd_scene_mgr_find_agent(mgr, argv[0].u.str, NULL);
        if (ag && argv[1].kind == TWD_LUA_T_NUM && argv[2].kind == TWD_LUA_T_NUM && argv[3].kind == TWD_LUA_T_NUM) {
            ag->pos[0] = argv[1].u.num;
            ag->pos[1] = argv[2].u.num;
            ag->pos[2] = argv[3].u.num;
            ok = 1;
        }
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
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

static void boot_sync_save_stats(twd_boot_loop_state_t *st, const char *op_name) {
    if (!st || !st->save_mgr) return;
    char slot[64];
    slot[0] = '\0';
    twd_save_get_metadata(st->save_mgr, slot, sizeof(slot), NULL, 0u, NULL, 0u, NULL);
    if (st->stats) {
        st->stats->save_ops_completed++;
        size_t sl = strlen(slot);
        if (sl >= sizeof(st->stats->save_slot_active)) sl = sizeof(st->stats->save_slot_active) - 1u;
        memcpy(st->stats->save_slot_active, slot, sl);
        st->stats->save_slot_active[sl] = '\0';
    }
    if (slot[0] != '\0') (void)twd_kv_set("save", "last_slot", slot);
    if (op_name) (void)twd_kv_set("save", "last_op", op_name);
}

static int lua_save_game(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    int ok = 0;
    if (st && st->save_mgr) {
        const char *slot = (argc >= 1 && argv[0].kind == TWD_LUA_T_STR) ? argv[0].u.str : NULL;
        const char *pri = st->scenes ? twd_scene_mgr_primary_name(st->scenes) : NULL;
        if (pri && pri[0] != '\0') (void)twd_save_set_active_scene(st->save_mgr, pri);
        if (twd_save_game(st->save_mgr, slot) == 0) {
            boot_sync_save_stats(st, "save");
            ok = 1;
        }
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_load_game(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    int ok = 0;
    if (st && st->save_mgr) {
        const char *slot = (argc >= 1 && argv[0].kind == TWD_LUA_T_STR) ? argv[0].u.str : NULL;
        if (twd_load_game(st->save_mgr, slot) == 0) {
            char loaded_scene[128];
            loaded_scene[0] = '\0';
            twd_save_get_metadata(st->save_mgr, NULL, 0u, NULL, 0u, loaded_scene, sizeof(loaded_scene), NULL);
            if (loaded_scene[0] != '\0' && st->scenes && !twd_scene_mgr_find(st->scenes, loaded_scene)) {
                (void)twd_scene_mgr_open(st->scenes, loaded_scene);
            }
            boot_sync_save_stats(st, "load");
            ok = 1;
        }
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_new_game(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    int ok = 0;
    if (st && st->save_mgr) {
        uint32_t slot_idx = (argc >= 1 && argv[0].kind == TWD_LUA_T_NUM && argv[0].u.num >= 0.0f)
            ? (uint32_t)argv[0].u.num : 1u;
        if (twd_save_new_game(st->save_mgr, slot_idx) == 0) {
            boot_sync_save_stats(st, "new");
            ok = 1;
        }
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_reset_game(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)argc; (void)argv;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    int ok = (st && st->save_mgr && twd_save_reset_game(st->save_mgr) == 0) ? 1 : 0;
    if (ok) boot_sync_save_stats(st, "reset");
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_is_saving(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)argc; (void)argv;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    int saving = (st && st->save_mgr) ? twd_save_is_saving(st->save_mgr) : 0;
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = saving; }
    return 0;
}

static int lua_is_save_corrupt(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    const char *slot = (argc >= 1 && argv[0].kind == TWD_LUA_T_STR) ? argv[0].u.str : NULL;
    int corrupt = (st && st->save_mgr) ? twd_save_is_corrupt(st->save_mgr, slot) : 0;
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = corrupt; }
    return 0;
}

static int lua_save_set_display_name(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    int ok = 0;
    if (st && st->save_mgr && argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        ok = (twd_save_set_display_name(st->save_mgr, argv[0].u.str) == 0) ? 1 : 0;
        if (ok) (void)twd_kv_set("save", "display_name", argv[0].u.str);
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_save_clear_props(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)argc; (void)argv;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    if (st && st->save_mgr) twd_save_clear_props(st->save_mgr);
    if (ret) { ret->kind = TWD_LUA_T_NIL; }
    return 0;
}

static int lua_save_remove_files(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    const char *slot = (argc >= 1 && argv[0].kind == TWD_LUA_T_STR) ? argv[0].u.str : NULL;
    int ok = (st && st->save_mgr && twd_save_delete(st->save_mgr, slot) == 0) ? 1 : 0;
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_save_has_space(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user; (void)argc; (void)argv;
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = 1; }
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

static void boot_handle_event(twd_boot_loop_state_t *st, const twd_win_event_t *ev) {
    if (!st || !ev) return;
    st->input_events++;
    if (ev->kind == TWD_EVT_QUIT || ev->kind == TWD_EVT_CLOSE) {
        twd_post_quit(0);
    } else if (ev->kind == TWD_EVT_RESIZE && st->gfx && ev->param1 > 0ULL && ev->param2 > 0) {
        st->cfg.width = (uint32_t)ev->param1;
        st->cfg.height = (uint32_t)ev->param2;
        twd_gfx_set_viewport(st->gfx, 0.0f, 0.0f, (float)st->cfg.width, (float)st->cfg.height, 0.0f, 1.0f);
    } else if (ev->kind == TWD_EVT_KEY) {
        twd_input_set_key((uint32_t)ev->param1, (ev->param2 != 0) ? 1 : 0);
        if (ev->param1 == 27ULL && ev->param2 != 0) {
            twd_post_quit(0);
        }
    } else if (ev->kind == TWD_EVT_POINTER) {
        int32_t px = (int32_t)(ev->param2 >> 32);
        int32_t py = (int32_t)(uint32_t)(ev->param2 & 0xffffffffu);
        uint32_t action = (uint32_t)(ev->param1 & 0xffffffffu);
        uint32_t btn = (action == TWD_TOUCH_DOWN || action == TWD_TOUCH_MOVE) ? 1u : 0u;
        twd_input_set_pointer((float)px, (float)py, btn);
        if (action == TWD_TOUCH_DOWN && st->scenes) {
            const char *pri = twd_scene_mgr_primary_name(st->scenes);
            twd_scene_t *psc = pri ? twd_scene_mgr_find(st->scenes, pri) : NULL;
            if (psc) {
                float uy = 48.0f;
                for (uint32_t ai = 0u; ai < psc->agent_count; ++ai) {
                    if (psc->agents[ai].visible && psc->agents[ai].ui_text[0] != '\0') {
                        if ((float)py >= uy && (float)py < uy + 24.0f) {
                            (void)twd_kv_set("ui", "last_hit", psc->agents[ai].name);
                            break;
                        }
                        uy += 24.0f;
                    }
                }
            }
        }
    }
}

static void boot_on_event(void *user, const twd_win_event_t *ev) {
    boot_handle_event((twd_boot_loop_state_t *)user, ev);
}

static void boot_on_tick(void *user, uint64_t dt_ns) {
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    if (!st) return;

    st->elapsed_ns += dt_ns;
    st->ticks++;

    twd_gamepad_state_t pad;
    (void)twd_input_get_gamepad_state(0u, &pad);

    twd_win_event_t ev;
    while (twd_window_poll_event(&ev) != 0) {
        boot_handle_event(st, &ev);
    }

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

static void boot_agent_world_transform(twd_scene_t *sc, const twd_scene_agent_t *ag,
                                       int *out_visible, float out_pos[3]) {
    int vis = ag->visible ? 1 : 0;
    out_pos[0] = ag->pos[0];
    out_pos[1] = ag->pos[1];
    out_pos[2] = ag->pos[2];
    const twd_scene_agent_t *cur = ag;
    for (int depth = 0; depth < 8 && cur && cur->parent_name[0] != '\0'; ++depth) {
        twd_scene_agent_t *par = twd_scene_find_agent(sc, cur->parent_name);
        if (!par || par == cur) break;
        if (!par->visible) vis = 0;
        out_pos[0] += par->pos[0];
        out_pos[1] += par->pos[1];
        out_pos[2] += par->pos[2];
        cur = par;
    }
    *out_visible = vis;
}

static twd_gfx_texture_t *boot_load_mesh_texture(
    twd_gfx_device_t *gfx, const twd_d3dmesh_t *mesh, const char *agent_or_mesh_name) {
    char cand[2][160];
    memset(cand, 0, sizeof(cand));
    if (mesh && mesh->texture_count > 0u && mesh->textures[0][0] != '\0') {
        size_t tn = strlen(mesh->textures[0]);
        if (tn < 144u) {
            memcpy(cand[0], mesh->textures[0], tn + 1u);
            if (tn < 6u || strcmp(cand[0] + tn - 6u, ".d3dtx") != 0) {
                memcpy(cand[0] + tn, ".d3dtx", 7u);
            }
        }
    }
    if (agent_or_mesh_name && agent_or_mesh_name[0] != '\0') {
        size_t al = strlen(agent_or_mesh_name);
        if (al >= 8u && strcmp(agent_or_mesh_name + al - 8u, ".d3dmesh") == 0) {
            al -= 8u;
        }
        if (al > 0u && al < 144u) {
            memcpy(cand[1], agent_or_mesh_name, al);
            memcpy(cand[1] + al, ".d3dtx", 7u);
        }
    }
    for (int ci = 0; ci < 2; ++ci) {
        if (cand[ci][0] == '\0') continue;
        size_t mts = 0u;
        void *mtb = twd_asset_read_alloc(cand[ci], &mts);
        if (mtb && mts > 0u) {
            twd_gfx_texture_t *tex = twd_d3dtx_upload_gfx_texture(gfx, mtb, mts, NULL);
            twd_asset_free(mtb);
            if (tex) return tex;
        } else if (mtb) {
            twd_asset_free(mtb);
        }
    }
    return NULL;
}

static void boot_on_render(void *user, twd_gfx_device_t *gfx) {
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    if (!st || !gfx) return;

    static const float boot_clear_rgba[4] = {0.06f, 0.07f, 0.09f, 1.0f};
    twd_gfx_clear(gfx, boot_clear_rgba, 1.0f, 0u);
    uint32_t dw = st->cfg.width ? st->cfg.width : 1280u;
    uint32_t dh = st->cfg.height ? st->cfg.height : 720u;
    float cam_x = 0.0f;
    float cam_y = 0.0f;
    if (st->scenes) {
        const char *pri = twd_scene_mgr_primary_name(st->scenes);
        twd_scene_t *psc = pri ? twd_scene_mgr_find(st->scenes, pri) : NULL;
        if (psc) {
            for (uint32_t ai = 0u; ai < psc->agent_count; ++ai) {
                const char *aname = psc->agents[ai].name;
                if (strncmp(aname, "cam_", 4u) == 0 ||
                    strncmp(aname, "Cam", 3u) == 0 ||
                    strstr(aname, "Camera") != NULL) {
                    cam_x = psc->agents[ai].pos[0];
                    cam_y = psc->agents[ai].pos[1];
                    break;
                }
            }
        }
    }

    if (st->splash_tex && st->scene_item_count == 0u) {
        uint32_t sw = 0u, sh = 0u;
        twd_gfx_texture_get_size(st->splash_tex, &sw, &sh);
        if (sw > 0u && sh > 0u) {
            (void)twd_gfx_draw_sprite(
                gfx, st->splash_tex, 0, 0, (int32_t)sw, (int32_t)sh,
                0, 0, (int32_t)dw, (int32_t)dh, NULL);
        }
    }

    if (st->scene_item_count > 0u) {
        static const uint8_t k_ui_tint[4] = {255u, 255u, 255u, 255u};
        float ppu = (float)dh / 11.5f;
        for (uint32_t ii = 0u; ii < st->scene_item_count; ++ii) {
            const twd_boot_scene_item_t *it = &st->scene_items[ii];
            if (!it->active) continue;
            if (it->is_bg && it->tex) {
                uint32_t tw = 0u, th = 0u;
                twd_gfx_texture_get_size(it->tex, &tw, &th);
                if (tw > 0u && th > 0u) {
                    (void)twd_gfx_draw_sprite(
                        gfx, it->tex, 0, 0, (int32_t)tw, (int32_t)th,
                        0, 0, (int32_t)dw, (int32_t)dh, NULL);
                    continue;
                }
            }
            float sx = (float)dw * 0.5f + (it->world_pos[0] - cam_x) * ppu;
            float sy = (float)dh * 0.5f - (it->world_pos[1] - cam_y) * ppu;
            float sc = it->scale[0] > 0.0f ? it->scale[0] : 1.0f;
            (void)twd_d3dmesh_draw_shaded(
                gfx, &it->mesh, it->tex, sx, sy, ppu * 3.0f * sc, k_ui_tint);
        }
    } else if (st->has_boot_mesh) {
        static const uint8_t k_mesh_tint[4] = {245u, 225u, 190u, 255u};
        const twd_gfx_texture_t *mtex = st->boot_mesh_tex ? st->boot_mesh_tex : st->splash_tex;
        (void)twd_d3dmesh_draw_shaded(
            gfx, &st->boot_mesh, mtex,
            (float)dw * 0.5f - cam_x * 20.0f,
            (float)dh * 0.5f + cam_y * 20.0f,
            180.0f, k_mesh_tint);
    }
    if (st->has_boot_font) {
        static const uint8_t k_white[4] = {255u, 255u, 255u, 255u};
        const char *label = (st->stats && st->stats->active_scene[0]) ? st->stats->active_scene : "Boot";
        (void)twd_font_draw_text(gfx, &st->boot_font, 16.0f, 16.0f, 1.0f, k_white, label);
        if (st->scenes) {
            const char *pri = twd_scene_mgr_primary_name(st->scenes);
            twd_scene_t *psc = pri ? twd_scene_mgr_find(st->scenes, pri) : NULL;
            if (psc) {
                float uy = 48.0f;
                for (uint32_t ai = 0u; ai < psc->agent_count; ++ai) {
                    if (psc->agents[ai].visible && psc->agents[ai].ui_text[0] != '\0') {
                        (void)twd_font_draw_text(gfx, &st->boot_font, 24.0f, uy, 1.0f, k_white, psc->agents[ai].ui_text);
                        uy += 24.0f;
                    }
                }
            }
        }
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
    int installed_key = 0;
    if (def_cfg.archive_key && def_cfg.archive_key[0] != '\0') {
        if (twd_crypto_install_archive_key(def_cfg.archive_key, strlen(def_cfg.archive_key)) == 0) {
            installed_key = 1;
        }
    } else if (twd_crypto_install_key_from_env() == 0) {
        installed_key = 1;
    }

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
    twd_boot_loop_state_t st;
    memset(&st, 0, sizeof(st));
    st.stats = &local_stats;
    st.scenes = twd_scene_mgr_create();
    st.save_mgr = twd_save_mgr_create(def_cfg.save_dir);
    if (st.save_mgr) {
        char sdir[512];
        if (twd_save_mgr_get_dir(st.save_mgr, sdir, sizeof(sdir)) > 0) {
            (void)twd_kv_set("save", "dir", sdir);
        }
    }

    local_stats.found_prefs_prop =
        boot_load_prefs_prop("prefs.prop", &local_stats, &w, &h) ||
        boot_load_prefs_prop("config/prefs.prop", &local_stats, &w, &h);
    {
        static const char *const k_boot_scripts[] = {
            "_boot.lua", "scripts/_boot.lua", "Boot.lua", "_resdesc_50_Boot.lua"
        };
        const char *boot_path = NULL;
        size_t boot_sz = 0u;
        void *boot_buf = NULL;
        for (size_t bi = 0u; bi < sizeof(k_boot_scripts) / sizeof(k_boot_scripts[0]) && !boot_buf; ++bi) {
            boot_buf = twd_asset_read_alloc(k_boot_scripts[bi], &boot_sz);
            if (boot_buf) boot_path = k_boot_scripts[bi];
        }
        if (boot_buf && boot_sz > 0u) {
            local_stats.found_boot_lua = 1;
            twd_lua_vm_t *lvm = twd_lua_vm_create();
            if (lvm) {
                (void)twd_lua_vm_register(lvm, "SceneOpen", lua_scene_open, st.scenes);
                (void)twd_lua_vm_register(lvm, "SceneAdd", lua_scene_add, st.scenes);
                (void)twd_lua_vm_register(lvm, "SceneIsActive", lua_scene_is_active, st.scenes);
                (void)twd_lua_vm_register(lvm, "SceneHide", lua_scene_hide, st.scenes);
                (void)twd_lua_vm_register(lvm, "SceneRemove", lua_scene_remove, st.scenes);
                (void)twd_lua_vm_register(lvm, "SceneFind", lua_scene_find, st.scenes);
                (void)twd_lua_vm_register(lvm, "SceneGetAgents", lua_scene_get_agents, st.scenes);
                (void)twd_lua_vm_register(lvm, "SceneAddAgent", lua_scene_add_agent, st.scenes);
                (void)twd_lua_vm_register(lvm, "AgentFind", lua_agent_find, st.scenes);
                (void)twd_lua_vm_register(lvm, "AgentSetPos", lua_agent_set_pos, st.scenes);
                (void)twd_lua_vm_register(lvm, "Input_IsKeyDown", lua_input_is_key_down, NULL);
                (void)twd_lua_vm_register(lvm, "CursorGetPos", lua_cursor_get_pos, NULL);
                (void)twd_lua_vm_register(lvm, "GameEngine_Quit", lua_engine_quit, NULL);
                (void)twd_lua_vm_register(lvm, "SaveGame", lua_save_game, &st);
                (void)twd_lua_vm_register(lvm, "LoadGame", lua_load_game, &st);
                (void)twd_lua_vm_register(lvm, "NewGame", lua_new_game, &st);
                (void)twd_lua_vm_register(lvm, "ResetGame", lua_reset_game, &st);
                (void)twd_lua_vm_register(lvm, "IsSaving", lua_is_saving, &st);
                (void)twd_lua_vm_register(lvm, "IsSaveGameCorrupt", lua_is_save_corrupt, &st);
                (void)twd_lua_vm_register(lvm, "SaveLoadSetSaveDisplayName", lua_save_set_display_name, &st);
                (void)twd_lua_vm_register(lvm, "SaveLoadClearRuntimeProperties", lua_save_clear_props, &st);
                (void)twd_lua_vm_register(lvm, "SaveLoadRemoveUserFiles", lua_save_remove_files, &st);
                (void)twd_lua_vm_register(lvm, "SaveLoadHasAvailableSpace", lua_save_has_space, &st);
                if (twd_lua_vm_exec_buffer(lvm, boot_buf, boot_sz, boot_path) == 0) {
                    local_stats.boot_script_ran = 1;
                    local_stats.boot_scripts_executed = twd_lua_vm_executed_count(lvm);
                }
                twd_lua_vm_destroy(lvm);
            }
        }
        if (boot_buf) twd_asset_free(boot_buf);
    }
    if (st.scenes) {
        if (twd_scene_mgr_scene_count(st.scenes) == 0u) {
            size_t usz = 0u;
            void *ubuf = twd_asset_read_alloc("ui_boot.scene", &usz);
            if (ubuf) {
                twd_asset_free(ubuf);
                (void)twd_scene_mgr_open(st.scenes, "ui_boot.scene");
            }
        }
        local_stats.boot_scenes_active = twd_scene_mgr_scene_count(st.scenes);
        local_stats.boot_scene_agents = twd_scene_mgr_total_agent_count(st.scenes);
        const char *primary = twd_scene_mgr_primary_name(st.scenes);
        if (primary) {
            size_t plen = strlen(primary);
            if (plen >= sizeof(local_stats.active_scene)) plen = sizeof(local_stats.active_scene) - 1u;
            memcpy(local_stats.active_scene, primary, plen);
            local_stats.active_scene[plen] = '\0';
        }
    }
    if (local_stats.active_scene[0] == '\0') {
        (void)twd_kv_get("lua.scene", "active", local_stats.active_scene, sizeof(local_stats.active_scene));
    }

    /* 3. Create window + graphics device / DXBC shader cache */
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
            static const char *const k_boot_tex[] = {
                "splash.d3dtx", "ui_boot.d3dtx", "ui_boot_titleBackground.d3dtx", "ui_boot_title.d3dtx"
            };
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

            if (st.scenes) {
                const char *pri = twd_scene_mgr_primary_name(st.scenes);
                twd_scene_t *psc = pri ? twd_scene_mgr_find(st.scenes, pri) : NULL;
                if (psc) {
                    for (uint32_t ai = 0u; ai < psc->agent_count && st.scene_item_count < TWD_BOOT_MAX_SCENE_ITEMS; ++ai) {
                        const twd_scene_agent_t *ag = &psc->agents[ai];
                        if (ag->render_mesh[0] == '\0') continue;
                        int eff_vis = 1;
                        float wpos[3];
                        boot_agent_world_transform(psc, ag, &eff_vis, wpos);
                        if (!eff_vis) continue;

                        size_t msz = 0u;
                        void *mbuf = twd_asset_read_alloc(ag->render_mesh, &msz);
                        if (mbuf && msz > 0u) {
                            twd_boot_scene_item_t *it = &st.scene_items[st.scene_item_count];
                            memset(it, 0, sizeof(*it));
                            if (twd_d3dmesh_parse_memory(mbuf, msz, &it->mesh) == 0 &&
                                twd_d3dmesh_upload_gfx(st.gfx, &it->mesh, &it->gpu) == 0) {
                                it->tex = boot_load_mesh_texture(st.gfx, &it->mesh, ag->name);
                                if (it->tex) local_stats.boot_textures_loaded++;
                                it->world_pos[0] = wpos[0];
                                it->world_pos[1] = wpos[1];
                                it->world_pos[2] = wpos[2];
                                it->scale[0] = ag->scale[0] > 0.0f ? ag->scale[0] : 1.0f;
                                it->scale[1] = ag->scale[1] > 0.0f ? ag->scale[1] : 1.0f;
                                it->scale[2] = ag->scale[2] > 0.0f ? ag->scale[2] : 1.0f;
                                it->is_bg = (strstr(ag->name, "Background") != NULL || wpos[2] <= -8.0f) ? 1 : 0;
                                it->active = 1;
                                st.scene_item_count++;
                                local_stats.boot_meshes_loaded++;
                            } else {
                                twd_d3dmesh_free(&it->mesh);
                            }
                        }
                        if (mbuf) twd_asset_free(mbuf);
                    }
                    /* Sort visible scene items back-to-front by world Z */
                    for (uint32_t i = 1u; i < st.scene_item_count; ++i) {
                        twd_boot_scene_item_t key = st.scene_items[i];
                        uint32_t j = i;
                        while (j > 0u && st.scene_items[j - 1u].world_pos[2] > key.world_pos[2]) {
                            st.scene_items[j] = st.scene_items[j - 1u];
                            j--;
                        }
                        st.scene_items[j] = key;
                    }
                }
            }

            if (st.scene_item_count == 0u) {
                const char *mesh_cand[4] = {NULL, "ui_boot.d3dmesh", "ui_boot_logoTTG.d3dmesh", "ui_boot_logoSkybound.d3dmesh"};
                if (st.scenes) {
                    const char *pri = twd_scene_mgr_primary_name(st.scenes);
                    twd_scene_t *psc = pri ? twd_scene_mgr_find(st.scenes, pri) : NULL;
                    if (psc) {
                        for (uint32_t ai = 0u; ai < psc->agent_count; ++ai) {
                            if (psc->agents[ai].visible && psc->agents[ai].render_mesh[0] != '\0') {
                                mesh_cand[0] = psc->agents[ai].render_mesh;
                                break;
                            }
                        }
                    }
                }
                for (size_t mi = 0u; mi < 4u && !st.has_boot_mesh; ++mi) {
                    if (!mesh_cand[mi]) continue;
                    size_t msz = 0u;
                    void *mbuf = twd_asset_read_alloc(mesh_cand[mi], &msz);
                    if (mbuf && msz > 0u) {
                        if (twd_d3dmesh_parse_memory(mbuf, msz, &st.boot_mesh) == 0) {
                            if (twd_d3dmesh_upload_gfx(st.gfx, &st.boot_mesh, &st.boot_mesh_gpu) == 0) {
                                local_stats.boot_meshes_loaded++;
                                st.has_boot_mesh = 1;
                                st.boot_mesh_tex = boot_load_mesh_texture(st.gfx, &st.boot_mesh, mesh_cand[mi]);
                                if (st.boot_mesh_tex) local_stats.boot_textures_loaded++;
                            } else {
                                twd_d3dmesh_free(&st.boot_mesh);
                            }
                        }
                    }
                    if (mbuf) twd_asset_free(mbuf);
                }
            }
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
    cb.on_event = boot_on_event;
    cb.user = &st;
    cb.max_frames = def_cfg.max_frames;

    twd_clear_quit();
    local_stats.frames_rendered = twd_android_run_frame_loop(st.win, st.gfx, &cb);
    local_stats.elapsed_ns = st.elapsed_ns;
    local_stats.input_events_processed = st.input_events;
    local_stats.clean_shutdown = st.clean_shutdown;
    if (st.gfx) {
        twd_gfx_stats_t gstats_end;
        twd_gfx_get_stats(st.gfx, &gstats_end);
        local_stats.draw_calls = gstats_end.draw_calls;
    }

    /* 6. Ordered shutdown */
    for (uint32_t ii = 0u; ii < st.scene_item_count; ++ii) {
        if (st.scene_items[ii].tex) twd_gfx_texture_destroy(st.scene_items[ii].tex);
        twd_d3dmesh_gpu_free(&st.scene_items[ii].gpu);
        twd_d3dmesh_free(&st.scene_items[ii].mesh);
    }
    if (st.has_boot_mesh) {
        twd_d3dmesh_gpu_free(&st.boot_mesh_gpu);
        twd_d3dmesh_free(&st.boot_mesh);
    }
    if (st.has_boot_font) twd_font_free(&st.boot_font);
    if (st.boot_mesh_tex) twd_gfx_texture_destroy(st.boot_mesh_tex);
    if (st.splash_tex) twd_gfx_texture_destroy(st.splash_tex);
    if (st.save_mgr) twd_save_mgr_destroy(st.save_mgr);
    if (st.scenes) twd_scene_mgr_destroy(st.scenes);
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
    if (installed_key) twd_crypto_clear_archive_key();
    (void)twd_media_shutdown();
    twd_audio_shutdown();
    twd_oodle_shutdown();
    twd_net_shutdown();
    twd_com_uninit();
    twd_platform_shutdown();

    if (out_stats) *out_stats = local_stats;
    return st.clean_shutdown ? 0 : -1;
}
