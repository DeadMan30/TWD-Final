/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

void twd_game_register_hooks(void) {
    twd_register_hook(0x140032410ULL, "engine_meta_op_register",
                      (void *)twd_engine_meta_op_register);
    twd_register_hook(0x140032620ULL, "engine_meta_op_find",
                      (void *)twd_engine_meta_op_find);
    twd_register_hook(0x1400359f0ULL, "engine_mat4_identity",
                      (void *)twd_engine_mat4_identity);
    twd_register_hook(0x140074930ULL, "engine_handle_equals",
                      (void *)twd_engine_handle_equals);
    twd_register_hook(0x1400b1970ULL, "engine_dlist_remove",
                      (void *)twd_engine_dlist_remove);
    twd_register_hook(0x14034e1e0ULL, "engine_xorshift128_next",
                      (void *)twd_engine_xorshift128_next);
}
