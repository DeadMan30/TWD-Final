/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*arr_rm_fn_t)(twd_ptr_array_t *arr, int32_t idx);

static void mutant_no_clear_tail(twd_ptr_array_t *arr, int32_t idx) {
    if (arr->count == 0) return;
    for (int32_t i = idx; i < arr->count - 1; ++i) arr->items[i] = arr->items[i + 1];
    arr->count--;
}

static void ref_ptr_array_remove_at(twd_ptr_array_t *arr, int32_t idx) {
    if (arr->count == 0) return;
    for (int32_t i = idx; i < arr->count - 1; ++i) arr->items[i] = arr->items[i + 1];
    arr->items[arr->count - 1] = NULL;
    arr->count--;
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
    arr_rm_fn_t baseline = ref_ptr_array_remove_at;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (arr_rm_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    int tags[5] = {10, 20, 30, 40, 50};
    const int cases[][2] = {{0, 0}, {5, 0}, {5, 2}, {5, 4}, {1, 0}};
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; ++i) {
        void *iw[5], *ig[5], *im[5];
        for (int k = 0; k < 5; ++k) iw[k] = ig[k] = im[k] = &tags[k];
        twd_ptr_array_t aw, ag, am;
        memset(&aw, 0, sizeof(aw)); aw.count = cases[i][0]; aw.items = iw;
        memset(&ag, 0, sizeof(ag)); ag.count = cases[i][0]; ag.items = ig;
        memset(&am, 0, sizeof(am)); am.count = cases[i][0]; am.items = im;

        baseline(&aw, cases[i][1]);
        twd_engine_ptr_array_remove_at(&ag, cases[i][1]);
        mutant_no_clear_tail(&am, cases[i][1]);

        if (aw.count == ag.count && memcmp(iw, ig, sizeof(iw)) == 0) passed++; else failed++;
        if (am.count != aw.count || memcmp(iw, im, sizeof(iw)) != 0) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
