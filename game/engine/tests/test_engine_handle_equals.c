/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef bool (*handle_eq_fn_t)(const twd_handle_obj_t *const *lhs,
                               const twd_handle_obj_t *const *rhs);

static bool mutant_ptr_only(const twd_handle_obj_t *const *lhs,
                            const twd_handle_obj_t *const *rhs) {
    return *lhs == *rhs;
}

static bool ref_handle_equals(const twd_handle_obj_t *const *lhs,
                              const twd_handle_obj_t *const *rhs) {
    const twd_handle_obj_t *a = *lhs;
    const twd_handle_obj_t *b = *rhs;
    if (a == NULL || b == NULL) return a == b;
    if (a->key != 0 && b->key != 0) return a->key == b->key;
    return a == b;
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
    handle_eq_fn_t baseline = ref_handle_equals;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (handle_eq_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    twd_handle_obj_t obj_zero_a, obj_zero_b, obj_k1_a, obj_k1_b, obj_k2;
    memset(&obj_zero_a, 0, sizeof(obj_zero_a));
    memset(&obj_zero_b, 0xff, sizeof(obj_zero_b));
    obj_zero_b.key = 0;
    memset(&obj_k1_a, 0x11, sizeof(obj_k1_a));
    obj_k1_a.key = 0xcafebabe00000001ULL;
    memset(&obj_k1_b, 0x22, sizeof(obj_k1_b));
    obj_k1_b.key = 0xcafebabe00000001ULL;
    memset(&obj_k2, 0x33, sizeof(obj_k2));
    obj_k2.key = 0xcafebabe00000002ULL;

    struct {
        const twd_handle_obj_t *a;
        const twd_handle_obj_t *b;
    } cases[] = {
        {NULL,       NULL},
        {NULL,       &obj_zero_a},
        {&obj_zero_a, NULL},
        {&obj_zero_a, &obj_zero_a},
        {&obj_zero_a, &obj_zero_b},
        {&obj_zero_a, &obj_k1_a},
        {&obj_k1_a,   &obj_zero_b},
        {&obj_k1_a,   &obj_k1_a},
        {&obj_k1_a,   &obj_k1_b}, /* distinct objects sharing non-zero key -> true */
        {&obj_k1_a,   &obj_k2},   /* distinct objects with distinct keys -> false */
    };

    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0;
    int failed = 0;
    int mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        bool want = baseline(&cases[i].a, &cases[i].b);
        bool got = twd_engine_handle_equals(&cases[i].a, &cases[i].b);
        bool mut = mutant_ptr_only(&cases[i].a, &cases[i].b);
        if (want != got) {
            fprintf(stderr, "FAIL case %d: want=%d got=%d\n", i, (int)want, (int)got);
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
