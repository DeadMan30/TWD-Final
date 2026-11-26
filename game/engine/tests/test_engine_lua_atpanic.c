/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void *(*lua_atpanic_fn_t)(twd_lua_state_t *L, void *panicf);

static void *mutant_no_update(twd_lua_state_t *L, void *panicf) {
    (void)panicf;
    return L->l_G->panic;
}

static void *ref_atpanic(twd_lua_state_t *L, void *panicf) {
    twd_lua_global_t *g = L->l_G;
    void *old = g->panic;
    g->panic = panicf;
    return old;
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
    lua_atpanic_fn_t baseline = ref_atpanic;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (lua_atpanic_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    void *const ptrs[] = {
        NULL,
        (void *)(uintptr_t)0x140001000ULL,
        (void *)(uintptr_t)0xdeadbeefcafebabeULL,
        NULL,
    };
    const int total = (int)(sizeof(ptrs) / sizeof(ptrs[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_lua_global_t gw, gg, gm;
        memset(&gw, 0, sizeof(gw));
        gw.panic = (void *)(uintptr_t)(0x1000 + i);
        gg = gw;
        gm = gw;
        twd_lua_state_t lw, lg, lm;
        memset(&lw, 0, sizeof(lw));
        lw.l_G = &gw;
        lg = lw; lg.l_G = &gg;
        lm = lw; lm.l_G = &gm;

        void *w = baseline(&lw, ptrs[i]);
        void *g = twd_engine_lua_atpanic(&lg, ptrs[i]);
        (void)mutant_no_update(&lm, ptrs[i]);

        if (w != g || gw.panic != gg.panic) failed++; else passed++;
        if (gm.panic != gw.panic) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
