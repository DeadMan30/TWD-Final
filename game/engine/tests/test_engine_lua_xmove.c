/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*lua_xmove_fn_t)(twd_lua_state_t *from, twd_lua_state_t *to, int32_t n);

static void mutant_reverse_order(twd_lua_state_t *from, twd_lua_state_t *to, int32_t n) {
    if (from == to) return;
    for (int32_t i = 0; i < n; i++) {
        from->top--;
        *to->top++ = *from->top;
    }
}

static void ref_lua_xmove(twd_lua_state_t *from, twd_lua_state_t *to, int32_t n) {
    if (from == to) return;
    from->top -= n;
    for (int32_t i = 0; i < n; i++) {
        twd_lua_tvalue_t *d = to->top++;
        d->v.u64 = from->top[i].v.u64;
        d->tt = from->top[i].tt;
    }
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
    lua_xmove_fn_t baseline = ref_lua_xmove;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (lua_xmove_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc; (void)argv;
#endif

    static const struct { int same; int start_from; int start_to; int32_t n; } cases[] = {
        {1, 5, 5, 3},
        {0, 5, 1, 0},
        {0, 5, 1, 1},
        {0, 6, 2, 3},
        {0, 8, 1, 5},
    };
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_lua_tvalue_t fa[12], ta[12], fb[12], tb[12], fm[12], tm[12];
        memset(ta, 0x33, sizeof(ta));
        for (int k = 0; k < 12; k++) {
            fa[k].v.u64 = 0x1000ULL + (uint64_t)k;
            fa[k].tt = (k % 4) + 1;
            fa[k].v.u32 = (uint32_t)(k + 10);
        }
        memcpy(fb, fa, sizeof(fb)); memcpy(fm, fa, sizeof(fm));
        memcpy(tb, ta, sizeof(tb)); memcpy(tm, ta, sizeof(tm));

        twd_lua_state_t Lfa = {0}, Lta = {0}, Lfb = {0}, Ltb = {0}, Lfm = {0}, Ltm = {0};
        Lfa.top = &fa[cases[i].start_from]; Lta.top = &ta[cases[i].start_to];
        Lfb.top = &fb[cases[i].start_from]; Ltb.top = &tb[cases[i].start_to];
        Lfm.top = &fm[cases[i].start_from]; Ltm.top = &tm[cases[i].start_to];

        baseline(&Lfa, cases[i].same ? &Lfa : &Lta, cases[i].n);
        twd_engine_lua_xmove(&Lfb, cases[i].same ? &Lfb : &Ltb, cases[i].n);
        mutant_reverse_order(&Lfm, cases[i].same ? &Lfm : &Ltm, cases[i].n);

        if ((Lta.top - ta) != (Ltm.top - tm) || memcmp(ta, tm, sizeof(ta)) != 0) mutant_killed = 1;
        if ((Lfa.top - fa) == (Lfb.top - fb) && (Lta.top - ta) == (Ltb.top - tb) &&
            memcmp(fa, fb, sizeof(fa)) == 0 && memcmp(ta, tb, sizeof(ta)) == 0) {
            passed++;
        } else {
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
