/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*meta_reg_fn_t)(twd_meta_class_desc_t *desc, twd_meta_op_node_t *node);

static void mutant_always_prepend(twd_meta_class_desc_t *desc, twd_meta_op_node_t *node) {
    node->next = desc->ops;
    desc->ops = node;
}

static void ref_meta_op_register(twd_meta_class_desc_t *desc, twd_meta_op_node_t *node) {
    twd_meta_op_node_t *head = desc->ops;
    for (twd_meta_op_node_t *cur = head; cur != NULL; cur = cur->next) {
        if (cur->op_id == node->op_id) {
            return;
        }
    }
    node->next = head;
    desc->ops = node;
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

static int compare_chains(const twd_meta_op_node_t *a_head, const twd_meta_op_node_t *a_base,
                          const twd_meta_op_node_t *b_head, const twd_meta_op_node_t *b_base) {
    const twd_meta_op_node_t *pa = a_head;
    const twd_meta_op_node_t *pb = b_head;
    for (int hop = 0; hop < 32; hop++) {
        if (pa == NULL && pb == NULL) return 1;
        if (pa == NULL || pb == NULL) return 0;
        if ((pa - a_base) != (pb - b_base)) return 0;
        if (pa->op_id != pb->op_id || pa->fn != pb->fn) return 0;
        pa = pa->next;
        pb = pb->next;
    }
    return 0;
}

static int run_one(meta_reg_fn_t baseline, meta_reg_fn_t fn, const int32_t *ids,
                   const int *insert_order, int count, int check_mut, int *mut_killed) {
    twd_meta_class_desc_t d_orig, d_got, d_mut;
    twd_meta_op_node_t n_orig[8], n_got[8], n_mut[8];
    memset(&d_orig, 0xa5, sizeof(d_orig));
    memset(&d_got, 0xa5, sizeof(d_got));
    memset(&d_mut, 0xa5, sizeof(d_mut));
    d_orig.ops = NULL;
    d_got.ops = NULL;
    d_mut.ops = NULL;

    for (int i = 0; i < count; i++) {
        void *tag = (void *)(uintptr_t)(0x1000ULL + (uint64_t)(i + 1) * 0x10ULL);
        n_orig[i].op_id = ids[i]; n_orig[i]._pad = 0; n_orig[i].fn = tag;
        n_orig[i].next = (twd_meta_op_node_t *)(uintptr_t)0xdeadbeefdeadbeefULL;
        n_got[i] = n_orig[i];
        n_mut[i] = n_orig[i];
    }

    for (int step = 0; step < count; step++) {
        int idx = insert_order[step];
        baseline(&d_orig, &n_orig[idx]);
        fn(&d_got, &n_got[idx]);
        mutant_always_prepend(&d_mut, &n_mut[idx]);

        if (!compare_chains(d_orig.ops, n_orig, d_got.ops, n_got) ||
            memcmp(d_orig.opaque, d_got.opaque, sizeof(d_orig.opaque)) != 0) {
            return 0;
        }
        if (check_mut && !compare_chains(d_orig.ops, n_orig, d_mut.ops, n_mut)) {
            *mut_killed = 1;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    meta_reg_fn_t baseline = ref_meta_op_register;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (meta_reg_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    int total = 8;
    int passed = 0;
    int failed = 0;
    int mutant_killed = 0;

    static const int32_t ids0[] = {0x4a};
    static const int ord0[] = {0};
    passed += run_one(baseline, twd_engine_meta_op_register, ids0, ord0, 1, 0, &mutant_killed);

    static const int32_t ids1[] = {1, 2, 3, 4};
    static const int ord1[] = {0, 1, 2, 3};
    passed += run_one(baseline, twd_engine_meta_op_register, ids1, ord1, 4, 0, &mutant_killed);

    static const int32_t ids2[] = {0x4a, 0x4a};
    static const int ord2[] = {0, 1};
    passed += run_one(baseline, twd_engine_meta_op_register, ids2, ord2, 2, 1, &mutant_killed);

    static const int32_t ids3[] = {10, 20, 30, 40, 20};
    static const int ord3[] = {0, 1, 2, 3, 4};
    passed += run_one(baseline, twd_engine_meta_op_register, ids3, ord3, 5, 1, &mutant_killed);

    static const int32_t ids4[] = {10, 20, 30, 40, 10};
    static const int ord4[] = {0, 1, 2, 3, 4};
    passed += run_one(baseline, twd_engine_meta_op_register, ids4, ord4, 5, 1, &mutant_killed);

    static const int32_t ids5[] = {0, -1, INT32_MAX, INT32_MIN, INT32_MIN};
    static const int ord5[] = {0, 1, 2, 3, 4};
    passed += run_one(baseline, twd_engine_meta_op_register, ids5, ord5, 5, 1, &mutant_killed);

    static const int32_t ids6[] = {7, 7, 7};
    static const int ord6[] = {0, 1, 2};
    passed += run_one(baseline, twd_engine_meta_op_register, ids6, ord6, 3, 1, &mutant_killed);

    static const int32_t ids7[] = {5, 12, -3, 99, 12, 42, -3, 100};
    static const int ord7[] = {0, 1, 2, 3, 4, 5, 6, 7};
    passed += run_one(baseline, twd_engine_meta_op_register, ids7, ord7, 8, 1, &mutant_killed);

    failed = total - passed;

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif

    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed,
           live_orig ? "x86_64_live_exe" : "ci_reference");

    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
