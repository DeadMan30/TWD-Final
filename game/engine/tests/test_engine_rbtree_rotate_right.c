/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*rbtree_rot_fn_t)(twd_rbtree_node_t **head_ptr, twd_rbtree_node_t *where);

static void mutant_skip_child_parent(twd_rbtree_node_t **head_ptr, twd_rbtree_node_t *where) {
    twd_rbtree_node_t *promoted = where->left;
    where->left = promoted->right;
    /* bug: omits updating promoted->right->parent when !is_nil */
    promoted->parent = where->parent;
    if (where == (*head_ptr)->parent) {
        (*head_ptr)->parent = promoted;
    } else if (where == where->parent->right) {
        where->parent->right = promoted;
    } else {
        where->parent->left = promoted;
    }
    promoted->right = where;
    where->parent = promoted;
}

static void ref_rbtree_rotate_right(twd_rbtree_node_t **head_ptr, twd_rbtree_node_t *where) {
    twd_rbtree_node_t *p = where->left;
    where->left = p->right;
    if (!p->right->is_nil) {
        p->right->parent = where;
    }
    p->parent = where->parent;
    if (where == (*head_ptr)->parent) {
        (*head_ptr)->parent = p;
    } else if (where == where->parent->right) {
        where->parent->right = p;
    } else {
        where->parent->left = p;
    }
    p->right = where;
    where->parent = p;
}

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
static void *load_exec_code(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 65536 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    void *mem = VirtualAlloc(NULL, (SIZE_T)sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { fclose(f); return NULL; }
    if (fread(mem, 1, (size_t)sz, f) != (size_t)sz) {
        VirtualFree(mem, 0, MEM_RELEASE);
        fclose(f);
        return NULL;
    }
    fclose(f);
    FlushInstructionCache(GetCurrentProcess(), mem, (SIZE_T)sz);
    return mem;
}
#endif

/* nodes[0] is sentinel nil header */
enum { TREE_CAP = 7 };

static int ptr_idx(const twd_rbtree_node_t *p, const twd_rbtree_node_t *base) {
    return (p == NULL) ? -1 : (int)(p - base);
}

static int trees_equal(const twd_rbtree_node_t *a, const twd_rbtree_node_t *b) {
    for (int i = 0; i < TREE_CAP; i++) {
        if (ptr_idx(a[i].left, a) != ptr_idx(b[i].left, b)) return 0;
        if (ptr_idx(a[i].parent, a) != ptr_idx(b[i].parent, b)) return 0;
        if (ptr_idx(a[i].right, a) != ptr_idx(b[i].right, b)) return 0;
        if (a[i].is_nil != b[i].is_nil || a[i].color != b[i].color) return 0;
    }
    return 1;
}

static void init_tree_slot(twd_rbtree_node_t *n, int root_idx) {
    memset(n, 0, sizeof(*n) * TREE_CAP);
    n[0].is_nil = 1;
    n[0].color = 1;
    n[0].left = &n[0];
    n[0].right = &n[0];
    n[0].parent = &n[root_idx];
    for (int i = 1; i < TREE_CAP; i++) {
        n[i].left = &n[0];
        n[i].right = &n[0];
        n[i].parent = &n[0];
    }
}

static int run_rot_case(rbtree_rot_fn_t baseline, int root_idx, int where_idx,
                        void (*build)(twd_rbtree_node_t *), int *mut_killed) {
    twd_rbtree_node_t na[TREE_CAP], nb[TREE_CAP], nm[TREE_CAP];
    init_tree_slot(na, root_idx); build(na);
    init_tree_slot(nb, root_idx); build(nb);
    init_tree_slot(nm, root_idx); build(nm);

    twd_rbtree_node_t *ha = &na[0], *hb = &nb[0], *hm = &nm[0];
    baseline(&ha, &na[where_idx]);
    twd_engine_rbtree_rotate_right(&hb, &nb[where_idx]);
    mutant_skip_child_parent(&hm, &nm[where_idx]);

    if (!trees_equal(na, nm)) *mut_killed = 1;
    if (!trees_equal(na, nb)) {
        fprintf(stderr, "FAIL rbtree case (root=%d where=%d)\n", root_idx, where_idx);
        return 0;
    }
    return 1;
}

/* Tree shapes */
static void shape_root_nil_subright(twd_rbtree_node_t *n) {
    /* root=1, 1->left=2, 2->right=nil */
    n[1].left = &n[2]; n[2].parent = &n[1];
}
static void shape_root_with_subright(twd_rbtree_node_t *n) {
    /* root=1, 1->left=2, 2->right=3 (non-nil!) */
    n[1].left = &n[2]; n[2].parent = &n[1];
    n[2].right = &n[3]; n[3].parent = &n[2];
}
static void shape_left_child_of_parent(twd_rbtree_node_t *n) {
    /* root=1, 1->left=2, 2->left=3, 3->right=4 */
    n[1].left = &n[2]; n[2].parent = &n[1];
    n[2].left = &n[3]; n[3].parent = &n[2];
    n[3].right = &n[4]; n[4].parent = &n[3];
}
static void shape_right_child_of_parent(twd_rbtree_node_t *n) {
    /* root=1, 1->right=2, 2->left=3, 3->right=4, 3->left=5, 2->right=6 */
    n[1].right = &n[2]; n[2].parent = &n[1];
    n[2].left = &n[3]; n[3].parent = &n[2];
    n[2].right = &n[6]; n[6].parent = &n[2];
    n[3].right = &n[4]; n[4].parent = &n[3];
    n[3].left = &n[5]; n[5].parent = &n[3];
}

int main(int argc, char **argv) {
    rbtree_rot_fn_t baseline = ref_rbtree_rotate_right;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (rbtree_rot_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    int total = 4;
    int passed = 0;
    int mutant_killed = 0;

    passed += run_rot_case(baseline, 1, 1, shape_root_nil_subright, &mutant_killed);
    passed += run_rot_case(baseline, 1, 1, shape_root_with_subright, &mutant_killed);
    passed += run_rot_case(baseline, 1, 2, shape_left_child_of_parent, &mutant_killed);
    passed += run_rot_case(baseline, 1, 2, shape_right_child_of_parent, &mutant_killed);

    int failed = total - passed;

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif

    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed,
           live_orig ? "x86_64_live_exe" : "ci_reference");

    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
