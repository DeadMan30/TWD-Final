/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Critical-path engine boot, VFS archive mount, renderer/audio initialization,
 * and frame loop bring-up connecting game hooks to the portable runtime PAL
 * (WinMain 0x14002f7b0 -> EngineInit 0x14006d450 -> ScriptBoot 0x14006d250
 *  -> RenderDeviceInit 0x1405d9f10 -> GameEngine::Loop 0x140067fd0).
 */
#include "twd_game.h"
#include "twd_runtime.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWD_BOOT_MAX_SCENE_ITEMS 48u
#define TWD_BOOT_MAX_MENU_BTNS   8u

typedef struct twd_boot_menu_btn {
    char label[64];
    char action[32];
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} twd_boot_menu_btn_t;

typedef struct twd_boot_scene_item {
    twd_d3dmesh_t mesh;
    twd_d3dmesh_gpu_t gpu;
    twd_gfx_texture_t *batch_tex[TWD_D3DMESH_MAX_TEXTURES];
    uint32_t batch_tex_count;
    char agent_name[64];
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
    twd_gfx_texture_t *boot_font_page_tex;
    twd_font_t boot_font;
    int has_boot_font;
    twd_font_t menu_font;
    int has_menu_font;
    twd_landb_t boot_landb;
    int has_boot_landb;
    twd_landb_t menu_landb;
    int has_menu_landb;
    twd_landb_t scene_landb;
    int has_scene_landb;
    uint32_t dlg_cursor;
    uint32_t dlg_step;
    char dlg_line[160];
    char dlg_node[64];
    char dlg_last_choice[64];
    int dlg_choices_open;
    twd_d3dmesh_t boot_mesh;
    twd_d3dmesh_gpu_t boot_mesh_gpu;
    int has_boot_mesh;
    twd_boot_scene_item_t scene_items[TWD_BOOT_MAX_SCENE_ITEMS];
    uint32_t scene_item_count;
#define TWD_MENU_PAGE_MAIN     0
#define TWD_MENU_PAGE_EPISODES 1
#define TWD_MENU_PAGE_SETTINGS 2

    twd_boot_menu_btn_t menu_btns[TWD_BOOT_MAX_MENU_BTNS];
    uint32_t menu_btn_count;
    int32_t menu_focus;
    int menu_page;
    int menu_script_ran;
    int opt_subtitles;
    int opt_windowed;
    int opt_fx_quality;
    float cam_pan_x;
    float cam_pan_y;
    float cam_pan_z;
    float cam_yaw;
    float cam_pitch;
    char selected_agent[64];
    char menu_status[96];
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

typedef struct boot_lua_ctx {
    char required[128][64];
    uint32_t required_count;
} boot_lua_ctx_t;

static int lua_ret_const_bool(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)argc; (void)argv;
    if (ret) {
        ret->kind = TWD_LUA_T_BOOL;
        ret->u.b = (user != NULL) ? 1 : 0;
    }
    return 0;
}

static int lua_ret_nil(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user; (void)argc; (void)argv;
    if (ret) ret->kind = TWD_LUA_T_NIL;
    return 0;
}

static int lua_resource_exists(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user;
    int ex = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        ex = twd_ttarch2_exists_mounted(argv[0].u.str);
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ex; }
    return 0;
}

static int boot_exec_lua_name(twd_lua_vm_t *vm, const char *name) {
    if (!vm || !name || name[0] == '\0') return -1;
    if (twd_lua_vm_exec_vfs(vm, name) == 0) return 0;
    size_t nl = strlen(name);
    if ((nl < 4u || strcmp(name + nl - 4u, ".lua") != 0) && nl + 5u < 160u) {
        char with_ext[160];
        memcpy(with_ext, name, nl);
        memcpy(with_ext + nl, ".lua", 5u);
        if (twd_lua_vm_exec_vfs(vm, with_ext) == 0) return 0;
    }
    return -1;
}

static int lua_load_script(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)user;
    int ok = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        if (boot_exec_lua_name(vm, argv[0].u.str) == 0) ok = 1;
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_game_mode(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)user;
    int ok = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        (void)twd_kv_set("lua.game", "mode", argv[0].u.str);
        if (boot_exec_lua_name(vm, argv[0].u.str) == 0) ok = 1;
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
    return 0;
}

static int lua_require(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    boot_lua_ctx_t *ctx = (boot_lua_ctx_t *)user;
    int ok = 0;
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str && argv[0].u.str[0] != '\0') {
        const char *m = argv[0].u.str;
        int already = 0;
        if (ctx) {
            for (uint32_t i = 0u; i < ctx->required_count; ++i) {
                if (strcmp(ctx->required[i], m) == 0) {
                    already = 1;
                    break;
                }
            }
            if (!already && ctx->required_count < 128u) {
                size_t ml = strlen(m);
                if (ml >= sizeof(ctx->required[0])) ml = sizeof(ctx->required[0]) - 1u;
                memcpy(ctx->required[ctx->required_count], m, ml);
                ctx->required[ctx->required_count][ml] = '\0';
                ctx->required_count++;
            }
        }
        ok = already ? 1 : (boot_exec_lua_name(vm, m) == 0 ? 1 : 0);
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = ok; }
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
        (void)twd_audio_load_bank_vfs(st->audio, "Master Bank.bank");
        (void)twd_audio_load_bank_vfs(st->audio, "Master Bank.strings.bank");
        twd_audio_bank_t *mm = twd_audio_load_bank_vfs(st->audio, "CommonMenuMusic.bank");
        if (!mm) mm = twd_audio_load_bank_vfs(st->audio, "Master.bank");
        if (mm && twd_audio_play_bank_event(st->audio, mm, 0u, 0.35f) == 0) {
            (void)twd_kv_set("audio", "menu_music_started", "1");
        }
    }
    twd_kv_set("Engine", "BootPhase", "LoopActive");
    return 0;
}

static void boot_switch_to_scene(twd_boot_loop_state_t *st, const char *scene_name);
static void boot_layout_menu_buttons(twd_boot_loop_state_t *st);
static void boot_populate_menu_buttons(twd_boot_loop_state_t *st, const char *scene_name);

static int boot_is_gameplay_3d(const twd_boot_loop_state_t *st, const char *scene_name) {
    if (!st || !scene_name || scene_name[0] == '\0') return 0;
    if (st->menu_btn_count > 0u) return 0;
    if (strncmp(scene_name, "ui_", 3u) == 0) return 0;
    if (strcmp(scene_name, "Menu.scene") == 0 || strcmp(scene_name, "Boot.scene") == 0) return 0;
    return 1;
}

static void boot_get_scene_camera(const twd_boot_loop_state_t *st, float out_pos[3],
                                  float *out_yaw, float *out_pitch) {
    float cx = 0.0f, cy = 1.5f, cz = 6.0f;
    int found = 0;
    if (st && st->scenes) {
        const char *pri = twd_scene_mgr_primary_name(st->scenes);
        twd_scene_t *psc = pri ? twd_scene_mgr_find(st->scenes, pri) : NULL;
        if (psc) {
            for (uint32_t ai = 0u; ai < psc->agent_count; ++ai) {
                const char *aname = psc->agents[ai].name;
                if ((strncmp(aname, "cam_", 4u) == 0 || strncmp(aname, "Cam", 3u) == 0 ||
                     strstr(aname, "Camera") != NULL) &&
                    (fabsf(psc->agents[ai].pos[0]) > 1e-4f ||
                     fabsf(psc->agents[ai].pos[1]) > 1e-4f ||
                     fabsf(psc->agents[ai].pos[2]) > 1e-4f)) {
                    cx = psc->agents[ai].pos[0];
                    cy = psc->agents[ai].pos[1];
                    cz = psc->agents[ai].pos[2];
                    found = 1;
                    break;
                }
            }
            if (!found) {
                for (uint32_t ai = 0u; ai < psc->agent_count; ++ai) {
                    const char *aname = psc->agents[ai].name;
                    if (strncmp(aname, "cam_", 4u) == 0 || strncmp(aname, "Cam", 3u) == 0 ||
                        strstr(aname, "Camera") != NULL) {
                        cx = psc->agents[ai].pos[0];
                        cy = psc->agents[ai].pos[1];
                        cz = psc->agents[ai].pos[2];
                        break;
                    }
                }
            }
        }
    }
    out_pos[0] = cx + (st ? st->cam_pan_x : 0.0f);
    out_pos[1] = cy + (st ? st->cam_pan_y : 0.0f);
    out_pos[2] = cz + (st ? st->cam_pan_z : 0.0f);
    if (out_yaw) *out_yaw = st ? st->cam_yaw : 0.0f;
    if (out_pitch) *out_pitch = st ? st->cam_pitch : 0.0f;
}

static void boot_launch_episode(twd_boot_loop_state_t *st, int ep_idx) {
    static const char *const k_ep_cands[4][3] = {
        {"adv_trainStation.scene", "adv_boardingSchoolExterior.scene", "adv_roadTile.scene"},
        {"adv_boardingSchoolDorm.scene", "adv_boardingSchoolExteriorNight.scene", "adv_forestShack.scene"},
        {"adv_boardingSchoolBasement.scene", "adv_forestShack.scene", "adv_trainStation.scene"},
        {"adv_forestTile.scene", "adv_roadTile.scene", "adv_boardingSchoolExterior.scene"}
    };
    if (!st || ep_idx < 0 || ep_idx >= 4) return;
    for (int ci = 0; ci < 3; ++ci) {
        const char *sc = k_ep_cands[ep_idx][ci];
        if (sc && twd_ttarch2_exists_mounted(sc)) {
            st->cam_pan_x = 0.0f;
            st->cam_pan_y = 0.0f;
            st->cam_pan_z = 0.0f;
            st->cam_yaw = 0.0f;
            st->cam_pitch = 0.0f;
            st->selected_agent[0] = '\0';
            st->menu_status[0] = '\0';
            boot_switch_to_scene(st, sc);
            return;
        }
    }
    memcpy(st->menu_status, "Ready: Episode 1 - Done Running", 32u);
}

static void boot_activate_menu_button(twd_boot_loop_state_t *st, uint32_t idx) {
    if (!st || idx >= st->menu_btn_count) return;
    const twd_boot_menu_btn_t *btn = &st->menu_btns[idx];
    char action[32];
    char label[64];
    memcpy(action, btn->action, sizeof(action));
    memcpy(label, btn->label, sizeof(label));
    st->menu_focus = (int32_t)idx;
    (void)twd_kv_set("ui", "last_hit", action);
    (void)twd_kv_set("ui", "last_button_label", label);
    (void)twd_kv_set("ui", "menu_action", action);

    if (strcmp(action, "play") == 0) {
        if (st->save_mgr) (void)twd_save_new_game(st->save_mgr, 1u);
        boot_launch_episode(st, 0);
    } else if (strcmp(action, "episodes") == 0) {
        memcpy(st->menu_status, "Select Episode (1-4)", 21u);
        if (twd_ttarch2_exists_mounted("ui_episodes.scene")) {
            st->menu_page = TWD_MENU_PAGE_EPISODES;
            st->menu_focus = 0;
            boot_switch_to_scene(st, "ui_episodes.scene");
        }
    } else if (strcmp(action, "ep1") == 0) {
        boot_launch_episode(st, 0);
    } else if (strcmp(action, "ep2") == 0) {
        boot_launch_episode(st, 1);
    } else if (strcmp(action, "ep3") == 0) {
        boot_launch_episode(st, 2);
    } else if (strcmp(action, "ep4") == 0) {
        boot_launch_episode(st, 3);
    } else if (strcmp(action, "settings") == 0) {
        st->menu_page = TWD_MENU_PAGE_SETTINGS;
        st->menu_focus = 0;
        memcpy(st->menu_status, "Settings", 9u);
        boot_populate_menu_buttons(st, "ui_menuMain.scene");
    } else if (strcmp(action, "opt_subtitles") == 0) {
        st->opt_subtitles = st->opt_subtitles ? 0 : 1;
        (void)twd_kv_set("prefs", "subtitles", st->opt_subtitles ? "1" : "0");
        boot_populate_menu_buttons(st, "ui_menuMain.scene");
    } else if (strcmp(action, "opt_windowed") == 0) {
        st->opt_windowed = st->opt_windowed ? 0 : 1;
        (void)twd_kv_set("prefs", "windowed", st->opt_windowed ? "1" : "0");
        boot_populate_menu_buttons(st, "ui_menuMain.scene");
    } else if (strcmp(action, "opt_fx") == 0) {
        st->opt_fx_quality = (st->opt_fx_quality + 1) % 3;
        boot_kv_set_u32("fx_brush_quality", (uint32_t)st->opt_fx_quality);
        boot_populate_menu_buttons(st, "ui_menuMain.scene");
    } else if (strcmp(action, "back_main") == 0) {
        st->menu_page = TWD_MENU_PAGE_MAIN;
        st->menu_focus = 0;
        st->menu_status[0] = '\0';
        if (st->stats && strcmp(st->stats->active_scene, "ui_menuMain.scene") != 0 &&
            twd_ttarch2_exists_mounted("ui_menuMain.scene")) {
            boot_switch_to_scene(st, "ui_menuMain.scene");
        } else {
            boot_populate_menu_buttons(st, "ui_menuMain.scene");
        }
    } else if (strcmp(action, "quit") == 0) {
        memcpy(st->menu_status, "Exiting...", 11u);
        twd_post_quit(0);
    }
}

static void boot_copy_trunc(char *dst, size_t cap, const char *src);
static void boot_advance_dialogue(twd_boot_loop_state_t *st, int step_forward);

static void boot_handle_event(twd_boot_loop_state_t *st, const twd_win_event_t *ev) {
    if (!st || !ev) return;
    st->input_events++;
    if (ev->kind == TWD_EVT_QUIT || ev->kind == TWD_EVT_CLOSE) {
        twd_post_quit(0);
    } else if (ev->kind == TWD_EVT_RESIZE && st->gfx && ev->param1 > 0ULL && ev->param2 > 0) {
        st->cfg.width = (uint32_t)ev->param1;
        st->cfg.height = (uint32_t)ev->param2;
        (void)twd_gfx_resize(st->gfx, st->cfg.width, st->cfg.height);
        twd_gfx_set_viewport(st->gfx, 0.0f, 0.0f, (float)st->cfg.width, (float)st->cfg.height, 0.0f, 1.0f);
        boot_layout_menu_buttons(st);
    } else if (ev->kind == TWD_EVT_KEY) {
        twd_input_set_key((uint32_t)ev->param1, (ev->param2 != 0) ? 1 : 0);
        if (ev->param2 != 0) {
            if (ev->param1 == 27ULL) {
                twd_post_quit(0);
            } else if ((ev->param1 == 13ULL || ev->param1 == 32ULL) && st->scenes && st->stats &&
                       strcmp(st->stats->active_scene, "ui_boot.scene") == 0 &&
                       twd_ttarch2_exists_mounted("ui_menuMain.scene")) {
                st->menu_page = TWD_MENU_PAGE_MAIN;
                st->menu_focus = 0;
                boot_switch_to_scene(st, "ui_menuMain.scene");
            } else if (st->menu_btn_count > 0u) {
                if (ev->param1 == 38ULL) {
                    st->menu_focus = (st->menu_focus > 0)
                        ? (st->menu_focus - 1) : (int32_t)(st->menu_btn_count - 1u);
                } else if (ev->param1 == 40ULL) {
                    st->menu_focus = (st->menu_focus + 1) % (int32_t)st->menu_btn_count;
                } else if (ev->param1 == 13ULL || ev->param1 == 32ULL) {
                    uint32_t fi = (st->menu_focus >= 0 && (uint32_t)st->menu_focus < st->menu_btn_count)
                        ? (uint32_t)st->menu_focus : 0u;
                    boot_activate_menu_button(st, fi);
                } else if (ev->param1 == 8ULL) {
                    if (st->menu_page == TWD_MENU_PAGE_SETTINGS) {
                        st->menu_page = TWD_MENU_PAGE_MAIN;
                        st->menu_focus = 0;
                        boot_populate_menu_buttons(st, "ui_menuMain.scene");
                    } else if (st->stats && strcmp(st->stats->active_scene, "ui_episodes.scene") == 0 &&
                               twd_ttarch2_exists_mounted("ui_menuMain.scene")) {
                        st->menu_page = TWD_MENU_PAGE_MAIN;
                        st->menu_focus = 0;
                        boot_switch_to_scene(st, "ui_menuMain.scene");
                    } else if (st->stats && strcmp(st->stats->active_scene, "ui_menuMain.scene") == 0 &&
                               twd_ttarch2_exists_mounted("ui_boot.scene")) {
                        boot_switch_to_scene(st, "ui_boot.scene");
                    }
                }
            } else if (st->scenes && st->stats && strcmp(st->stats->active_scene, "ui_boot.scene") != 0) {
                /* Gameplay 3D camera controls & back navigation */
                if (ev->param1 == 65ULL || ev->param1 == 37ULL) {
                    st->cam_pan_x -= 0.5f;
                    (void)twd_kv_set("scene", "cam_panned", "1");
                } else if (ev->param1 == 68ULL || ev->param1 == 39ULL) {
                    st->cam_pan_x += 0.5f;
                    (void)twd_kv_set("scene", "cam_panned", "1");
                } else if (ev->param1 == 87ULL || ev->param1 == 38ULL) {
                    st->cam_pan_z -= 0.5f;
                    st->cam_pan_y += 0.25f;
                    (void)twd_kv_set("scene", "cam_panned", "1");
                } else if (ev->param1 == 83ULL || ev->param1 == 40ULL) {
                    st->cam_pan_z += 0.5f;
                    st->cam_pan_y -= 0.25f;
                    (void)twd_kv_set("scene", "cam_panned", "1");
                } else if (ev->param1 == 81ULL) {
                    st->cam_yaw -= 0.12f;
                    (void)twd_kv_set("scene", "cam_panned", "1");
                } else if (ev->param1 == 69ULL) {
                    st->cam_yaw += 0.12f;
                    (void)twd_kv_set("scene", "cam_panned", "1");
                } else if (ev->param1 == 82ULL) {
                    st->cam_pan_x = 0.0f;
                    st->cam_pan_y = 0.0f;
                    st->cam_pan_z = 0.0f;
                    st->cam_yaw = 0.0f;
                    st->cam_pitch = 0.0f;
                } else if (ev->param1 == 32ULL || ev->param1 == 78ULL) {
                    boot_advance_dialogue(st, 1);
                } else if (st->dlg_choices_open && ev->param1 >= 49ULL && ev->param1 <= 51ULL) {
                    static const char *const k_choices[3] = {"Help AJ", "Check Area", "Stay Silent"};
                    uint32_t ci = (uint32_t)(ev->param1 - 49ULL);
                    boot_copy_trunc(st->dlg_last_choice, sizeof(st->dlg_last_choice), k_choices[ci]);
                    (void)twd_kv_set("scene", "last_choice", st->dlg_last_choice);
                    st->dlg_choices_open = 0;
                    boot_advance_dialogue(st, 1);
                } else if (ev->param1 == 8ULL) {
                    if (twd_ttarch2_exists_mounted("ui_episodes.scene")) {
                        st->menu_page = TWD_MENU_PAGE_EPISODES;
                        boot_switch_to_scene(st, "ui_episodes.scene");
                    } else if (twd_ttarch2_exists_mounted("ui_menuMain.scene")) {
                        st->menu_page = TWD_MENU_PAGE_MAIN;
                        boot_switch_to_scene(st, "ui_menuMain.scene");
                    }
                }
            }
        }
    } else if (ev->kind == TWD_EVT_POINTER) {
        int32_t px = (int32_t)(ev->param2 >> 32);
        int32_t py = (int32_t)(uint32_t)(ev->param2 & 0xffffffffu);
        uint32_t action = (uint32_t)(ev->param1 & 0xffffffffu);
        uint32_t btn = (action == TWD_TOUCH_DOWN || action == TWD_TOUCH_MOVE) ? 1u : 0u;
        twd_input_set_pointer((float)px, (float)py, btn);
        if (st->menu_btn_count > 0u) {
            for (uint32_t bi = 0u; bi < st->menu_btn_count; ++bi) {
                const twd_boot_menu_btn_t *b = &st->menu_btns[bi];
                if (px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h) {
                    st->menu_focus = (int32_t)bi;
                    if (action == TWD_TOUCH_DOWN) {
                        boot_activate_menu_button(st, bi);
                    }
                    return;
                }
            }
        }
        if (action == TWD_TOUCH_DOWN && st->scenes) {
            if (st->stats && strcmp(st->stats->active_scene, "ui_boot.scene") == 0 &&
                twd_ttarch2_exists_mounted("ui_menuMain.scene")) {
                st->menu_page = TWD_MENU_PAGE_MAIN;
                st->menu_focus = 0;
                boot_switch_to_scene(st, "ui_menuMain.scene");
                return;
            }
            if (st->stats && boot_is_gameplay_3d(st, st->stats->active_scene)) {
                if (st->opt_subtitles && (st->dlg_line[0] != '\0' || st->dlg_choices_open)) {
                    uint32_t dh = st->cfg.height ? st->cfg.height : 720u;
                    uint32_t dw = st->cfg.width ? st->cfg.width : 1280u;
                    int32_t bar_h = st->dlg_choices_open ? 96 : 64;
                    int32_t bar_y = (int32_t)dh - bar_h - 12;
                    if (bar_y < 80) bar_y = 80;
                    if (px >= 24 && px < (int32_t)dw - 24 && py >= bar_y && py < bar_y + bar_h) {
                        if (st->dlg_choices_open) {
                            static const char *const k_tchoices[3] = {"Help AJ", "Check Area", "Stay Silent"};
                            int32_t bar_w = (int32_t)dw - 48;
                            int32_t rel_x = px - 24;
                            uint32_t ci = (bar_w > 0) ? (uint32_t)((rel_x * 3) / bar_w) : 0u;
                            if (ci > 2u) ci = 2u;
                            boot_copy_trunc(st->dlg_last_choice, sizeof(st->dlg_last_choice), k_tchoices[ci]);
                            (void)twd_kv_set("scene", "last_choice", st->dlg_last_choice);
                            st->dlg_choices_open = 0;
                        }
                        if (st->has_scene_landb) {
                            boot_advance_dialogue(st, 1);
                        }
                        return;
                    }
                }
            }
            if (st->stats && boot_is_gameplay_3d(st, st->stats->active_scene) &&
                st->scene_item_count > 0u) {
                float cp[3], cyaw = 0.0f, cpitch = 0.0f;
                boot_get_scene_camera(st, cp, &cyaw, &cpitch);
                uint32_t dw = st->cfg.width ? st->cfg.width : 1280u;
                uint32_t dh = st->cfg.height ? st->cfg.height : 720u;
                float cy = cosf(cyaw), sy = sinf(cyaw);
                float cpi = cosf(cpitch), spi = sinf(cpitch);
                float rx = cy, rz = sy;
                float fx = sy * cpi, fy = spi, fz = -cy * cpi;
                float ux = -sy * spi, uy = cpi, uz = cy * spi;
                float focal = ((float)dh * 0.5f) / tanf(55.0f * 0.5f * (3.14159265f / 180.0f));
                float best_d2 = 1e30f;
                int best_idx = -1;
                for (uint32_t ii = 0u; ii < st->scene_item_count; ++ii) {
                    const twd_boot_scene_item_t *it = &st->scene_items[ii];
                    if (!it->active || it->is_bg || it->agent_name[0] == '\0') continue;
                    float dx = it->world_pos[0] - cp[0];
                    float dy = it->world_pos[1] - cp[1];
                    float dz = it->world_pos[2] - cp[2];
                    float vz = dx * fx + dy * fy + dz * fz;
                    if (vz < 0.08f) continue;
                    float vx = dx * rx + dz * rz;
                    float vy = dx * ux + dy * uy + dz * uz;
                    float sx = (float)dw * 0.5f + (vx / vz) * focal;
                    float sy_s = (float)dh * 0.5f - (vy / vz) * focal;
                    float d2 = (sx - (float)px) * (sx - (float)px) + (sy_s - (float)py) * (sy_s - (float)py);
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        best_idx = (int)ii;
                    }
                }
                if (best_idx >= 0) {
                    size_t nl = strlen(st->scene_items[best_idx].agent_name);
                    if (nl >= sizeof(st->selected_agent)) nl = sizeof(st->selected_agent) - 1u;
                    memcpy(st->selected_agent, st->scene_items[best_idx].agent_name, nl);
                    st->selected_agent[nl] = '\0';
                    (void)twd_kv_set("ui", "last_hit", st->selected_agent);
                    if (st->has_scene_landb) {
                        boot_advance_dialogue(st, 1);
                    }
                    return;
                }
            }
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

static twd_gfx_texture_t *boot_upload_d3dtx_vfs(twd_gfx_device_t *gfx, const char *name) {
    size_t mts = 0u;
    void *mtb = twd_asset_read_alloc(name, &mts);
    if (!mtb) return NULL;
    twd_gfx_texture_t *tex = (mts > 0u) ? twd_d3dtx_upload_gfx_texture(gfx, mtb, mts, NULL) : NULL;
    twd_asset_free(mtb);
    return tex;
}

static uint32_t boot_load_mesh_textures(
    twd_gfx_device_t *gfx,
    const twd_d3dmesh_t *mesh,
    const char *agent_or_mesh_name,
    twd_gfx_texture_t **out_texs,
    uint32_t max_texs,
    uint32_t *out_uploaded) {
    uint32_t highest = 0u, uploaded = 0u;
    if (out_uploaded) *out_uploaded = 0u;
    if (!gfx || !out_texs || max_texs == 0u) return 0u;
    for (uint32_t ti = 0u; ti < max_texs; ++ti) out_texs[ti] = NULL;

    if (mesh) {
        for (uint32_t ti = 0u; ti < mesh->texture_count && ti < max_texs; ++ti) {
            if (mesh->texture_crcs[ti] == 0ULL) continue;
            size_t mts = 0u;
            void *mtb = twd_ttarch2_lookup_mounted_crc64_alloc(mesh->texture_crcs[ti], &mts);
            if (mtb) {
                twd_gfx_texture_t *tex = (mts > 0u) ? twd_d3dtx_upload_gfx_texture(gfx, mtb, mts, NULL) : NULL;
                twd_ttarch2_free(mtb);
                if (tex) {
                    out_texs[ti] = tex;
                    highest = ti + 1u;
                    uploaded++;
                }
            }
        }
        if (!out_texs[0] && mesh->texture_count > 0u && mesh->textures[0][0] != '\0') {
            char nm[160];
            size_t tn = strlen(mesh->textures[0]);
            if (tn < 144u) {
                memcpy(nm, mesh->textures[0], tn + 1u);
                if (tn < 6u || strcmp(nm + tn - 6u, ".d3dtx") != 0) memcpy(nm + tn, ".d3dtx", 7u);
                out_texs[0] = boot_upload_d3dtx_vfs(gfx, nm);
                if (out_texs[0]) { if (highest == 0u) highest = 1u; uploaded++; }
            }
        }
    }
    if (!out_texs[0] && agent_or_mesh_name && agent_or_mesh_name[0] != '\0') {
        char cand[160];
        size_t al = strlen(agent_or_mesh_name);
        if (al >= 8u && strcmp(agent_or_mesh_name + al - 8u, ".d3dmesh") == 0) al -= 8u;
        if (al > 0u && al < 144u) {
            memcpy(cand, agent_or_mesh_name, al);
            memcpy(cand + al, ".d3dtx", 7u);
            out_texs[0] = boot_upload_d3dtx_vfs(gfx, cand);
            if (out_texs[0]) { if (highest == 0u) highest = 1u; uploaded++; }
        }
    }
    if (!out_texs[0] && highest > 1u) {
        for (uint32_t ti = 1u; ti < highest; ++ti) {
            if (out_texs[ti]) { out_texs[0] = out_texs[ti]; out_texs[ti] = NULL; break; }
        }
    }
    if (out_uploaded) *out_uploaded = uploaded;
    return highest;
}

static twd_gfx_texture_t *boot_load_mesh_texture(
    twd_gfx_device_t *gfx, const twd_d3dmesh_t *mesh, const char *agent_or_mesh_name) {
    twd_gfx_texture_t *one = NULL;
    (void)boot_load_mesh_textures(gfx, mesh, agent_or_mesh_name, &one, 1u, NULL);
    return one;
}

static void boot_free_scene_items(twd_boot_loop_state_t *st) {
    if (!st) return;
    for (uint32_t ii = 0u; ii < st->scene_item_count; ++ii) {
        for (uint32_t ti = 0u; ti < TWD_D3DMESH_MAX_TEXTURES; ++ti) {
            if (st->scene_items[ii].batch_tex[ti]) {
                twd_gfx_texture_destroy(st->scene_items[ii].batch_tex[ti]);
            }
        }
        twd_d3dmesh_gpu_free(&st->scene_items[ii].gpu);
        twd_d3dmesh_free(&st->scene_items[ii].mesh);
    }
    st->scene_item_count = 0u;
}

static void boot_populate_menu_buttons(twd_boot_loop_state_t *st, const char *scene_name);

static void boot_populate_scene_items(twd_boot_loop_state_t *st) {
    if (!st || !st->scenes) return;
    boot_free_scene_items(st);
    const char *pri = twd_scene_mgr_primary_name(st->scenes);
    twd_scene_t *psc = pri ? twd_scene_mgr_find(st->scenes, pri) : NULL;
    if (!psc) return;

    boot_populate_menu_buttons(st, pri);

    if (st->has_boot_landb) {
        twd_scene_agent_t *ag_start = twd_scene_find_agent(psc, "ui_boot_start");
        const char *start_txt = twd_landb_find_text(&st->boot_landb, 0x17a04004u);
        if (ag_start && start_txt && ag_start->ui_text[0] == '\0') {
            twd_landb_strip_markup(start_txt, ag_start->ui_text, sizeof(ag_start->ui_text));
        }
        twd_scene_agent_t *ag_legal = twd_scene_find_agent(psc, "ui_boot_legalText");
        const char *legal_txt = twd_landb_find_text(&st->boot_landb, 0x17a04007u);
        if (ag_legal && legal_txt && ag_legal->ui_text[0] == '\0') {
            twd_landb_strip_markup(legal_txt, ag_legal->ui_text, sizeof(ag_legal->ui_text));
        }
    }

    if (!st->gfx) return;
    for (uint32_t ai = 0u; ai < psc->agent_count && st->scene_item_count < TWD_BOOT_MAX_SCENE_ITEMS; ++ai) {
        const twd_scene_agent_t *ag = &psc->agents[ai];
        if (ag->render_mesh[0] == '\0') continue;
        int eff_vis = 1;
        float wpos[3];
        boot_agent_world_transform(psc, ag, &eff_vis, wpos);
        if (!eff_vis) continue;

        size_t msz = 0u;
        void *mbuf = twd_asset_read_alloc(ag->render_mesh, &msz);
        if (mbuf && msz > 0u) {
            twd_boot_scene_item_t *it = &st->scene_items[st->scene_item_count];
            memset(it, 0, sizeof(*it));
            if (twd_d3dmesh_parse_memory(mbuf, msz, &it->mesh) == 0 &&
                twd_d3dmesh_upload_gfx(st->gfx, &it->mesh, &it->gpu) == 0) {
                uint32_t up_cnt = 0u;
                it->batch_tex_count = boot_load_mesh_textures(
                    st->gfx, &it->mesh, ag->name, it->batch_tex, TWD_D3DMESH_MAX_TEXTURES, &up_cnt);
                if (st->stats) st->stats->boot_textures_loaded += up_cnt;
                {
                    size_t anl = strlen(ag->name);
                    if (anl >= sizeof(it->agent_name)) anl = sizeof(it->agent_name) - 1u;
                    memcpy(it->agent_name, ag->name, anl);
                    it->agent_name[anl] = '\0';
                }
                it->world_pos[0] = wpos[0];
                it->world_pos[1] = wpos[1];
                it->world_pos[2] = wpos[2];
                it->scale[0] = ag->scale[0] > 0.0f ? ag->scale[0] : 1.0f;
                it->scale[1] = ag->scale[1] > 0.0f ? ag->scale[1] : 1.0f;
                it->scale[2] = ag->scale[2] > 0.0f ? ag->scale[2] : 1.0f;
                it->is_bg = (strstr(ag->name, "Background") != NULL || wpos[2] <= -8.0f) ? 1 : 0;
                it->active = 1;
                st->scene_item_count++;
                if (st->stats) st->stats->boot_meshes_loaded++;
            } else {
                twd_d3dmesh_free(&it->mesh);
            }
        }
        if (mbuf) twd_asset_free(mbuf);
    }
    for (uint32_t i = 1u; i < st->scene_item_count; ++i) {
        twd_boot_scene_item_t key = st->scene_items[i];
        uint32_t j = i;
        while (j > 0u && st->scene_items[j - 1u].world_pos[2] > key.world_pos[2]) {
            st->scene_items[j] = st->scene_items[j - 1u];
            j--;
        }
        st->scene_items[j] = key;
    }
}

static void boot_layout_menu_buttons(twd_boot_loop_state_t *st) {
    if (!st) return;
    uint32_t dh = st->cfg.height ? st->cfg.height : 720u;
    int32_t base_x = 56;
    int32_t base_y = (dh >= 480u) ? 140 : 88;
    int32_t btn_w = (dh >= 480u) ? 320 : 240;
    int32_t btn_h = (dh >= 480u) ? 44 : 34;
    int32_t step_y = btn_h + 12;
    for (uint32_t i = 0u; i < st->menu_btn_count; ++i) {
        st->menu_btns[i].x = base_x;
        st->menu_btns[i].y = base_y + (int32_t)i * step_y;
        st->menu_btns[i].w = btn_w;
        st->menu_btns[i].h = btn_h;
    }
}

static void boot_copy_trunc(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t sl = strlen(src);
    if (sl >= cap) sl = cap - 1u;
    if (sl > 0u) memcpy(dst, src, sl);
    dst[sl] = '\0';
}

static void boot_format_single_line(char *dst, size_t cap, const char *raw) {
    twd_landb_strip_markup(raw ? raw : "", dst, cap);
    for (size_t i = 0u; dst[i] != '\0'; ++i) {
        if (dst[i] == '\n' || dst[i] == '\r') dst[i] = ' ';
    }
}

static void boot_populate_menu_buttons(twd_boot_loop_state_t *st, const char *scene_name) {
    if (!st) return;
    st->menu_btn_count = 0u;
    if (!scene_name) return;

    if (strcmp(scene_name, "ui_episodes.scene") == 0) {
        st->menu_page = TWD_MENU_PAGE_EPISODES;
        static const struct {
            uint32_t landb_id;
            const char *fallback;
            const char *action;
        } k_eps[5] = {
            {0x17a0a203u, "Episode 1: Done Running", "ep1"},
            {0x17a0a204u, "Episode 2: Suffer the Children", "ep2"},
            {0x17a0a205u, "Episode 3: Broken Toys", "ep3"},
            {0x17a0a206u, "Episode 4: Take Us Back", "ep4"},
            {0u, "Back", "back_main"}
        };
        for (uint32_t i = 0u; i < 5u && i < TWD_BOOT_MAX_MENU_BTNS; ++i) {
            twd_boot_menu_btn_t *b = &st->menu_btns[st->menu_btn_count++];
            memset(b, 0, sizeof(*b));
            const char *txt = (k_eps[i].landb_id != 0u && st->has_menu_landb)
                ? twd_landb_find_text(&st->menu_landb, k_eps[i].landb_id) : NULL;
            if (txt && txt[0] != '\0') boot_format_single_line(b->label, sizeof(b->label), txt);
            if (b->label[0] == '\0') boot_copy_trunc(b->label, sizeof(b->label), k_eps[i].fallback);
            boot_copy_trunc(b->action, sizeof(b->action), k_eps[i].action);
        }
    } else if (strcmp(scene_name, "ui_menuMain.scene") == 0 && st->menu_page == TWD_MENU_PAGE_SETTINGS) {
        static const char *const k_fx_names[3] = {"Low", "Medium", "High"};
        const char *fx_lbl = k_fx_names[(st->opt_fx_quality >= 0 && st->opt_fx_quality < 3)
            ? st->opt_fx_quality : 2];
        const char *opt_lbls[4];
        static const char *const k_opt_acts[4] = {
            "opt_subtitles", "opt_windowed", "opt_fx", "back_main"
        };
        opt_lbls[0] = st->opt_subtitles ? "Subtitles: ON" : "Subtitles: OFF";
        opt_lbls[1] = st->opt_windowed ? "Windowed: ON" : "Windowed: OFF";
        opt_lbls[2] = fx_lbl;
        opt_lbls[3] = "Back";
        for (uint32_t i = 0u; i < 4u && i < TWD_BOOT_MAX_MENU_BTNS; ++i) {
            twd_boot_menu_btn_t *b = &st->menu_btns[st->menu_btn_count++];
            memset(b, 0, sizeof(*b));
            if (i == 2u) {
                static const char k_pfx[] = "FX Quality: ";
                size_t pl = sizeof(k_pfx) - 1u;
                memcpy(b->label, k_pfx, pl);
                boot_copy_trunc(b->label + pl, sizeof(b->label) - pl, opt_lbls[i]);
            } else {
                boot_copy_trunc(b->label, sizeof(b->label), opt_lbls[i]);
            }
            boot_copy_trunc(b->action, sizeof(b->action), k_opt_acts[i]);
        }
    } else if (strcmp(scene_name, "ui_menuMain.scene") == 0) {
        st->menu_page = TWD_MENU_PAGE_MAIN;
        static const struct {
            uint32_t landb_id;
            const char *fallback;
            const char *action;
        } k_btns[4] = {
            {0x17a0a293u, "Play", "play"},
            {0x17a0a03cu, "Episodes", "episodes"},
            {0x17a0a30bu, "Extras & Settings", "settings"},
            {0x17a0a153u, "Exit", "quit"}
        };
        for (uint32_t i = 0u; i < 4u && i < TWD_BOOT_MAX_MENU_BTNS; ++i) {
            twd_boot_menu_btn_t *b = &st->menu_btns[st->menu_btn_count++];
            memset(b, 0, sizeof(*b));
            const char *txt = st->has_menu_landb
                ? twd_landb_find_text(&st->menu_landb, k_btns[i].landb_id) : NULL;
            if (txt && txt[0] != '\0') boot_format_single_line(b->label, sizeof(b->label), txt);
            if (b->label[0] == '\0') boot_copy_trunc(b->label, sizeof(b->label), k_btns[i].fallback);
            boot_copy_trunc(b->action, sizeof(b->action), k_btns[i].action);
        }
    } else {
        return;
    }

    if (st->menu_focus < 0 || (uint32_t)st->menu_focus >= st->menu_btn_count) {
        st->menu_focus = 0;
    }
    boot_layout_menu_buttons(st);
}

static void boot_advance_dialogue(twd_boot_loop_state_t *st, int step_forward);

static int lua_dlg_set_node(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm;
    twd_boot_loop_state_t *st = (twd_boot_loop_state_t *)user;
    if (st && argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        boot_copy_trunc(st->dlg_node, sizeof(st->dlg_node), argv[0].u.str);
        (void)twd_kv_set("scene", "dialog_node", st->dlg_node);
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = 1; }
    return 0;
}

static int lua_logic_set(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user;
    if (argc >= 2 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str) {
        if (argv[1].kind == TWD_LUA_T_STR && argv[1].u.str) {
            (void)twd_kv_set("logic", argv[0].u.str, argv[1].u.str);
        } else if (argv[1].kind == TWD_LUA_T_BOOL) {
            (void)twd_kv_set("logic", argv[0].u.str, argv[1].u.b ? "1" : "0");
        }
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = 1; }
    return 0;
}

static int lua_logic_get(twd_lua_vm_t *vm, void *user, int argc, const twd_lua_val_t *argv, twd_lua_val_t *ret) {
    (void)vm; (void)user;
    char buf[32];
    if (argc >= 1 && argv[0].kind == TWD_LUA_T_STR && argv[0].u.str &&
        twd_kv_get("logic", argv[0].u.str, buf, sizeof(buf)) > 0) {
        if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = (buf[0] != '0') ? 1 : 0; }
        return 0;
    }
    if (ret) { ret->kind = TWD_LUA_T_BOOL; ret->u.b = 0; }
    return 0;
}

static void boot_register_lua_api(twd_lua_vm_t *lvm, twd_boot_loop_state_t *st, boot_lua_ctx_t *bctx) {
    if (!lvm || !st || !bctx) return;
    (void)twd_lua_vm_register(lvm, "SceneOpen", lua_scene_open, st->scenes);
    (void)twd_lua_vm_register(lvm, "Game_SceneOpen", lua_scene_open, st->scenes);
    (void)twd_lua_vm_register(lvm, "SceneAdd", lua_scene_add, st->scenes);
    (void)twd_lua_vm_register(lvm, "SceneIsActive", lua_scene_is_active, st->scenes);
    (void)twd_lua_vm_register(lvm, "SceneHide", lua_scene_hide, st->scenes);
    (void)twd_lua_vm_register(lvm, "SceneRemove", lua_scene_remove, st->scenes);
    (void)twd_lua_vm_register(lvm, "SceneFind", lua_scene_find, st->scenes);
    (void)twd_lua_vm_register(lvm, "SceneGetAgents", lua_scene_get_agents, st->scenes);
    (void)twd_lua_vm_register(lvm, "SceneAddAgent", lua_scene_add_agent, st->scenes);
    (void)twd_lua_vm_register(lvm, "AgentFind", lua_agent_find, st->scenes);
    (void)twd_lua_vm_register(lvm, "AgentSetPos", lua_agent_set_pos, st->scenes);
    (void)twd_lua_vm_register(lvm, "Game_SetSceneDialogNode", lua_dlg_set_node, st);
    (void)twd_lua_vm_register(lvm, "Game_RunSceneDialog", lua_dlg_set_node, st);
    (void)twd_lua_vm_register(lvm, "Game_GetLoaded", lua_ret_const_bool, (void *)(uintptr_t)1u);
    (void)twd_lua_vm_register(lvm, "Game_GetSkipEnterCutscenes", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "Game_GetPlayerDialog", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "DlgPreloadAll", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "LogicGet", lua_logic_get, NULL);
    (void)twd_lua_vm_register(lvm, "LogicSet", lua_logic_set, NULL);
    (void)twd_lua_vm_register(lvm, "Input_IsKeyDown", lua_input_is_key_down, NULL);
    (void)twd_lua_vm_register(lvm, "CursorGetPos", lua_cursor_get_pos, NULL);
    (void)twd_lua_vm_register(lvm, "GameEngine_Quit", lua_engine_quit, NULL);
    (void)twd_lua_vm_register(lvm, "SaveGame", lua_save_game, st);
    (void)twd_lua_vm_register(lvm, "LoadGame", lua_load_game, st);
    (void)twd_lua_vm_register(lvm, "NewGame", lua_new_game, st);
    (void)twd_lua_vm_register(lvm, "ResetGame", lua_reset_game, st);
    (void)twd_lua_vm_register(lvm, "IsSaving", lua_is_saving, st);
    (void)twd_lua_vm_register(lvm, "IsSaveGameCorrupt", lua_is_save_corrupt, st);
    (void)twd_lua_vm_register(lvm, "SaveLoadSetSaveDisplayName", lua_save_set_display_name, st);
    (void)twd_lua_vm_register(lvm, "SaveLoadClearRuntimeProperties", lua_save_clear_props, st);
    (void)twd_lua_vm_register(lvm, "SaveLoadRemoveUserFiles", lua_save_remove_files, st);
    (void)twd_lua_vm_register(lvm, "SaveLoadHasAvailableSpace", lua_save_has_space, st);
    (void)twd_lua_vm_register(lvm, "LoadScript", lua_load_script, NULL);
    (void)twd_lua_vm_register(lvm, "GameMode", lua_game_mode, NULL);
    (void)twd_lua_vm_register(lvm, "require", lua_require, bctx);
    (void)twd_lua_vm_register(lvm, "ResourceExists", lua_resource_exists, NULL);
    (void)twd_lua_vm_register(lvm, "IsToolBuild", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "IsRunningFromMaya", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "IsPlatformAppleTV", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "IsPlatformXboxOne", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "IsPlatformPS4", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "IsPlatformAndroid", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "IsPlatformIOS", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "IsPlatformSmallScreen", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "Input_UseCursor", lua_ret_const_bool, (void *)(uintptr_t)1u);
    (void)twd_lua_vm_register(lvm, "Input_UseTouch", lua_ret_const_bool, NULL);
    (void)twd_lua_vm_register(lvm, "DebugMenu_AddButton", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "DebugMenu_AddCustomButton", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "DebugMenu_ToggleButtons", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "OverlayShowLoadingScreen", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "SceneSetTimeScale", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "InputMapperAddEvent", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "ThreadStart", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "Vector", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "FileSetExtension", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "FileStripExtension", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "CreateResource", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "PropertyGet", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "PropertySet", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "AgentSetProperty", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "TextSet", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "Preload_SetSubProjectShaderPack", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "PlatformAppleTV_RemoveAssetPack", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "DLCStatus_Refresh", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "UserManager_AutomaticallyLogIn", lua_ret_nil, NULL);
    (void)twd_lua_vm_register(lvm, "PlatformAppleTV_SetMenuSuspendAllowed", lua_ret_nil, NULL);
}

static void boot_exec_menu_main(twd_boot_loop_state_t *st) {
    if (!st || st->menu_script_ran) return;
    if (!twd_ttarch2_exists_mounted("Menu_Main.lua")) return;
    st->menu_script_ran = 1;
    twd_lua_vm_t *mvm = twd_lua_vm_create();
    if (!mvm) return;
    boot_lua_ctx_t bctx;
    memset(&bctx, 0, sizeof(bctx));
    boot_register_lua_api(mvm, st, &bctx);
    if (twd_lua_vm_exec_vfs(mvm, "Menu_Main.lua") == 0) {
        if (st->stats) {
            st->stats->boot_scripts_executed += twd_lua_vm_executed_count(mvm);
        }
        (void)twd_kv_set("lua.menu", "loaded", "1");
        if (twd_lua_vm_get_global(mvm, "Menu_Main_Start") != NULL) {
            if (twd_lua_vm_call_global(mvm, "Menu_Main_Start", 0, NULL, NULL) == 0) {
                (void)twd_kv_set("lua.menu", "started", "1");
            }
        }
    }
    twd_lua_vm_destroy(mvm);
}

static void boot_u32_to_dec(uint32_t v, char *buf, size_t cap) {
    char tmp[16];
    size_t i = sizeof(tmp);
    tmp[--i] = '\0';
    do {
        tmp[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && i > 0u);
    boot_copy_trunc(buf, cap, &tmp[i]);
}

static void boot_advance_dialogue(twd_boot_loop_state_t *st, int step_forward) {
    if (!st || !st->has_scene_landb || st->scene_landb.entry_count == 0u) return;
    uint32_t n = st->scene_landb.entry_count;
    uint32_t start = step_forward ? ((st->dlg_cursor + 1u) % n) : (st->dlg_cursor % n);
    if (step_forward) {
        st->dlg_step++;
        if ((st->dlg_step % 3u) == 0u) {
            st->dlg_choices_open = 1;
        }
    }
    for (uint32_t off = 0u; off < n; ++off) {
        uint32_t idx = (start + off) % n;
        const twd_landb_entry_t *e = &st->scene_landb.entries[idx];
        if (!e->text || e->text[0] == '\0') continue;
        char cleaned[160];
        boot_format_single_line(cleaned, sizeof(cleaned), e->text);
        if (cleaned[0] == '\0') continue;
        st->dlg_cursor = idx;
        boot_copy_trunc(st->dlg_line, sizeof(st->dlg_line), cleaned);
        (void)twd_kv_set("scene", "subtitle", st->dlg_line);
        if (e->res_id != 0u) {
            char wav[32];
            boot_u32_to_dec(e->res_id, wav, sizeof(wav));
            size_t wl = strlen(wav);
            if (wl + 5u <= sizeof(wav)) {
                memcpy(wav + wl, ".wav", 5u);
                if (twd_ttarch2_exists_mounted(wav)) {
                    (void)twd_kv_set("audio", "last_voice", wav);
                    if (st->audio && twd_audio_play_fsb_vfs(st->audio, wav, 0.85f) == 0) {
                        (void)twd_kv_set("audio", "voice_started", "1");
                    }
                }
            }
        }
        return;
    }
}

static void boot_start_gameplay_scene(twd_boot_loop_state_t *st, const char *scene_name) {
    if (!st || !scene_name) return;
    if (st->has_scene_landb) {
        twd_landb_free(&st->scene_landb);
        st->has_scene_landb = 0;
    }
    st->dlg_cursor = 0u;
    st->dlg_step = 0u;
    st->dlg_line[0] = '\0';
    st->dlg_node[0] = '\0';
    st->dlg_choices_open = 0;

    const char *base = scene_name;
    if (strncmp(base, "adv_", 4u) == 0) base += 4u;
    size_t blen = strlen(base);
    if (blen >= 6u && strcmp(base + blen - 6u, ".scene") == 0) blen -= 6u;
    if (blen > 0u && blen < 96u) {
        char base_name[96];
        memcpy(base_name, base, blen);
        base_name[blen] = '\0';

        char lua_name[112];
        memcpy(lua_name, base_name, blen);
        memcpy(lua_name + blen, ".lua", 5u);
        if (twd_ttarch2_exists_mounted(lua_name)) {
            twd_lua_vm_t *svm = twd_lua_vm_create();
            if (svm) {
                boot_lua_ctx_t bctx;
                memset(&bctx, 0, sizeof(bctx));
                boot_register_lua_api(svm, st, &bctx);
                if (twd_lua_vm_exec_vfs(svm, lua_name) == 0) {
                    if (st->stats) st->stats->boot_scripts_executed += twd_lua_vm_executed_count(svm);
                    (void)twd_kv_set("scene", "script_loaded", lua_name);
                    if (twd_lua_vm_get_global(svm, base_name) != NULL) {
                        (void)twd_lua_vm_call_global(svm, base_name, 0, NULL, NULL);
                    }
                    if (twd_lua_vm_get_global(svm, "OnLogicReady") != NULL) {
                        (void)twd_lua_vm_call_global(svm, "OnLogicReady", 0, NULL, NULL);
                    }
                }
                twd_lua_vm_destroy(svm);
            }
        }

        static const char *const k_acts[4] = {
            "_act1_english.landb", "_act2_english.landb", "_act3_english.landb", "_english.landb"
        };
        for (int ai = 0; ai < 4 && !st->has_scene_landb; ++ai) {
            char lb_name[144];
            size_t lo = 0u;
            if (ai < 3) {
                memcpy(lb_name, "env_", 4u);
                lo = 4u;
            }
            memcpy(lb_name + lo, base_name, blen);
            lo += blen;
            size_t suf_l = strlen(k_acts[ai]);
            if (lo + suf_l < sizeof(lb_name)) {
                memcpy(lb_name + lo, k_acts[ai], suf_l + 1u);
                size_t lsz = 0u;
                void *lbuf = twd_asset_read_alloc(lb_name, &lsz);
                if (lbuf && lsz > 0u && twd_landb_parse_memory(lbuf, lsz, &st->scene_landb) == 0) {
                    st->has_scene_landb = 1;
                    (void)twd_kv_set("scene", "landb", lb_name);
                }
                if (lbuf) twd_asset_free(lbuf);
            }
        }
        if (st->has_scene_landb) {
            boot_advance_dialogue(st, 0);
        }
    }
}

static void boot_switch_to_scene(twd_boot_loop_state_t *st, const char *scene_name) {
    if (!st || !st->scenes || !scene_name || scene_name[0] == '\0') return;
    if (strcmp(scene_name, "ui_menuMain.scene") == 0) {
        boot_exec_menu_main(st);
    }
    if (twd_scene_mgr_open(st->scenes, scene_name) != 0) return;
    boot_populate_scene_items(st);
    if (boot_is_gameplay_3d(st, scene_name)) {
        boot_start_gameplay_scene(st, scene_name);
    } else if (st->has_scene_landb) {
        twd_landb_free(&st->scene_landb);
        st->has_scene_landb = 0;
        st->dlg_line[0] = '\0';
        st->dlg_choices_open = 0;
    }
    if (st->stats) {
        st->stats->boot_scenes_active = twd_scene_mgr_scene_count(st->scenes);
        st->stats->boot_scene_agents = twd_scene_mgr_total_agent_count(st->scenes);
        const char *pri = twd_scene_mgr_primary_name(st->scenes);
        if (pri) {
            size_t plen = strlen(pri);
            if (plen >= sizeof(st->stats->active_scene)) plen = sizeof(st->stats->active_scene) - 1u;
            memcpy(st->stats->active_scene, pri, plen);
            st->stats->active_scene[plen] = '\0';
        }
    }
    (void)twd_kv_set("lua.scene", "active", scene_name);
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
    cam_x += st->cam_pan_x;
    cam_y += st->cam_pan_y;

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
        static const uint8_t k_sel_tint[4] = {255u, 215u, 90u, 255u};
        const char *pri_sc = st->scenes ? twd_scene_mgr_primary_name(st->scenes) : NULL;
        int is_3d = boot_is_gameplay_3d(st, pri_sc);
        float cpos[3] = {0.0f, 0.0f, 5.0f};
        float cyaw = 0.0f, cpitch = 0.0f;
        if (is_3d) boot_get_scene_camera(st, cpos, &cyaw, &cpitch);
        float ppu = (float)dh / 11.5f;
        for (uint32_t ii = 0u; ii < st->scene_item_count; ++ii) {
            const twd_boot_scene_item_t *it = &st->scene_items[ii];
            if (!it->active) continue;
            if (it->is_bg && it->batch_tex[0]) {
                uint32_t tw = 0u, th = 0u;
                twd_gfx_texture_get_size(it->batch_tex[0], &tw, &th);
                if (tw > 0u && th > 0u) {
                    (void)twd_gfx_draw_sprite(
                        gfx, it->batch_tex[0], 0, 0, (int32_t)tw, (int32_t)th,
                        0, 0, (int32_t)dw, (int32_t)dh, NULL);
                    continue;
                }
            }
            const uint8_t *tint = (st->selected_agent[0] != '\0' &&
                                   strcmp(st->selected_agent, it->agent_name) == 0)
                ? k_sel_tint : k_ui_tint;
            if (is_3d) {
                (void)twd_d3dmesh_draw_camera_multi(
                    gfx, &it->mesh, (const twd_gfx_texture_t *const *)it->batch_tex,
                    it->batch_tex_count, cpos, cyaw, cpitch, 55.0f,
                    it->world_pos, it->scale, tint);
            } else {
                float sx = (float)dw * 0.5f + (it->world_pos[0] - cam_x) * ppu;
                float sy = (float)dh * 0.5f - (it->world_pos[1] - cam_y) * ppu;
                float sc = it->scale[0] > 0.0f ? it->scale[0] : 1.0f;
                (void)twd_d3dmesh_draw_shaded(
                    gfx, &it->mesh, it->batch_tex[0], sx, sy, ppu * 3.0f * sc, tint);
            }
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
    if (st->has_boot_font || st->has_menu_font) {
        static const uint8_t k_white[4] = {255u, 255u, 255u, 255u};
        static const uint8_t k_card_bg[4] = {20u, 24u, 32u, 195u};
        static const uint8_t k_card_focus[4] = {92u, 62u, 22u, 225u};
        static const uint8_t k_accent[4] = {235u, 175u, 55u, 255u};
        static const uint8_t k_muted[4] = {195u, 200u, 210u, 240u};
        const twd_font_t *hdr_font = st->has_boot_font ? &st->boot_font : &st->menu_font;
        const twd_font_t *menu_font = st->has_menu_font ? &st->menu_font : hdr_font;
        const char *label = (st->stats && st->stats->active_scene[0]) ? st->stats->active_scene : "Boot";
        (void)twd_font_draw_text(gfx, hdr_font, 16.0f, 16.0f, 1.0f, k_white, label);
        if (st->menu_btn_count == 0u && strcmp(label, "ui_boot.scene") != 0 &&
            strncmp(label, "ui_", 3u) != 0) {
            (void)twd_font_draw_text(gfx, hdr_font, 16.0f, 40.0f, 0.85f, k_muted,
                                     "WASD: Move  Q/E: Orbit  Space/N: Next Line  Click: Inspect  Backspace: Menu");
            if (st->selected_agent[0] != '\0') {
                char sel_buf[96];
                static const char k_spfx[] = "Selected: ";
                size_t sp = sizeof(k_spfx) - 1u;
                memcpy(sel_buf, k_spfx, sp);
                boot_copy_trunc(sel_buf + sp, sizeof(sel_buf) - sp, st->selected_agent);
                (void)twd_font_draw_text(gfx, hdr_font, 16.0f, 64.0f, 0.9f, k_accent, sel_buf);
            }
            if (st->opt_subtitles && (st->dlg_line[0] != '\0' || st->dlg_choices_open)) {
                static const uint8_t k_sub_bg[4] = {10u, 12u, 16u, 215u};
                int32_t bar_h = st->dlg_choices_open ? 96 : 64;
                int32_t bar_y = (int32_t)dh - bar_h - 12;
                if (bar_y < 80) bar_y = 80;
                (void)twd_gfx_draw_rect(gfx, 24, bar_y, (int32_t)dw - 48, bar_h, k_sub_bg);
                if (st->dlg_line[0] != '\0') {
                    (void)twd_font_draw_text(gfx, menu_font, 40.0f, (float)(bar_y + 14),
                                             (dh >= 480u) ? 0.72f : 0.6f, k_white, st->dlg_line);
                }
                if (st->dlg_choices_open) {
                    (void)twd_font_draw_text(gfx, hdr_font, 40.0f, (float)(bar_y + 54), 0.9f, k_accent,
                                             "[1] Help AJ    [2] Check Area    [3] Stay Silent");
                }
            }
        }
        if (st->menu_btn_count > 0u) {
            float txt_scale = (dh >= 480u) ? 0.85f : 0.68f;
            float line_h = (menu_font->height > 0.0f) ? menu_font->height * txt_scale : 22.0f;
            for (uint32_t bi = 0u; bi < st->menu_btn_count; ++bi) {
                const twd_boot_menu_btn_t *b = &st->menu_btns[bi];
                int focused = ((int32_t)bi == st->menu_focus);
                (void)twd_gfx_draw_rect(gfx, b->x, b->y, b->w, b->h, focused ? k_card_focus : k_card_bg);
                if (focused) {
                    (void)twd_gfx_draw_rect(gfx, b->x, b->y, 6, b->h, k_accent);
                }
                float ty = (float)b->y + ((float)b->h - line_h) * 0.5f;
                if (ty < (float)b->y + 4.0f) ty = (float)b->y + 4.0f;
                (void)twd_font_draw_text(gfx, menu_font, (float)(b->x + 18), ty, txt_scale,
                                         focused ? k_white : k_muted, b->label);
            }
            if (st->menu_status[0] != '\0') {
                const twd_boot_menu_btn_t *last = &st->menu_btns[st->menu_btn_count - 1u];
                (void)twd_font_draw_text(gfx, menu_font, (float)last->x,
                                         (float)(last->y + last->h + 16),
                                         txt_scale * 0.85f, k_accent, st->menu_status);
            }
        }
        if (st->scenes) {
            const char *pri = twd_scene_mgr_primary_name(st->scenes);
            twd_scene_t *psc = pri ? twd_scene_mgr_find(st->scenes, pri) : NULL;
            if (psc) {
                float uy = 48.0f;
                for (uint32_t ai = 0u; ai < psc->agent_count; ++ai) {
                    if (psc->agents[ai].visible && psc->agents[ai].ui_text[0] != '\0') {
                        (void)twd_font_draw_text(gfx, hdr_font, 24.0f, uy, 1.0f, k_white, psc->agents[ai].ui_text);
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
    if (!installed_key) {
        size_t ksz = 0u;
        void *kbuf = twd_asset_read_alloc("archive_key.txt", &ksz);
        if (!kbuf && game_dir && strlen(game_dir) + sizeof("/archive_key.txt") + 1u < 540u) {
            char kp[540];
            size_t gl = strlen(game_dir);
            memcpy(kp, game_dir, gl);
            memcpy(kp + gl, "/archive_key.txt", sizeof("/archive_key.txt"));
            twd_file_t kf;
            if (twd_file_open(kp, TWD_FILE_OPEN_READ, &kf) == 0) {
                uint8_t tmp[256];
                int64_t nr = twd_file_read(&kf, tmp, sizeof(tmp) - 1u);
                twd_file_close(&kf);
                if (nr > 0) {
                    tmp[(size_t)nr] = 0u;
                    size_t kl = (size_t)nr;
                    while (kl > 0u && (tmp[kl - 1u] == '\n' || tmp[kl - 1u] == '\r' ||
                                       tmp[kl - 1u] == ' ' || tmp[kl - 1u] == '\t')) {
                        tmp[--kl] = 0u;
                    }
                    if (kl > 0u && twd_crypto_install_archive_key(tmp, kl) == 0) {
                        installed_key = 1;
                    }
                }
            }
        } else if (kbuf && ksz > 0u) {
            uint8_t *kp = (uint8_t *)kbuf;
            size_t kl = ksz;
            while (kl > 0u && (kp[kl - 1u] == '\n' || kp[kl - 1u] == '\r' ||
                               kp[kl - 1u] == ' ' || kp[kl - 1u] == '\t')) {
                kl--;
            }
            if (kl > 0u && twd_crypto_install_archive_key(kp, kl) == 0) {
                installed_key = 1;
            }
        }
        if (kbuf) twd_asset_free(kbuf);
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
#ifdef __ANDROID__
        if (local_stats.mounted_archives == 0) {
            char idir[512];
            if (twd_android_get_data_dir(0, idir, sizeof(idir)) > 0 && strcmp(idir, game_dir) != 0 &&
                twd_path_is_dir(idir)) {
                int mi = twd_ttarch2_mount_dir(idir);
                if (mi > 0) local_stats.mounted_archives += mi;
                size_t il = strlen(idir);
                if (il + sizeof("/Archives") + 1u < 540u) {
                    char iarch[540];
                    memcpy(iarch, idir, il);
                    memcpy(iarch + il, "/Archives", sizeof("/Archives"));
                    if (twd_path_is_dir(iarch)) {
                        int mia = twd_ttarch2_mount_dir(iarch);
                        if (mia > 0) local_stats.mounted_archives += mia;
                    }
                }
            }
        }
#endif
        if ((local_stats.audio_caps & TWD_AUDIO_CAP_FMOD_STUDIO) == 0 && gl + 24u < sizeof(dir_buf)) {
            char fmod_c[540], fmod_s[540];
#ifdef _WIN32
            static const char k_fc[] = "/fmod64.dll";
            static const char k_fs[] = "/fmodstudio64.dll";
#else
            static const char k_fc[] = "/libfmod.so";
            static const char k_fs[] = "/libfmodstudio.so";
#endif
            memcpy(fmod_c, game_dir, gl);
            memcpy(fmod_c + gl, k_fc, sizeof(k_fc));
            memcpy(fmod_s, game_dir, gl);
            memcpy(fmod_s + gl, k_fs, sizeof(k_fs));
            local_stats.audio_caps = twd_audio_init(fmod_s, fmod_c);
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
    st.opt_subtitles = 1;
    st.opt_windowed = 1;
    st.opt_fx_quality = 2;
    {
        char pv[16];
        if (twd_kv_get("prefs", "windowed", pv, sizeof(pv)) > 0) st.opt_windowed = (pv[0] != '0') ? 1 : 0;
        if (twd_kv_get("prefs", "fx_brush_quality", pv, sizeof(pv)) > 0 &&
            pv[0] >= '0' && pv[0] <= '2') {
            st.opt_fx_quality = pv[0] - '0';
        }
    }
    {
        static const char *const k_boot_scripts[] = {
            "Boot.lua", "_boot.lua", "scripts/_boot.lua", "_resdesc_50_Boot.lua"
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
                boot_lua_ctx_t bctx;
                memset(&bctx, 0, sizeof(bctx));
                boot_register_lua_api(lvm, &st, &bctx);
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
            {
                static const char *const k_boot_fonts[] = {
                    "ui_boot.font", "default.font", "MenuBody.font"
                };
                for (size_t fi = 0u; fi < sizeof(k_boot_fonts) / sizeof(k_boot_fonts[0]) && !st.has_boot_font; ++fi) {
                    size_t fsz = 0u;
                    void *fbuf = twd_asset_read_alloc(k_boot_fonts[fi], &fsz);
                    if (fbuf && fsz > 0u && twd_font_parse_memory(fbuf, fsz, &st.boot_font) == 0) {
                        local_stats.boot_fonts_loaded++;
                        st.has_boot_font = 1;
                        if (st.boot_font.page_count > 0u && st.boot_font.page_hashes[0] != 0ULL) {
                            size_t psz = 0u;
                            void *pbuf = twd_ttarch2_lookup_mounted_crc64_alloc(st.boot_font.page_hashes[0], &psz);
                            if (pbuf && psz > 0u) {
                                st.boot_font_page_tex = twd_d3dtx_upload_gfx_texture(st.gfx, pbuf, psz, NULL);
                                if (st.boot_font_page_tex) {
                                    (void)twd_font_attach_page(&st.boot_font, 0u, st.boot_font_page_tex);
                                    local_stats.boot_textures_loaded++;
                                }
                            }
                            if (pbuf) twd_ttarch2_free(pbuf);
                        }
                        if (!st.boot_font_page_tex && st.splash_tex) {
                            (void)twd_font_attach_page(&st.boot_font, 0u, st.splash_tex);
                        }
                    }
                    if (fbuf) twd_asset_free(fbuf);
                }
            }
            {
                size_t mfsz = 0u;
                void *mfbuf = twd_asset_read_alloc("MenuBody.font", &mfsz);
                if (mfbuf && mfsz > 0u && twd_font_parse_memory(mfbuf, mfsz, &st.menu_font) == 0) {
                    st.has_menu_font = 1;
                    local_stats.boot_fonts_loaded++;
                }
                if (mfbuf) twd_asset_free(mfbuf);
            }
            {
                size_t lbsz = 0u;
                void *lbbuf = twd_asset_read_alloc("boot_english.landb", &lbsz);
                if (lbbuf && lbsz > 0u && twd_landb_parse_memory(lbbuf, lbsz, &st.boot_landb) == 0) {
                    st.has_boot_landb = 1;
                }
                if (lbbuf) twd_asset_free(lbbuf);

                size_t mlbsz = 0u;
                void *mlbbuf = twd_asset_read_alloc("ui_menu_english.landb", &mlbsz);
                if (mlbbuf && mlbsz > 0u && twd_landb_parse_memory(mlbbuf, mlbsz, &st.menu_landb) == 0) {
                    st.has_menu_landb = 1;
                }
                if (mlbbuf) twd_asset_free(mlbbuf);
            }

            if (strcmp(local_stats.active_scene, "ui_menuMain.scene") == 0) {
                boot_exec_menu_main(&st);
            }
            boot_populate_scene_items(&st);

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
    boot_free_scene_items(&st);
    if (st.has_boot_mesh) {
        twd_d3dmesh_gpu_free(&st.boot_mesh_gpu);
        twd_d3dmesh_free(&st.boot_mesh);
    }
    if (st.has_boot_font) twd_font_free(&st.boot_font);
    if (st.has_menu_font) twd_font_free(&st.menu_font);
    if (st.has_boot_landb) twd_landb_free(&st.boot_landb);
    if (st.has_menu_landb) twd_landb_free(&st.menu_landb);
    if (st.has_scene_landb) twd_landb_free(&st.scene_landb);
    if (st.boot_font_page_tex) twd_gfx_texture_destroy(st.boot_font_page_tex);
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
