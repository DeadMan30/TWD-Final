/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

void twd_game_register_hooks(void) {
    twd_register_hook(0x140032340ULL, "engine_meta_slot_init_ptr",
                      (void *)twd_engine_meta_slot_init_ptr);
    twd_register_hook(0x140032360ULL, "engine_meta_member_find_str",
                      (void *)twd_engine_meta_member_find_str);
    twd_register_hook(0x1400323c0ULL, "engine_meta_member_find",
                      (void *)twd_engine_meta_member_find);
    twd_register_hook(0x140032410ULL, "engine_meta_op_register",
                      (void *)twd_engine_meta_op_register);
    twd_register_hook(0x140032510ULL, "engine_meta_vtable_move_construct",
                      (void *)twd_engine_meta_vtable_move_construct);
    twd_register_hook(0x140032540ULL, "engine_meta_vtable_alloc",
                      (void *)twd_engine_meta_vtable_alloc);
    twd_register_hook(0x140032570ULL, "engine_meta_vtable_construct",
                      (void *)twd_engine_meta_vtable_construct);
    twd_register_hook(0x140032590ULL, "engine_meta_vtable_copy_construct",
                      (void *)twd_engine_meta_vtable_copy_construct);
    twd_register_hook(0x1400325c0ULL, "engine_meta_vtable_destruct",
                      (void *)twd_engine_meta_vtable_destruct);
    twd_register_hook(0x1400325f0ULL, "engine_meta_vtable_delete",
                      (void *)twd_engine_meta_vtable_delete);
    twd_register_hook(0x140032620ULL, "engine_meta_op_find",
                      (void *)twd_engine_meta_op_find);
    twd_register_hook(0x1400359f0ULL, "engine_mat4_identity",
                      (void *)twd_engine_mat4_identity);
    twd_register_hook(0x140037630ULL, "engine_mat4_uniform_scale",
                      (void *)twd_engine_mat4_uniform_scale);
    twd_register_hook(0x140037670ULL, "engine_mat4_diagonal_scale",
                      (void *)twd_engine_mat4_diagonal_scale);
    twd_register_hook(0x140037960ULL, "engine_mat4_from_translation",
                      (void *)twd_engine_mat4_from_translation);
    twd_register_hook(0x1400379b0ULL, "engine_mat4_from_scale_translation_vec",
                      (void *)twd_engine_mat4_from_scale_translation_vec);
    twd_register_hook(0x140057650ULL, "engine_rbtree_rotate_right",
                      (void *)twd_engine_rbtree_rotate_right);
    twd_register_hook(0x140074930ULL, "engine_handle_equals",
                      (void *)twd_engine_handle_equals);
    twd_register_hook(0x1400b1970ULL, "engine_dlist_remove",
                      (void *)twd_engine_dlist_remove);
    twd_register_hook(0x14034e1e0ULL, "engine_xorshift128_next",
                      (void *)twd_engine_xorshift128_next);
    twd_register_hook(0x140951910ULL, "engine_lua_atpanic",
                      (void *)twd_engine_lua_atpanic);
    twd_register_hook(0x140951950ULL, "engine_lua_absindex",
                      (void *)twd_engine_lua_absindex);
    twd_register_hook(0x140951980ULL, "engine_lua_gettop",
                      (void *)twd_engine_lua_gettop);
    twd_register_hook(0x1409519a0ULL, "engine_lua_settop",
                      (void *)twd_engine_lua_settop);
    twd_register_hook(0x140951c60ULL, "engine_lua_xmove",
                      (void *)twd_engine_lua_xmove);
    twd_register_hook(0x140952380ULL, "engine_lua_pushnil",
                      (void *)twd_engine_lua_pushnil);
    twd_register_hook(0x1409523a0ULL, "engine_lua_pushnumber",
                      (void *)twd_engine_lua_pushnumber);
    twd_register_hook(0x1409523c0ULL, "engine_lua_pushinteger",
                      (void *)twd_engine_lua_pushinteger);
    twd_register_hook(0x1409523f0ULL, "engine_lua_pushunsigned",
                      (void *)twd_engine_lua_pushunsigned);
    twd_register_hook(0x1409526a0ULL, "engine_lua_pushboolean",
                      (void *)twd_engine_lua_pushboolean);
    twd_register_hook(0x1409526d0ULL, "engine_lua_pushlightuserdata",
                      (void *)twd_engine_lua_pushlightuserdata);
    twd_register_hook(0x1409526f0ULL, "engine_lua_pushthread",
                      (void *)twd_engine_lua_pushthread);
    twd_register_hook(0x140952fa0ULL, "engine_lua_getctx",
                      (void *)twd_engine_lua_getctx);
}

TWD_EXPORT
uint32_t twd_game_hook_count(void) {
    return twd_hook_count();
}

TWD_EXPORT
const twd_hook_entry_t *twd_game_hook_at(uint32_t index) {
    return twd_hook_at(index);
}
