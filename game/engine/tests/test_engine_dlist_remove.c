/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef bool (*dlist_rm_fn_t)(twd_dlist_t *list, twd_dlist_node_t *node);

static bool mutant_no_tail_clear(twd_dlist_t *list, twd_dlist_node_t *node) {
    if (node == list->head) {
        twd_dlist_node_t *next = node->next;
        list->head = next;
        if (next) next->prev = NULL;
        /* bug: forgets to clear list->tail when emptying 1-node list */
    } else if (node == list->tail) {
        twd_dlist_node_t *prev = node->prev;
        list->tail = prev;
        if (prev) prev->next = NULL;
    } else if (node->prev && node->next) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
    } else {
        return false;
    }
    node->prev = NULL;
    node->next = NULL;
    list->count--;
    return true;
}

static bool ref_dlist_remove(twd_dlist_t *list, twd_dlist_node_t *node) {
    if (node == list->head) {
        twd_dlist_node_t *next = node->next;
        list->head = next;
        if (next) next->prev = NULL; else list->tail = NULL;
    } else if (node == list->tail) {
        twd_dlist_node_t *prev = node->prev;
        list->tail = prev;
        if (prev) prev->next = NULL; else list->head = NULL;
    } else if (node->prev && node->next) {
        node->next->prev = node->prev;
        node->prev->next = node->next;
    } else {
        return false;
    }
    node->prev = NULL;
    node->next = NULL;
    list->count--;
    return true;
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

static void init_chain(twd_dlist_t *list, twd_dlist_node_t *nodes, int n) {
    memset(list, 0, sizeof(*list));
    memset(nodes, 0, sizeof(*nodes) * 5);
    list->count = n;
    if (n == 0) return;
    list->head = &nodes[0];
    list->tail = &nodes[n - 1];
    for (int i = 0; i < n; i++) {
        nodes[i].prev = (i > 0) ? &nodes[i - 1] : NULL;
        nodes[i].next = (i + 1 < n) ? &nodes[i + 1] : NULL;
    }
}

static int ptr_idx(const twd_dlist_node_t *p, const twd_dlist_node_t *base) {
    return p ? (int)(p - base) : -1;
}

static int lists_match(const twd_dlist_t *la, const twd_dlist_node_t *na,
                       const twd_dlist_t *lb, const twd_dlist_node_t *nb, int n) {
    if (la->count != lb->count) return 0;
    if (ptr_idx(la->head, na) != ptr_idx(lb->head, nb)) return 0;
    if (ptr_idx(la->tail, na) != ptr_idx(lb->tail, nb)) return 0;
    for (int i = 0; i < n; i++) {
        if (ptr_idx(na[i].prev, na) != ptr_idx(nb[i].prev, nb)) return 0;
        if (ptr_idx(na[i].next, na) != ptr_idx(nb[i].next, nb)) return 0;
    }
    return 1;
}

static int run_case(dlist_rm_fn_t baseline, int n, int rm_idx,
                    int prev_nonnull, int next_nonnull, int *mut_killed) {
    twd_dlist_t la, lb, lm;
    twd_dlist_node_t na[5], nb[5], nm[5];
    init_chain(&la, na, n);
    init_chain(&lb, nb, n);
    init_chain(&lm, nm, n);

    if (rm_idx >= n) {
        /* Unlinked / half-linked sentinel node (index 4). */
        na[4].prev = prev_nonnull ? &na[0] : NULL;
        na[4].next = next_nonnull ? &na[1] : NULL;
        nb[4].prev = prev_nonnull ? &nb[0] : NULL;
        nb[4].next = next_nonnull ? &nb[1] : NULL;
        nm[4].prev = prev_nonnull ? &nm[0] : NULL;
        nm[4].next = next_nonnull ? &nm[1] : NULL;
    }

    bool wa = baseline(&la, &na[rm_idx]);
    bool wb = twd_engine_dlist_remove(&lb, &nb[rm_idx]);
    bool wm = mutant_no_tail_clear(&lm, &nm[rm_idx]);

    if (wm != wa || !lists_match(&la, na, &lm, nm, 5)) {
        *mut_killed = 1;
    }
    if (wa != wb || !lists_match(&la, na, &lb, nb, 5)) {
        fprintf(stderr, "FAIL dlist case (n=%d rm=%d): wa=%d wb=%d\n", n, rm_idx, (int)wa, (int)wb);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    dlist_rm_fn_t baseline = ref_dlist_remove;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (dlist_rm_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    int total = 10;
    int passed = 0;
    int mutant_killed = 0;

    passed += run_case(baseline, 1, 0, 0, 0, &mutant_killed); /* single element */
    passed += run_case(baseline, 2, 0, 0, 0, &mutant_killed); /* 2-elem head */
    passed += run_case(baseline, 2, 1, 0, 0, &mutant_killed); /* 2-elem tail */
    passed += run_case(baseline, 3, 1, 0, 0, &mutant_killed); /* 3-elem middle */
    passed += run_case(baseline, 4, 0, 0, 0, &mutant_killed); /* 4-elem head */
    passed += run_case(baseline, 4, 2, 0, 0, &mutant_killed); /* 4-elem middle */
    passed += run_case(baseline, 4, 3, 0, 0, &mutant_killed); /* 4-elem tail */
    passed += run_case(baseline, 0, 4, 0, 0, &mutant_killed); /* empty list, unlinked node */
    passed += run_case(baseline, 2, 4, 1, 0, &mutant_killed); /* half-linked (prev only) */
    passed += run_case(baseline, 2, 4, 0, 1, &mutant_killed); /* half-linked (next only) */

    int failed = total - passed;

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif

    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed,
           live_orig ? "x86_64_live_exe" : "ci_reference");

    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
