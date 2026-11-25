/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*lua_pushlud_fn_t)(twd_lua_state_t *L, void *p);

static void mutant_wrong_tag(twd_lua_state_t *L, void *p) {
    L->top->v.u64 = (uint64_t)(uintptr_t)p;
    L->top->tt = 7; /* LUA_TUSERDATA instead of LUA_TLIGHTUSERDATA (2) */
    L->top++;
}

static void ref_lua_pushlightuserdata(twd_lua_state_t *L, void *p) {
    L->top->v.u64 = (uint64_t)(uintptr_t)p;
    L->top->tt = 2;
    L->top++;
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
    lua_pushlud_fn_t baseline = ref_lua_pushlightuserdata;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (lua_pushlud_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc; (void)argv;
#endif

    static const uintptr_t ptrs[] = {0ULL, 0x12345678ULL, 0xdeadbeefcafebabeULL, UINTPTR_MAX};
    const int total = (int)(sizeof(ptrs) / sizeof(ptrs[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_lua_tvalue_t sa[4], sb[4], sm[4];
        memset(sa, 0xa5, sizeof(sa));
        memcpy(sb, sa, sizeof(sb)); memcpy(sm, sa, sizeof(sm));
        twd_lua_state_t la = {0}, lb = {0}, lm = {0};
        la.top = &sa[0]; lb.top = &sb[0]; lm.top = &sm[0];
        baseline(&la, (void *)ptrs[i]);
        twd_engine_lua_pushlightuserdata(&lb, (void *)ptrs[i]);
        mutant_wrong_tag(&lm, (void *)ptrs[i]);
        if (memcmp(sa, sm, sizeof(sa)) != 0) mutant_killed = 1;
        if ((la.top - sa) == (lb.top - sb) && memcmp(sa, sb, sizeof(sa)) == 0) passed++; else failed++;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
