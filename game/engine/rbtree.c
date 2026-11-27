/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stddef.h>

_Static_assert(offsetof(twd_rbtree_node_t, left) == 0x00, "rbtree left offset");
_Static_assert(offsetof(twd_rbtree_node_t, parent) == 0x08, "rbtree parent offset");
_Static_assert(offsetof(twd_rbtree_node_t, right) == 0x10, "rbtree right offset");
_Static_assert(offsetof(twd_rbtree_node_t, is_nil) == 0x19, "rbtree is_nil offset");

/*
 * Original: WalkingDead4.exe @ 0x140057650 (89 bytes, 148 callers)
 *
 * Performs a right rotation around `where` in an MSVC-layout red-black tree
 * whose sentinel header node is pointed to by *head_ptr.
 */
TWD_EXPORT
void twd_engine_rbtree_rotate_right(twd_rbtree_node_t **head_ptr, twd_rbtree_node_t *where) {
    twd_rbtree_node_t *promoted = where->left;
    where->left = promoted->right;
    if (!promoted->right->is_nil) {
        promoted->right->parent = where;
    }
    promoted->parent = where->parent;

    twd_rbtree_node_t *head = *head_ptr;
    if (where == head->parent) {
        head->parent = promoted;
    } else if (where == where->parent->right) {
        where->parent->right = promoted;
    } else {
        where->parent->left = promoted;
    }
    promoted->right = where;
    where->parent = promoted;
}

/*
 * Original: WalkingDead4.exe @ 0x1400575f0 (85 bytes)
 *
 * Performs a left rotation around `where` in an MSVC-layout red-black tree
 * whose sentinel header node is pointed to by *head_ptr.
 */
TWD_EXPORT
void twd_engine_rbtree_rotate_left(twd_rbtree_node_t **head_ptr, twd_rbtree_node_t *where) {
    twd_rbtree_node_t *promoted = where->right;
    where->right = promoted->left;
    if (!promoted->left->is_nil) {
        promoted->left->parent = where;
    }
    promoted->parent = where->parent;

    twd_rbtree_node_t *head = *head_ptr;
    if (where == head->parent) {
        head->parent = promoted;
    } else if (where == where->parent->left) {
        where->parent->left = promoted;
    } else {
        where->parent->right = promoted;
    }
    promoted->left = where;
    where->parent = promoted;
}
