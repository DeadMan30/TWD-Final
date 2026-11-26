/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void *(*vt_alloc_fn_t)(const twd_meta_class_desc_t *desc);

static int g_dummy = 42;
static void *mock_alloc_ok(void) { return &g_dummy; }
static void *mock_alloc_null(void) { return NULL; }

static void *mutant_always_null(const twd_meta_class_desc_t *desc) {
    (void)desc;
    return NULL;
}

static void *ref_alloc(const twd_meta_class_desc_t *desc) {
    const twd_meta_vtable_t *vt = desc->vtable;
    if (vt && vt->alloc) {
        void *p = vt->alloc();
        if (p) return p;
    }
    return NULL;
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
    vt_alloc_fn_t baseline = ref_alloc;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (vt_alloc_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    twd_meta_vtable_t vt_empty, vt_null_ret, vt_ok;
    memset(&vt_empty, 0, sizeof(vt_empty));
    memset(&vt_null_ret, 0, sizeof(vt_null_ret));
    vt_null_ret.alloc = mock_alloc_null;
    memset(&vt_ok, 0, sizeof(vt_ok));
    vt_ok.alloc = mock_alloc_ok;

    twd_meta_class_desc_t d_null_vt, d_null_slot, d_null_ret, d_ok;
    memset(&d_null_vt, 0, sizeof(d_null_vt));
    memset(&d_null_slot, 0, sizeof(d_null_slot));
    d_null_slot.vtable = &vt_empty;
    memset(&d_null_ret, 0, sizeof(d_null_ret));
    d_null_ret.vtable = &vt_null_ret;
    memset(&d_ok, 0, sizeof(d_ok));
    d_ok.vtable = &vt_ok;

    const twd_meta_class_desc_t *cases[] = {&d_null_vt, &d_null_slot, &d_null_ret, &d_ok};
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        void *want = baseline(cases[i]);
        void *got = twd_engine_meta_vtable_alloc(cases[i]);
        void *mut = mutant_always_null(cases[i]);
        if (want != got) failed++; else passed++;
        if (mut != want) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
