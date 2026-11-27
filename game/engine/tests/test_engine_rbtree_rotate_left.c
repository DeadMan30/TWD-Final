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
    twd_rbtree_node_t *promoted = where->right;
    where->right = promoted->left;
    /* bug: omits updating promoted->left->parent when !is_nil */
    promoted->parent = where->parent;
    if (where == (*head_ptr)->parent) (*head_ptr)->parent = promoted;
    else if (where == where->parent->left) where->parent->left = promoted;
    else where->parent->right = promoted;
    promoted->left = where;
    where->parent = promoted;
}

static void ref_rbtree_rotate_left(twd_rbtree_node_t **head_ptr, twd_rbtree_node_t *where) {
    twd_rbtree_node_t *p = where->right;
    where->right = p->left;
    if (!p->left->is_nil) p->left->parent = where;
    p->parent = where->parent;
    if (where == (*head_ptr)->parent) (*head_ptr)->parent = p;
    else if (where == where->parent->left) where->parent->left = p;
    else where->parent->right = p;
    p->left = where;
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
    n[0].is_nil = 1; n[0].color = 1;
    n[0].left = &n[0]; n[0].right = &n[0]; n[0].parent = &n[root_idx];
    for (int i = 1; i < TREE_CAP; i++) {
        n[i].left = &n[0]; n[i].right = &n[0]; n[i].parent = &n[0];
    }
}

static void build_case(twd_rbtree_node_t *n, int cid, int *out_where) {
    if (cid == 0) { /* where is root, promoted left is nil */
        init_tree_slot(n, 1);
        n[1].right = &n[2]; n[2].parent = &n[1];
        *out_where = 1;
    } else if (cid == 1) { /* where is root with promoted left subtree */
        init_tree_slot(n, 1);
        n[1].right = &n[2]; n[2].parent = &n[1];
        n[2].left = &n[3];  n[3].parent = &n[2];
        *out_where = 1;
    } else if (cid == 2) { /* where is parent->left */
        init_tree_slot(n, 4);
        n[4].left = &n[1];  n[1].parent = &n[4];
        n[1].right = &n[2]; n[2].parent = &n[1];
        n[2].left = &n[3];  n[3].parent = &n[2];
        *out_where = 1;
    } else { /* where is parent->right */
        init_tree_slot(n, 4);
        n[4].right = &n[1]; n[1].parent = &n[4];
        n[1].right = &n[2]; n[2].parent = &n[1];
        n[2].left = &n[3];  n[3].parent = &n[2];
        n[2].right = &n[5]; n[5].parent = &n[2];
        *out_where = 1;
    }
}

int main(int argc, char **argv) {
    rbtree_rot_fn_t baseline = ref_rbtree_rotate_left;
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

    const int total = 4;
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; ++i) {
        twd_rbtree_node_t nw[TREE_CAP], ng[TREE_CAP], nm[TREE_CAP];
        int wi = 1;
        build_case(nw, i, &wi);
        build_case(ng, i, &wi);
        build_case(nm, i, &wi);
        twd_rbtree_node_t *hw = &nw[0], *hg = &ng[0], *hm = &nm[0];
        baseline(&hw, &nw[wi]);
        twd_engine_rbtree_rotate_left(&hg, &ng[wi]);
        mutant_skip_child_parent(&hm, &nm[wi]);
        if (trees_equal(nw, ng)) passed++; else failed++;
        if (!trees_equal(nw, nm)) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
