/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*ref_det_fn_t)(twd_ref50_obj_t **slot);

static void mutant_inc_instead(twd_ref50_obj_t **slot) {
    twd_ref50_obj_t *obj = *slot;
    *slot = NULL;
    if (obj) obj->refcount++;
}

static void ref_detach_dec(twd_ref50_obj_t **slot) {
    twd_ref50_obj_t *obj = *slot;
    *slot = NULL;
    if (obj) obj->refcount--;
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
    ref_det_fn_t baseline = ref_detach_dec;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (ref_det_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const int cases[][2] = {{0, 0}, {1, 1}, {1, 3}, {1, 42}};
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; ++i) {
        twd_ref50_obj_t ow, og, om;
        memset(&ow, 0x33, sizeof(ow));
        ow.refcount = cases[i][1];
        og = ow; om = ow;
        twd_ref50_obj_t *sw = cases[i][0] ? &ow : NULL;
        twd_ref50_obj_t *sg = cases[i][0] ? &og : NULL;
        twd_ref50_obj_t *sm = cases[i][0] ? &om : NULL;
        baseline(&sw);
        twd_engine_refptr_detach_dec(&sg);
        mutant_inc_instead(&sm);
        if (sw == NULL && sg == NULL && ow.refcount == og.refcount) passed++; else failed++;
        if (cases[i][0] && om.refcount != ow.refcount) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
