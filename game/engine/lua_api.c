/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>

_Static_assert(sizeof(twd_lua_tvalue_t) == 0x10, "lua TValue size");
_Static_assert(offsetof(twd_lua_tvalue_t, tt) == 0x08, "lua TValue tt offset");
_Static_assert(offsetof(twd_lua_state_t, top) == 0x10, "lua_State top offset");
_Static_assert(offsetof(twd_lua_state_t, base_ptr) == 0x20, "lua_State base_ptr offset");

/*
 * Original: WalkingDead4.exe @ 0x1409519a0 (71 bytes, 1,549 callers)
 *
 * Sets the Lua stack top to positive index `idx` (nil-filling new slots) or
 * adjusts relative to current top when `idx < 0`.
 */
TWD_EXPORT
void twd_engine_lua_settop(twd_lua_state_t *L, int32_t idx) {
    if (idx < 0) {
        int32_t delta = (int32_t)((uint32_t)idx + 1u);
        L->top += (ptrdiff_t)delta;
        return;
    }
    twd_lua_tvalue_t *new_top = *L->base_ptr + ((ptrdiff_t)idx + 1);
    while (L->top < new_top) {
        L->top->tt = 0;
        L->top++;
    }
    L->top = new_top;
}

/*
 * Original: WalkingDead4.exe @ 0x1409523c0 (29 bytes, 155 callers)
 *
 * Pushes a signed 64-bit integer converted to single-precision float number
 * (LUA_TNUMBER == 3) onto the Lua stack.
 */
TWD_EXPORT
void twd_engine_lua_pushinteger(twd_lua_state_t *L, int64_t n) {
    twd_lua_tvalue_t *top = L->top;
    top->tt = 3;
    top->v.f = (float)n;
    L->top = top + 1;
}

/*
 * Original: WalkingDead4.exe @ 0x1409526a0 (28 bytes, 332 callers)
 *
 * Pushes a normalized boolean (0 or 1, LUA_TBOOLEAN == 1) onto the Lua stack.
 */
TWD_EXPORT
void twd_engine_lua_pushboolean(twd_lua_state_t *L, int32_t b) {
    twd_lua_tvalue_t *top = L->top;
    top->v.u32 = (b != 0) ? 1u : 0u;
    top->tt = 1;
    L->top = top + 1;
}
