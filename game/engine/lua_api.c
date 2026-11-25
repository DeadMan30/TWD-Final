/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>

_Static_assert(sizeof(twd_lua_tvalue_t) == 0x10, "lua TValue size");
_Static_assert(offsetof(twd_lua_tvalue_t, tt) == 0x08, "lua TValue tt offset");
_Static_assert(offsetof(twd_lua_state_t, top) == 0x10, "lua_State top offset");
_Static_assert(offsetof(twd_lua_state_t, base_ptr) == 0x20, "lua_State base_ptr offset");

/*
 * Original: WalkingDead4.exe @ 0x140951950 (35 bytes, 13 callers)
 *
 * Converts a relative negative stack index (> -1001000 pseudo-index threshold)
 * into a 1-based absolute index, preserving positive indices and pseudo-indices.
 */
TWD_EXPORT
int32_t twd_engine_lua_absindex(const twd_lua_state_t *L, int32_t idx) {
    if ((uint32_t)idx + 0xf4627u <= 0xf4627u) {
        return (int32_t)((uint32_t)idx + (uint32_t)(L->top - *L->base_ptr));
    }
    return idx;
}

/*
 * Original: WalkingDead4.exe @ 0x140951980 (20 bytes, 1,585 callers)
 *
 * Returns the number of elements on the active Lua call frame's stack.
 */
TWD_EXPORT
int64_t twd_engine_lua_gettop(const twd_lua_state_t *L) {
    return (int64_t)(L->top - (*L->base_ptr + 1));
}

/*
 * Original: WalkingDead4.exe @ 0x1409519a0 (71 bytes, 1,549 callers)
 *
 * Sets the Lua stack top to positive index `idx` (nil-filling new slots) or
 * adjusts relative to current top when `idx < 0`.
 */
TWD_EXPORT
void twd_engine_lua_settop(twd_lua_state_t *L, int32_t idx) {
    if (idx >= 0) {
        twd_lua_tvalue_t *func = *L->base_ptr;
        while (L->top < func + 1 + idx) {
            (L->top++)->tt = 0;
        }
        L->top = func + 1 + idx;
    } else {
        L->top += idx + 1;
    }
}

/*
 * Original: WalkingDead4.exe @ 0x140951c60 (91 bytes, 9 callers)
 *
 * Moves `n` values from the top of `from`'s stack onto `to`'s stack.
 */
TWD_EXPORT
void twd_engine_lua_xmove(twd_lua_state_t *from, twd_lua_state_t *to, int32_t n) {
    if (from == to) {
        return;
    }
    from->top += -(ptrdiff_t)n;
    for (int32_t i = 0; i < n; i++) {
        const twd_lua_tvalue_t *io2 = from->top + i;
        twd_lua_tvalue_t *io1 = to->top++;
        io1->v.u64 = io2->v.u64;
        io1->tt = io2->tt;
    }
}

/*
 * Original: WalkingDead4.exe @ 0x140952380 (17 bytes, 332 callers)
 *
 * Pushes LUA_TNIL (0) onto the Lua stack.
 */
TWD_EXPORT
void twd_engine_lua_pushnil(twd_lua_state_t *L) {
    L->top->tt = 0;
    L->top++;
}

/*
 * Original: WalkingDead4.exe @ 0x1409523a0 (21 bytes, 140 callers)
 *
 * Pushes a single-precision float LUA_TNUMBER (3) onto the Lua stack.
 */
TWD_EXPORT
void twd_engine_lua_pushnumber(twd_lua_state_t *L, float n) {
    twd_lua_tvalue_t *top = L->top;
    top->v.f = n;
    top->tt = 3;
    L->top++;
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
    L->top++;
}

/*
 * Original: WalkingDead4.exe @ 0x1409523f0 (48 bytes, 14 callers)
 *
 * Pushes an unsigned 32-bit integer converted to single-precision float
 * LUA_TNUMBER (3) onto the Lua stack.
 */
TWD_EXPORT
void twd_engine_lua_pushunsigned(twd_lua_state_t *L, uint32_t n) {
    float fv = (n <= 0x7fffffffu) ? (float)(int32_t)n : (float)n;
    twd_lua_tvalue_t *top = L->top;
    top->v.f = fv;
    top->tt = 3;
    L->top++;
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
    L->top++;
}

/*
 * Original: WalkingDead4.exe @ 0x1409526d0 (20 bytes, 7 callers)
 *
 * Pushes a light userdata pointer (LUA_TLIGHTUSERDATA == 2) onto the Lua stack.
 */
TWD_EXPORT
void twd_engine_lua_pushlightuserdata(twd_lua_state_t *L, void *p) {
    twd_lua_tvalue_t *top = L->top;
    top->v.u64 = (uint64_t)(uintptr_t)p;
    top->tt = 2;
    L->top++;
}
