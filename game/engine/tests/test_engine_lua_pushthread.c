/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef int32_t (*lua_pushthread_fn_t)(twd_lua_state_t *L);

static int32_t mutant_wrong_tt(twd_lua_state_t *L) {
    L->top->v.u64 = (uint64_t)(uintptr_t)L;
    L->top->tt = 8;
    L->top++;
    return L->l_G->mainthread == L;
}

static int32_t ref_pushthread(twd_lua_state_t *L) {
    twd_lua_tvalue_t *top = L->top;
    top->v.u64 = (uint64_t)(uintptr_t)L;
    top->tt = 0x48;
    L->top++;
    return L->l_G->mainthread == L;
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
    lua_pushthread_fn_t baseline = ref_pushthread;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (lua_pushthread_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const int is_main[] = {0, 1, 0, 1};
    const int total = (int)(sizeof(is_main) / sizeof(is_main[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_lua_tvalue_t sw[2], sg[2], sm[2];
        memset(sw, 0xcc, sizeof(sw));
        memset(sg, 0xcc, sizeof(sg));
        memset(sm, 0xcc, sizeof(sm));

        twd_lua_global_t gw, gg, gm;
        memset(&gw, 0, sizeof(gw));
        gg = gw; gm = gw;

        twd_lua_state_t lw, lg, lm;
        memset(&lw, 0, sizeof(lw));
        lw.top = &sw[0]; lw.l_G = &gw;
        lg = lw; lg.top = &sg[0]; lg.l_G = &gg;
        lm = lw; lm.top = &sm[0]; lm.l_G = &gm;

        if (is_main[i]) {
            gw.mainthread = &lw;
            gg.mainthread = &lg;
            gm.mainthread = &lm;
        }

        int32_t rw = baseline(&lw);
        int32_t rg = twd_engine_lua_pushthread(&lg);
        int32_t rm = mutant_wrong_tt(&lm);
        (void)rm;

        int ok = (rw == rg)
              && (lw.top - sw == 1) && (lg.top - sg == 1)
              && (sw[0].v.u64 == (uint64_t)(uintptr_t)&lw)
              && (sg[0].v.u64 == (uint64_t)(uintptr_t)&lg)
              && (sw[0].tt == sg[0].tt);
        if (!ok) failed++; else passed++;
        if (sm[0].tt != sw[0].tt) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
