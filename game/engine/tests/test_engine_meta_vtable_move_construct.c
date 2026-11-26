/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*vt_move_fn_t)(const twd_meta_class_desc_t *desc, void *dst, void *src);

static int g_calls = 0;
static void *g_dst = NULL;
static void *g_src = NULL;

static void mock_move(void *dst, void *src) {
    g_calls++;
    g_dst = dst;
    g_src = src;
}

static void mutant_noop(const twd_meta_class_desc_t *desc, void *dst, void *src) {
    (void)desc; (void)dst; (void)src;
}

static void ref_move(const twd_meta_class_desc_t *desc, void *dst, void *src) {
    const twd_meta_vtable_t *vt = desc->vtable;
    if (vt && vt->move_construct) vt->move_construct(dst, src);
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
    vt_move_fn_t baseline = ref_move;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (vt_move_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    twd_meta_vtable_t vt_empty;
    memset(&vt_empty, 0, sizeof(vt_empty));
    twd_meta_vtable_t vt_full;
    memset(&vt_full, 0, sizeof(vt_full));
    vt_full.move_construct = mock_move;

    twd_meta_class_desc_t d_null_vt, d_null_slot, d_live;
    memset(&d_null_vt, 0, sizeof(d_null_vt));
    memset(&d_null_slot, 0, sizeof(d_null_slot));
    d_null_slot.vtable = &vt_empty;
    memset(&d_live, 0, sizeof(d_live));
    d_live.vtable = &vt_full;

    int a = 1, b = 2;
    const struct {
        const twd_meta_class_desc_t *d;
        void *dst;
        void *src;
    } cases[] = {
        {&d_null_vt,   &a, &b},
        {&d_null_slot, &a, &b},
        {&d_live,      &a, &b},
        {&d_live,      NULL, &a},
    };

    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        g_calls = 0; g_dst = NULL; g_src = NULL;
        baseline(cases[i].d, cases[i].dst, cases[i].src);
        int wc = g_calls; void *wd = g_dst; void *ws = g_src;

        g_calls = 0; g_dst = NULL; g_src = NULL;
        twd_engine_meta_vtable_move_construct(cases[i].d, cases[i].dst, cases[i].src);
        int gc = g_calls; void *gd = g_dst; void *gs = g_src;

        g_calls = 0; g_dst = NULL; g_src = NULL;
        mutant_noop(cases[i].d, cases[i].dst, cases[i].src);
        int mc = g_calls;

        if (wc != gc || wd != gd || ws != gs) failed++; else passed++;
        if (mc != wc) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
