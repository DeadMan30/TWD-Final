/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*lua_pushnum_fn_t)(twd_lua_state_t *L, float n);

static void mutant_zero_float(twd_lua_state_t *L, float n) {
    (void)n;
    L->top->v.f = 0.0f;
    L->top->tt = 3;
    L->top++;
}

static void ref_lua_pushnumber(twd_lua_state_t *L, float n) {
    L->top->v.f = n;
    L->top->tt = 3;
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
    lua_pushnum_fn_t baseline = ref_lua_pushnumber;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (lua_pushnum_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc; (void)argv;
#endif

    static const uint32_t bits[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x40490fdbu, 0x7f800000u, 0x7fc00000u};
    const int total = (int)(sizeof(bits) / sizeof(bits[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        float val = 0.0f;
        memcpy(&val, &bits[i], 4);
        twd_lua_tvalue_t sa[4], sb[4], sm[4];
        memset(sa, 0xa5, sizeof(sa));
        memcpy(sb, sa, sizeof(sb)); memcpy(sm, sa, sizeof(sm));
        twd_lua_state_t la = {0}, lb = {0}, lm = {0};
        la.top = &sa[0]; lb.top = &sb[0]; lm.top = &sm[0];
        baseline(&la, val); twd_engine_lua_pushnumber(&lb, val); mutant_zero_float(&lm, val);
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
