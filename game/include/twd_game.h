/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TWD_GAME_H
#define TWD_GAME_H

#include "twd_runtime.h"

#ifdef _WIN32
#define TWD_EXPORT __declspec(dllexport)
#else
#define TWD_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct twd_meta_op_node {
    int32_t op_id;
    uint32_t _pad;
    void *fn;
    struct twd_meta_op_node *next;
} twd_meta_op_node_t;

typedef struct twd_meta_class_desc {
    uint8_t opaque[0x30];
    twd_meta_op_node_t *ops;
} twd_meta_class_desc_t;

typedef struct twd_handle_obj {
    uint8_t opaque[0x28];
    uint64_t key;
} twd_handle_obj_t;

typedef struct twd_dlist_node {
    struct twd_dlist_node *prev;
    struct twd_dlist_node *next;
} twd_dlist_node_t;

typedef struct twd_dlist {
    int32_t count;
    int32_t _pad;
    twd_dlist_node_t *head;
    twd_dlist_node_t *tail;
} twd_dlist_t;

TWD_EXPORT void twd_game_register_hooks(void);
TWD_EXPORT void twd_engine_meta_op_register(twd_meta_class_desc_t *desc, twd_meta_op_node_t *node);
TWD_EXPORT void *twd_engine_meta_op_find(const twd_meta_class_desc_t *desc, int32_t op_id);
TWD_EXPORT void twd_engine_mat4_identity(float mat[16]);
TWD_EXPORT bool twd_engine_handle_equals(const twd_handle_obj_t *const *lhs,
                                         const twd_handle_obj_t *const *rhs);
TWD_EXPORT bool twd_engine_dlist_remove(twd_dlist_t *list, twd_dlist_node_t *node);
TWD_EXPORT uint32_t twd_engine_xorshift128_next(uint32_t state[4]);

#ifdef __cplusplus
}
#endif

#endif /* TWD_GAME_H */
