/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

/*
 * Original: WalkingDead4.exe @ 0x14034e1e0 (47 bytes, 97 callers)
 *
 * Advances the 4-word (128-bit) Marsaglia xorshift state in place and returns
 * the new state[3] word. MSVC folded (t >> 8) ^ (w >> 19) into
 * ((w >> 11) ^ t) >> 8 in machine code; both expressions are identical for
 * all uint32_t inputs.
 */
TWD_EXPORT
uint32_t twd_engine_xorshift128_next(uint32_t state[4]) {
    uint32_t s0 = state[0];
    uint32_t t = s0 ^ (s0 << 11);
    state[0] = state[1];
    state[1] = state[2];
    uint32_t w = state[3];
    state[2] = w;
    uint32_t tmp = ((w >> 11) ^ t) >> 8;
    w = tmp ^ w ^ t;
    state[3] = w;
    return w;
}
