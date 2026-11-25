/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef int32_t (*lua_absindex_fn_t)(const twd_lua_state_t *L, int32_t idx);

static int32_t mutant_no_pseudo_check(const twd_lua_state_t *L, int32_t idx) {
    if (idx < 0) return idx + (int32_t)(L->top - *L->base_ptr);
    return idx;
}

static int32_t ref_lua_absindex(const twd_lua_state_t *L, int32_t idx) {
    if ((uint32_t)idx + 0xf4627u <= 0xf4627u) {
        return (int32_t)((uint32_t)idx + (uint32_t)(L->top - *L->base_ptr));
    }
    return idx;
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
    if (fread(mem, 1, (size_t)sz, f) != (size_t)sz) { VirtualFree(mem, 0, MEM_RELEASE); fclose(f); return NULL; }
    fclose(f);
    FlushInstructionCache(GetCurrentProcess(), mem, (SIZE_T)sz);
    return mem;
}
#endif

int main(int argc, char **argv) {
    lua_absindex_fn_t baseline = ref_lua_absindex;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (lua_absindex_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc; (void)argv;
#endif

    twd_lua_tvalue_t stack[16];
    memset(stack, 0, sizeof(stack));
    twd_lua_tvalue_t *base = &stack[0];
    twd_lua_state_t L;
    memset(&L, 0, sizeof(L));
    L.base_ptr = &base;
    L.top = &stack[6];

    static const int32_t idxs[] = {1, 5, 0, -1, -5, -1000999, -1001000, -1001001, INT32_MIN, INT32_MAX};
    const int total = (int)(sizeof(idxs) / sizeof(idxs[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        int32_t want = baseline(&L, idxs[i]);
        int32_t got = twd_engine_lua_absindex(&L, idxs[i]);
        int32_t mut = mutant_no_pseudo_check(&L, idxs[i]);
        if (mut != want) mutant_killed = 1;
        if (want == got) passed++; else {
            fprintf(stderr, "FAIL absindex idx=%d: want=%d got=%d\n", idxs[i], want, got);
            failed++;
        }
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
