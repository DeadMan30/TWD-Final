/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>

_Static_assert(offsetof(twd_handle_obj_t, key) == 0x28, "handle object key offset");

/*
 * Original: WalkingDead4.exe @ 0x140074930 (59 bytes, 119 callers)
 *
 * Compares two indirect object handle pointers. If both refer to non-NULL
 * objects with non-zero resource keys at +0x28, compares the keys; otherwise
 * compares the underlying object pointers directly.
 */
TWD_EXPORT
bool twd_engine_handle_equals(const twd_handle_obj_t *const *lhs,
                              const twd_handle_obj_t *const *rhs) {
    const twd_handle_obj_t *a = *lhs;
    const twd_handle_obj_t *b = *rhs;
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (a->key != 0 && b->key != 0) {
        return a->key == b->key;
    }
    return a == b;
}
