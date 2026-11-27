/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TWD_GAME_H
#define TWD_GAME_H

#include "twd_runtime.h"

#ifdef _WIN32
#define TWD_EXPORT __declspec(dllexport)
#else
#define TWD_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct twd_atomic_slot {
    uint8_t _pad[0x20];
    int64_t val;
} twd_atomic_slot_t;

typedef struct twd_meta_member {
    const char *name;
    uint8_t _pad[0x18];
    struct twd_meta_member *next;
} twd_meta_member_t;

typedef struct twd_meta_vtable {
    void *(*alloc)(void);
    void (*destruct)(void *obj);
    void (*construct)(void *obj);
    void (*copy_construct)(void *dst, const void *src);
    void (*delete_obj)(void *obj);
    void (*move_construct)(void *dst, void *src);
} twd_meta_vtable_t;

typedef struct twd_meta_op_node {
    int32_t op_id;
    uint32_t _pad;
    void *fn;
    struct twd_meta_op_node *next;
} twd_meta_op_node_t;

typedef struct twd_meta_class_desc {
    uint8_t opaque[0x28];
    twd_meta_member_t *members;
    twd_meta_op_node_t *ops;
    uint8_t _pad_38[0x08];
    const twd_meta_vtable_t *vtable;
} twd_meta_class_desc_t;

typedef struct twd_string {
    uint8_t _pad[0x08];
    union {
        char buf[0x10];
        const char *ptr;
    } u;
    uint64_t len;
    uint64_t cap;
} twd_string_t;

typedef struct twd_handle_obj {
    uint8_t opaque[0x28];
    uint64_t key;
} twd_handle_obj_t;

typedef struct twd_dlist_node {
    struct twd_dlist_node *prev;
    struct twd_dlist_node *next;
} twd_dlist_node_t;

typedef struct twd_dlist {
    int32_t count;
    int32_t _pad;
    twd_dlist_node_t *head;
    twd_dlist_node_t *tail;
} twd_dlist_t;

typedef struct twd_rbtree_node {
    struct twd_rbtree_node *left;
    struct twd_rbtree_node *parent;
    struct twd_rbtree_node *right;
    uint8_t color;
    uint8_t is_nil;
    uint8_t _pad[6];
} twd_rbtree_node_t;

typedef struct twd_ref_obj {
    void (**vtable)(struct twd_ref_obj *self, int32_t flags);
    volatile int32_t refcount;
    int32_t _pad;
} twd_ref_obj_t;

typedef struct twd_ref50_obj {
    uint8_t opaque[0x50];
    volatile int32_t refcount;
    int32_t _pad;
} twd_ref50_obj_t;

typedef struct twd_vtable_obj {
    void (**vtable)(void *self, int32_t flags);
} twd_vtable_obj_t;

typedef struct twd_ptr_array {
    uint8_t opaque[0x18];
    int32_t count;
    int32_t _pad;
    void **items;
} twd_ptr_array_t;

typedef struct twd_lua_tvalue {
    union {
        uint64_t u64;
        uint32_t u32;
        float f;
    } v;
    int32_t tt;
    uint32_t _pad;
} twd_lua_tvalue_t;

typedef struct twd_lua_global {
    uint8_t _pad[0x100];
    void *panic;
    void *mainthread;
} twd_lua_global_t;

typedef struct twd_lua_callinfo {
    twd_lua_tvalue_t *func;
    uint8_t _pad0[0x1a];
    uint8_t callstatus;
    uint8_t _pad1[0x0d];
    int32_t ctx;
    uint8_t _pad2[0x15];
    uint8_t status;
} twd_lua_callinfo_t;

typedef struct twd_lua_state {
    uint8_t _pad0[0x10];
    twd_lua_tvalue_t *top;
    twd_lua_global_t *l_G;
    twd_lua_tvalue_t **base_ptr;
} twd_lua_state_t;

typedef struct twd_boot_config {
    const char *game_dir;
    const char *shader_cache_dir;
    const char *save_dir;
    const char *archive_key;
    uint32_t width;
    uint32_t height;
    uint32_t max_frames;
    int preferred_backend;
} twd_boot_config_t;

typedef struct twd_boot_stats {
    uint32_t hook_count;
    int mounted_archives;
    int found_prefs_prop;
    uint32_t prefs_meta_version;
    uint32_t prefs_property_count;
    int found_boot_lua;
    int boot_script_ran;
    uint32_t boot_scripts_executed;
    uint32_t boot_textures_loaded;
    uint32_t boot_fonts_loaded;
    uint32_t boot_meshes_loaded;
    uint32_t boot_scenes_active;
    uint32_t boot_scene_agents;
    char active_scene[64];
    uint32_t save_ops_completed;
    uint32_t input_events_processed;
    char save_slot_active[64];
    int gfx_backend;
    int audio_caps;
    uint32_t width;
    uint32_t height;
    uint64_t frames_rendered;
    uint64_t draw_calls;
    uint64_t elapsed_ns;
    int clean_shutdown;
} twd_boot_stats_t;

TWD_EXPORT void twd_game_register_hooks(void);
TWD_EXPORT uint32_t twd_game_hook_count(void);
TWD_EXPORT const twd_hook_entry_t *twd_game_hook_at(uint32_t index);
TWD_EXPORT int twd_game_boot_run(const twd_boot_config_t *cfg, twd_boot_stats_t *out_stats);
TWD_EXPORT int64_t twd_engine_meta_slot_init_ptr(twd_atomic_slot_t *obj, int64_t val);
TWD_EXPORT twd_meta_member_t *twd_engine_meta_member_find_str(const twd_meta_class_desc_t *desc,
                                                              const twd_string_t *name);
TWD_EXPORT twd_meta_member_t *twd_engine_meta_member_find(const twd_meta_class_desc_t *desc,
                                                          const char *name);
TWD_EXPORT void twd_engine_meta_op_register(twd_meta_class_desc_t *desc, twd_meta_op_node_t *node);
TWD_EXPORT void twd_engine_meta_vtable_move_construct(const twd_meta_class_desc_t *desc,
                                                      void *dst, void *src);
TWD_EXPORT void *twd_engine_meta_vtable_alloc(const twd_meta_class_desc_t *desc);
TWD_EXPORT void twd_engine_meta_vtable_construct(const twd_meta_class_desc_t *desc, void *obj);
TWD_EXPORT void twd_engine_meta_vtable_copy_construct(const twd_meta_class_desc_t *desc,
                                                      void *dst, const void *src);
TWD_EXPORT void twd_engine_meta_vtable_destruct(const twd_meta_class_desc_t *desc, void *obj);
TWD_EXPORT void twd_engine_meta_vtable_delete(const twd_meta_class_desc_t *desc, void *obj);
TWD_EXPORT void *twd_engine_meta_op_find(const twd_meta_class_desc_t *desc, int32_t op_id);
TWD_EXPORT void twd_engine_mat4_identity(float mat[16]);
TWD_EXPORT void twd_engine_mat4_uniform_scale(float mat[16], float s);
TWD_EXPORT void twd_engine_mat4_diagonal_scale(float mat[16], float sx, float sy, float sz);
TWD_EXPORT void twd_engine_mat4_from_translation(float mat[16], const float t[3]);
TWD_EXPORT void twd_engine_mat4_from_scale_translation_vec(float mat[16], const float s[3],
                                                           const float t[3]);
TWD_EXPORT void twd_engine_rbtree_rotate_right(twd_rbtree_node_t **head_ptr,
                                               twd_rbtree_node_t *where);
TWD_EXPORT bool twd_engine_handle_equals(const twd_handle_obj_t *const *lhs,
                                         const twd_handle_obj_t *const *rhs);
TWD_EXPORT bool twd_engine_dlist_remove(twd_dlist_t *list, twd_dlist_node_t *node);
TWD_EXPORT uint32_t twd_engine_xorshift128_next(uint32_t state[4]);
TWD_EXPORT void *twd_engine_lua_atpanic(twd_lua_state_t *L, void *panicf);
TWD_EXPORT int32_t twd_engine_lua_absindex(const twd_lua_state_t *L, int32_t idx);
TWD_EXPORT int64_t twd_engine_lua_gettop(const twd_lua_state_t *L);
TWD_EXPORT void twd_engine_lua_settop(twd_lua_state_t *L, int32_t idx);
TWD_EXPORT void twd_engine_lua_xmove(twd_lua_state_t *from, twd_lua_state_t *to, int32_t n);
TWD_EXPORT void twd_engine_lua_pushnil(twd_lua_state_t *L);
TWD_EXPORT void twd_engine_lua_pushnumber(twd_lua_state_t *L, float n);
TWD_EXPORT void twd_engine_lua_pushinteger(twd_lua_state_t *L, int64_t n);
TWD_EXPORT void twd_engine_lua_pushunsigned(twd_lua_state_t *L, uint32_t n);
TWD_EXPORT void twd_engine_lua_pushboolean(twd_lua_state_t *L, int32_t b);
TWD_EXPORT void twd_engine_lua_pushlightuserdata(twd_lua_state_t *L, void *p);
TWD_EXPORT int32_t twd_engine_lua_pushthread(twd_lua_state_t *L);
TWD_EXPORT int32_t twd_engine_lua_getctx(const twd_lua_state_t *L, int32_t *ctx);
TWD_EXPORT void twd_engine_telemetry_cohort_init(void *ctx);
TWD_EXPORT void twd_engine_rail_sdk_init(void);
TWD_EXPORT uint8_t twd_engine_pc_shortcut_init(void *ctx);
TWD_EXPORT uint8_t twd_engine_breakpad_http_upload(void);
TWD_EXPORT float *twd_engine_mat4_mul_scalar(const float src[16], float out[16], float s);
TWD_EXPORT void twd_engine_mat4_from_diag_translation(float mat[16], float tx, float ty, float tz,
                                                      float sx, float sy, float sz);
TWD_EXPORT void twd_engine_refptr_release_vtable(twd_ref_obj_t **slot);
TWD_EXPORT void twd_engine_refptr_detach_dec(twd_ref50_obj_t **slot);
TWD_EXPORT void twd_engine_vtable_destroy_slot2(twd_vtable_obj_t *obj);
TWD_EXPORT void twd_engine_rbtree_rotate_left(twd_rbtree_node_t **head_ptr,
                                              twd_rbtree_node_t *where);
TWD_EXPORT void twd_engine_ptr_array_remove_at(twd_ptr_array_t *arr, int32_t idx);

#ifdef __cplusplus
}
#endif

#endif /* TWD_GAME_H */
