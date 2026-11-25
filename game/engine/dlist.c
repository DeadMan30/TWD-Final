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
    twd_dlist_node_t *head = list->head;
    if (node == head) {
        twd_dlist_node_t *next = head->next;
        list->head = next;
        if (next != NULL) {
            next->prev = NULL;
        } else {
            list->tail = NULL;
        }
        head->prev = NULL;
        head->next = NULL;
        list->count--;
        return true;
    }
    twd_dlist_node_t *tail = list->tail;
    if (node == tail) {
        twd_dlist_node_t *prev = tail->prev;
        list->tail = prev;
        if (prev != NULL) {
            prev->next = NULL;
        } else {
            list->head = NULL;
        }
        tail->prev = NULL;
        tail->next = NULL;
        list->count--;
        return true;
    }
    if (node->next == NULL || node->prev == NULL) {
        return false;
    }
    node->next->prev = node->prev;
    node->prev->next = node->next;
    list->count--;
    node->prev = NULL;
    node->next = NULL;
    return true;
}
