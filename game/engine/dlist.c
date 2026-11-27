/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>

_Static_assert(offsetof(twd_dlist_node_t, prev) == 0x00, "dlist prev offset");
_Static_assert(offsetof(twd_dlist_node_t, next) == 0x08, "dlist next offset");
_Static_assert(offsetof(twd_dlist_t, head) == 0x08, "dlist head offset");
_Static_assert(offsetof(twd_dlist_t, tail) == 0x10, "dlist tail offset");
_Static_assert(offsetof(twd_ptr_array_t, count) == 0x18, "ptr array count offset");
_Static_assert(offsetof(twd_ptr_array_t, items) == 0x20, "ptr array items offset");

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

/*
 * Original: WalkingDead4.exe @ 0x140060410 (88 bytes)
 *
 * Shifts elements after `idx` left by one slot in a pointer array container,
 * clears the vacated tail slot to NULL, and decrements count.
 */
TWD_EXPORT
void twd_engine_ptr_array_remove_at(twd_ptr_array_t *arr, int32_t idx) {
    int32_t n = arr->count;
    if (n == 0) {
        return;
    }
    for (int32_t i = idx; i < arr->count - 1; ++i) {
        arr->items[i] = arr->items[i + 1];
    }
    int32_t last = arr->count;
    arr->count = last - 1;
    arr->items[last - 1] = NULL;
}
