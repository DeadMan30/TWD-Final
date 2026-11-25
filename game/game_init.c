/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

void twd_game_register_hooks(void) {
    twd_register_hook(0x14034e1e0ULL, "engine_xorshift128_next",
                      (void *)twd_engine_xorshift128_next);
}
