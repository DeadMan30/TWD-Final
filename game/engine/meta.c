/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

_Static_assert(offsetof(twd_atomic_slot_t, val) == 0x20, "atomic slot val offset");
_Static_assert(offsetof(twd_meta_member_t, next) == 0x20, "meta member next offset");
_Static_assert(offsetof(twd_meta_vtable_t, move_construct) == 0x28, "meta vtable move offset");
_Static_assert(offsetof(twd_meta_op_node_t, fn) == 0x08, "meta op fn offset");
_Static_assert(offsetof(twd_meta_op_node_t, next) == 0x10, "meta op next offset");
_Static_assert(offsetof(twd_meta_class_desc_t, members) == 0x28, "meta desc members offset");
_Static_assert(offsetof(twd_meta_class_desc_t, ops) == 0x30, "meta desc ops offset");
_Static_assert(offsetof(twd_meta_class_desc_t, vtable) == 0x40, "meta desc vtable offset");
_Static_assert(offsetof(twd_string_t, cap) == 0x20, "string cap offset");

/*
 * Original: WalkingDead4.exe @ 0x140032340 (13 bytes, 1 caller)
 *
 * Atomically initializes obj->val (+0x20) with val if currently 0 and returns
 * the resulting stored value.
 */
TWD_EXPORT
int64_t twd_engine_meta_slot_init_ptr(twd_atomic_slot_t *obj, int64_t val) {
#ifdef _MSC_VER
    int64_t prev = _InterlockedCompareExchange64(&obj->val, val, 0);
    return prev == 0 ? val : prev;
#else
    int64_t expected = 0;
    if (__atomic_compare_exchange_n(&obj->val, &expected, val, 0, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST)) {
        return val;
    }
    return expected;
#endif
}

/*
 * Original: WalkingDead4.exe @ 0x140032360 (84 bytes, 1 caller)
 *
 * Looks up a MetaMemberDescription entry in desc->members matching a
 * Telltale/std::string small-buffer string by name.
 */
TWD_EXPORT
twd_meta_member_t *twd_engine_meta_member_find_str(const twd_meta_class_desc_t *desc,
                                                  const twd_string_t *name) {
    const char *s = (name->cap < 0x10u) ? name->u.buf : name->u.ptr;
    twd_meta_member_t *m = desc->members;
    while (m != NULL && strcmp(m->name, s) != 0) {
        m = m->next;
    }
    return m;
}

/*
 * Original: WalkingDead4.exe @ 0x1400323c0 (68 bytes, 2 callers)
 *
 * Looks up a MetaMemberDescription entry in desc->members by C-string name.
 */
TWD_EXPORT
twd_meta_member_t *twd_engine_meta_member_find(const twd_meta_class_desc_t *desc,
                                              const char *name) {
    twd_meta_member_t *m = desc->members;
    while (m != NULL && strcmp(m->name, name) != 0) {
        m = m->next;
    }
    return m;
}

/*
 * Original: WalkingDead4.exe @ 0x140032410 (39 bytes, 1,138 callers)
 *
 * Prepends a MetaOperation node onto desc->ops if no registered node already
 * matches node->op_id.
 */
TWD_EXPORT
void twd_engine_meta_op_register(twd_meta_class_desc_t *desc, twd_meta_op_node_t *node) {
    twd_meta_op_node_t *head = desc->ops;
    for (twd_meta_op_node_t *cur = head; cur != NULL; cur = cur->next) {
        if (cur->op_id == node->op_id) {
            return;
        }
    }
    node->next = head;
    desc->ops = node;
}

/*
 * Original: WalkingDead4.exe @ 0x140032510 (31 bytes, 5 callers)
 *
 * Invokes the MetaClassDescription vtable move-construct slot (+0x28) if set.
 */
TWD_EXPORT
void twd_engine_meta_vtable_move_construct(const twd_meta_class_desc_t *desc,
                                           void *dst, void *src) {
    const twd_meta_vtable_t *vt = desc->vtable;
    if (vt != NULL && vt->move_construct != NULL) {
        vt->move_construct(dst, src);
    }
}

/*
 * Original: WalkingDead4.exe @ 0x140032540 (35 bytes, 60 callers)
 *
 * Invokes the MetaClassDescription vtable alloc/new slot (+0x00), returning
 * NULL when absent or allocation fails.
 */
TWD_EXPORT
void *twd_engine_meta_vtable_alloc(const twd_meta_class_desc_t *desc) {
    const twd_meta_vtable_t *vt = desc->vtable;
    if (vt != NULL && vt->alloc != NULL) {
        void *p = vt->alloc();
        if (p != NULL) {
            return p;
        }
    }
    return NULL;
}

/*
 * Original: WalkingDead4.exe @ 0x140032570 (25 bytes, 8 callers)
 *
 * Invokes the MetaClassDescription vtable construct slot (+0x10) if set.
 */
TWD_EXPORT
void twd_engine_meta_vtable_construct(const twd_meta_class_desc_t *desc, void *obj) {
    const twd_meta_vtable_t *vt = desc->vtable;
    if (vt != NULL && vt->construct != NULL) {
        vt->construct(obj);
    }
}

/*
 * Original: WalkingDead4.exe @ 0x140032590 (31 bytes, 64 callers)
 *
 * Invokes the MetaClassDescription vtable copy-construct slot (+0x18) if set.
 */
TWD_EXPORT
void twd_engine_meta_vtable_copy_construct(const twd_meta_class_desc_t *desc,
                                           void *dst, const void *src) {
    const twd_meta_vtable_t *vt = desc->vtable;
    if (vt != NULL && vt->copy_construct != NULL) {
        vt->copy_construct(dst, src);
    }
}

/*
 * Original: WalkingDead4.exe @ 0x1400325c0 (30 bytes, 23 callers)
 *
 * Invokes the MetaClassDescription vtable destruct slot (+0x08) when obj is non-NULL.
 */
TWD_EXPORT
void twd_engine_meta_vtable_destruct(const twd_meta_class_desc_t *desc, void *obj) {
    const twd_meta_vtable_t *vt = desc->vtable;
    if (vt != NULL && vt->destruct != NULL && obj != NULL) {
        vt->destruct(obj);
    }
}

/*
 * Original: WalkingDead4.exe @ 0x1400325f0 (30 bytes, 28 callers)
 *
 * Invokes the MetaClassDescription vtable delete slot (+0x20) when obj is non-NULL.
 */
TWD_EXPORT
void twd_engine_meta_vtable_delete(const twd_meta_class_desc_t *desc, void *obj) {
    const twd_meta_vtable_t *vt = desc->vtable;
    if (vt != NULL && vt->delete_obj != NULL && obj != NULL) {
        vt->delete_obj(obj);
    }
}

/*
 * Original: WalkingDead4.exe @ 0x140032620 (37 bytes, 2,114 callers)
 *
 * Searches desc->ops for op_id and returns the associated operation pointer, or
 * NULL if absent.
 */
TWD_EXPORT
void *twd_engine_meta_op_find(const twd_meta_class_desc_t *desc, int32_t op_id) {
    for (const twd_meta_op_node_t *cur = desc->ops; cur != NULL; cur = cur->next) {
        if (cur->op_id == op_id) {
            return cur->fn;
        }
    }
    return NULL;
}

/*
 * Original: WalkingDead4.exe @ 0x140056bd0 (18 bytes)
 *
 * Invokes virtual scalar destructor slot +0x10 with flags=1 if obj is non-NULL.
 */
TWD_EXPORT
void twd_engine_vtable_destroy_slot2(twd_vtable_obj_t *obj) {
    if (obj != NULL) {
        obj->vtable[2](obj, 1);
    }
}
