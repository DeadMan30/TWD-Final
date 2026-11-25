/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>

_Static_assert(offsetof(twd_meta_op_node_t, fn) == 0x08, "meta op fn offset");
_Static_assert(offsetof(twd_meta_op_node_t, next) == 0x10, "meta op next offset");
_Static_assert(offsetof(twd_meta_class_desc_t, ops) == 0x30, "meta desc ops offset");

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
