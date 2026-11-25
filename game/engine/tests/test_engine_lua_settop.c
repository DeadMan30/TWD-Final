/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*lua_settop_fn_t)(twd_lua_state_t *L, int32_t idx);

static void mutant_no_nil_fill(twd_lua_state_t *L, int32_t idx) {
    if (idx < 0) {
        L->top += (ptrdiff_t)((int32_t)((uint32_t)idx + 1u));
        return;
    }
    L->top = *L->base_ptr + ((ptrdiff_t)idx + 1);
}

static void ref_lua_settop(twd_lua_state_t *L, int32_t idx) {
    if (idx < 0) {
        L->top += (ptrdiff_t)((int32_t)((uint32_t)idx + 1u));
        return;
    }
    twd_lua_tvalue_t *new_top = *L->base_ptr + ((ptrdiff_t)idx + 1);
    while (L->top < new_top) {
        L->top->tt = 0;
        L->top++;
    }
    L->top = new_top;
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

static int stacks_equal(const twd_lua_state_t *a, const twd_lua_tvalue_t *sa,
                        const twd_lua_state_t *b, const twd_lua_tvalue_t *sb, int len) {
    if ((a->top - sa) != (b->top - sb)) return 0;
    for (int i = 0; i < len; i++) {
        if (sa[i].v.u64 != sb[i].v.u64 || sa[i].tt != sb[i].tt) return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    lua_settop_fn_t baseline = ref_lua_settop;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (lua_settop_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const struct {
        int start_top;
        int32_t idx;
    } cases[] = {
        {1, 0},
        {4, 0},
        {4, 2},
        {1, 5},
        {2, 6},
        {5, -1},
        {5, -2},
        {5, -5},
    };

    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0;
    int failed = 0;
    int mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_lua_tvalue_t sa[16], sb[16], sm[16];
        memset(sa, 0x55, sizeof(sa));
        for (int k = 0; k < 16; k++) {
            sa[k].tt = 3;
            sa[k].v.u64 = (uint64_t)(k + 1) * 0x11111111ULL;
        }
        memcpy(sb, sa, sizeof(sb));
        memcpy(sm, sa, sizeof(sm));

        twd_lua_tvalue_t *ba = &sa[0], *bb = &sb[0], *bm = &sm[0];
        twd_lua_state_t la, lb, lm;
        memset(&la, 0, sizeof(la));
        memset(&lb, 0, sizeof(lb));
        memset(&lm, 0, sizeof(lm));
        la.base_ptr = &ba; la.top = &sa[cases[i].start_top];
        lb.base_ptr = &bb; lb.top = &sb[cases[i].start_top];
        lm.base_ptr = &bm; lm.top = &sm[cases[i].start_top];

        baseline(&la, cases[i].idx);
        twd_engine_lua_settop(&lb, cases[i].idx);
        mutant_no_nil_fill(&lm, cases[i].idx);

        if (!stacks_equal(&la, sa, &lm, sm, 16)) mutant_killed = 1;
        if (stacks_equal(&la, sa, &lb, sb, 16)) {
            passed++;
        } else {
            fprintf(stderr, "FAIL lua_settop case %d (start=%d idx=%d)\n",
                    i, cases[i].start_top, cases[i].idx);
            failed++;
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
