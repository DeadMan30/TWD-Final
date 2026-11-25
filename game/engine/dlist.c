/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>

_Static_assert(offsetof(twd_dlist_node_t, prev) == 0x00, "dlist prev offset");
_Static_assert(offsetof(twd_dlist_node_t, next) == 0x08, "dlist next offset");
_Static_assert(offsetof(twd_dlist_t, head) == 0x08, "dlist head offset");
_Static_assert(offsetof(twd_dlist_t, tail) == 0x10, "dlist tail offset");

/*
 * Original: WalkingDead4.exe @ 0x1400b1970 (166 bytes, 78 callers)
 *
 * Unlinks a node from an intrusive doubly-linked list, decrements count, and
 * clears the unlinked node's prev/next pointers. Returns false if the node is
 * not head, tail, or doubly linked in the interior.
 */
TWD_EXPORT
bool twd_engine_dlist_remove(twd_dlist_t *list, twd_dlist_node_t *node) {
    if (node == list->head) {
        twd_dlist_node_t *next = node->next;
        list->head = next;
        if (next != NULL) {
            next->prev = NULL;
        } else {
            list->tail = NULL;
        }
    } else if (node == list->tail) {
        twd_dlist_node_t *prev = node->prev;
        list->tail = prev;
        if (prev != NULL) {
            prev->next = NULL;
        } else {
            list->head = NULL;
        }
    } else {
        twd_dlist_node_t *next = node->next;
        twd_dlist_node_t *prev = node->prev;
        if (next == NULL || prev == NULL) {
            return false;
        }
        next->prev = prev;
        prev->next = next;
    }
    node->prev = NULL;
    node->next = NULL;
    list->count--;
    return true;
}
