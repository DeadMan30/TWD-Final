/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void *(*meta_find_fn_t)(const twd_meta_class_desc_t *desc, int32_t op_id);

static void *mutant_only_head(const twd_meta_class_desc_t *desc, int32_t op_id) {
    if (desc->ops && desc->ops->op_id == op_id) {
        return desc->ops->fn;
    }
    return NULL;
}

static void *ref_meta_op_find(const twd_meta_class_desc_t *desc, int32_t op_id) {
    for (const twd_meta_op_node_t *cur = desc->ops; cur != NULL; cur = cur->next) {
        if (cur->op_id == op_id) {
            return cur->fn;
        }
    }
    return NULL;
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

int main(int argc, char **argv) {
    meta_find_fn_t baseline = ref_meta_op_find;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (meta_find_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    twd_meta_op_node_t nodes[6];
    nodes[0] = (twd_meta_op_node_t){0,          0, (void *)(uintptr_t)0xdeadbeefcafebabeULL, &nodes[1]};
    nodes[1] = (twd_meta_op_node_t){0x49,       0, (void *)(uintptr_t)0x140385e70ULL,        &nodes[2]};
    nodes[2] = (twd_meta_op_node_t){0x4c,       0, NULL,                                     &nodes[3]};
    nodes[3] = (twd_meta_op_node_t){-1,         0, (void *)(uintptr_t)0xfeedface00000001ULL, &nodes[4]};
    nodes[4] = (twd_meta_op_node_t){INT32_MAX,  0, (void *)(uintptr_t)0x7ffffffffffffff0ULL, &nodes[5]};
    nodes[5] = (twd_meta_op_node_t){INT32_MIN,  0, (void *)(uintptr_t)0x8000000000000008ULL, NULL};

    twd_meta_class_desc_t empty_desc;
    memset(&empty_desc, 0, sizeof(empty_desc));
    twd_meta_class_desc_t full_desc;
    memset(&full_desc, 0x5a, sizeof(full_desc));
    full_desc.ops = &nodes[0];

    struct {
        const twd_meta_class_desc_t *desc;
        int32_t op_id;
    } cases[] = {
        {&empty_desc, 0},
        {&empty_desc, 0x4c},
        {&full_desc,  0},
        {&full_desc,  0x49},
        {&full_desc,  0x4c},
        {&full_desc,  -1},
        {&full_desc,  INT32_MAX},
        {&full_desc,  INT32_MIN},
        {&full_desc,  0x48},
        {&full_desc,  1},
    };

    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0;
    int failed = 0;
    int mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        void *want = baseline(cases[i].desc, cases[i].op_id);
        void *got = twd_engine_meta_op_find(cases[i].desc, cases[i].op_id);
        void *mut = mutant_only_head(cases[i].desc, cases[i].op_id);
        if (want != got) {
            fprintf(stderr, "FAIL case %d (op_id=%d): want=%p got=%p\n", i, cases[i].op_id, want, got);
            failed++;
        } else {
            passed++;
        }
        if (mut != want) {
            mutant_killed = 1;
        }
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif

    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed,
           live_orig ? "x86_64_live_exe" : "ci_reference");

    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
