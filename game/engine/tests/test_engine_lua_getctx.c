/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef int32_t (*lua_getctx_fn_t)(const twd_lua_state_t *L, int32_t *ctx);

static int32_t mutant_no_ctx_write(const twd_lua_state_t *L, int32_t *ctx) {
    (void)ctx;
    const twd_lua_callinfo_t *ci = (const twd_lua_callinfo_t *)L->base_ptr;
    if (ci->callstatus & 0x8u) return ci->status;
    return 0;
}

static int32_t ref_getctx(const twd_lua_state_t *L, int32_t *ctx) {
    const twd_lua_callinfo_t *ci = (const twd_lua_callinfo_t *)L->base_ptr;
    if (ci->callstatus & 0x8u) {
        if (ctx != NULL) *ctx = ci->ctx;
        return ci->status;
    }
    return 0;
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
    lua_getctx_fn_t baseline = ref_getctx;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (lua_getctx_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const struct {
        uint8_t callstatus;
        int32_t ctx_val;
        uint8_t status;
        int pass_ctx;
    } cases[] = {
        {0x00, 99,   1, 1},
        {0x07, -1,   2, 1},
        {0x08, 1234, 1, 1},
        {0x08, -77,  2, 1},
        {0x08, 42,   5, 0},
        {0xff, 0,    3, 1},
    };
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_lua_callinfo_t ci;
        memset(&ci, 0, sizeof(ci));
        ci.callstatus = cases[i].callstatus;
        ci.ctx = cases[i].ctx_val;
        ci.status = cases[i].status;

        twd_lua_state_t L;
        memset(&L, 0, sizeof(L));
        L.base_ptr = (twd_lua_tvalue_t **)&ci;

        int32_t cw = -999, cg = -999, cm = -999;
        int32_t rw = baseline(&L, cases[i].pass_ctx ? &cw : NULL);
        int32_t rg = twd_engine_lua_getctx(&L, cases[i].pass_ctx ? &cg : NULL);
        int32_t rm = mutant_no_ctx_write(&L, cases[i].pass_ctx ? &cm : NULL);
        (void)rm;

        if (rw != rg || cw != cg) failed++; else passed++;
        if (cw != cm) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
