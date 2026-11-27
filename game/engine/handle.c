/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

_Static_assert(offsetof(twd_handle_obj_t, key) == 0x28, "handle object key offset");
_Static_assert(offsetof(twd_ref_obj_t, refcount) == 0x08, "ref obj refcount offset");
_Static_assert(offsetof(twd_ref50_obj_t, refcount) == 0x50, "ref50 obj refcount offset");

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
    if (a && b) {
        uint64_t ka = a->key;
        uint64_t kb = b->key;
        if (!ka || !kb) {
            return a == b;
        }
        return ka == kb;
    }
    return !a && !b;
}

/*
 * Original: WalkingDead4.exe @ 0x140043300 (46 bytes)
 *
 * Clears *slot, atomically decrements obj->refcount (+0x08), and invokes
 * vtable[0](obj, 1) when the count drops to 0.
 */
TWD_EXPORT
void twd_engine_refptr_release_vtable(twd_ref_obj_t **slot) {
    twd_ref_obj_t *obj = *slot;
    *slot = NULL;
    if (obj != NULL) {
#ifdef _MSC_VER
        if (_InterlockedDecrement((volatile long *)&obj->refcount) == 0) {
#else
        if (__atomic_sub_fetch(&obj->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
#endif
            obj->vtable[0](obj, 1);
        }
    }
}

/*
 * Original: WalkingDead4.exe @ 0x140043360 (26 bytes)
 *
 * Clears *slot and atomically decrements obj->refcount (+0x50) if non-NULL.
 */
TWD_EXPORT
void twd_engine_refptr_detach_dec(twd_ref50_obj_t **slot) {
    twd_ref50_obj_t *obj = *slot;
    *slot = NULL;
    if (obj != NULL) {
#ifdef _MSC_VER
        (void)_InterlockedExchangeAdd((volatile long *)&obj->refcount, -1);
#else
        __atomic_sub_fetch(&obj->refcount, 1, __ATOMIC_SEQ_CST);
#endif
    }
}
