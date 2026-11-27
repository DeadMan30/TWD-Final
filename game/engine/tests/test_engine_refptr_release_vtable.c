/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*ref_rel_fn_t)(twd_ref_obj_t **slot);

static int g_dtor_calls = 0;
static twd_ref_obj_t *g_dtor_last_obj = NULL;
static int32_t g_dtor_last_flag = 0;

static void mock_dtor(twd_ref_obj_t *self, int32_t flags) {
    g_dtor_calls++;
    g_dtor_last_obj = self;
    g_dtor_last_flag = flags;
}

static void (*g_vtbl[2])(twd_ref_obj_t *, int32_t) = {mock_dtor, NULL};

static void mutant_no_clear(twd_ref_obj_t **slot) {
    twd_ref_obj_t *obj = *slot;
    /* bug: forgets *slot = NULL */
    if (obj && --obj->refcount == 0) obj->vtable[0](obj, 1);
}

static void ref_release_vtable(twd_ref_obj_t **slot) {
    twd_ref_obj_t *obj = *slot;
    *slot = NULL;
    if (obj && --obj->refcount == 0) obj->vtable[0](obj, 1);
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

static int run_one(ref_rel_fn_t fn, int init_rc, int non_null, int *out_rc,
                   int *out_calls, int *out_flag, int *out_cleared) {
    twd_ref_obj_t obj;
    memset(&obj, 0, sizeof(obj));
    obj.vtable = g_vtbl;
    obj.refcount = init_rc;
    twd_ref_obj_t *slot = non_null ? &obj : NULL;
    g_dtor_calls = 0;
    g_dtor_last_obj = NULL;
    g_dtor_last_flag = 0;
    fn(&slot);
    *out_rc = obj.refcount;
    *out_calls = g_dtor_calls;
    *out_flag = g_dtor_last_flag;
    *out_cleared = (slot == NULL) ? 1 : 0;
    return (!non_null || g_dtor_calls == 0 || g_dtor_last_obj == &obj) ? 1 : 0;
}

int main(int argc, char **argv) {
    ref_rel_fn_t baseline = ref_release_vtable;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (ref_rel_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const int cases[][2] = {{0, 0}, {1, 2}, {1, 1}, {1, 5}};
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; ++i) {
        int rcw, cw, fw, clw, rcg, cg, fg, clg, rcm, cm, fm, clm;
        int okw = run_one(baseline, cases[i][1], cases[i][0], &rcw, &cw, &fw, &clw);
        int okg = run_one(twd_engine_refptr_release_vtable, cases[i][1], cases[i][0], &rcg, &cg, &fg, &clg);
        (void)run_one(mutant_no_clear, cases[i][1], cases[i][0], &rcm, &cm, &fm, &clm);
        if (okw && okg && rcw == rcg && cw == cg && fw == fg && clw == clg) passed++; else failed++;
        if (clm != clw) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
