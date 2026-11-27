/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*vt_dest_fn_t)(twd_vtable_obj_t *obj);

static int g_calls = 0;
static void *g_last_self = NULL;
static int32_t g_last_flag = 0;

static void mock_slot2(void *self, int32_t flags) {
    g_calls++;
    g_last_self = self;
    g_last_flag = flags;
}

static void (*g_vtbl[4])(void *, int32_t) = {NULL, NULL, mock_slot2, NULL};

static void mutant_wrong_flag(twd_vtable_obj_t *obj) {
    if (obj) obj->vtable[2](obj, 0);
}

static void ref_vtable_destroy_slot2(twd_vtable_obj_t *obj) {
    if (obj) obj->vtable[2](obj, 1);
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
    vt_dest_fn_t baseline = ref_vtable_destroy_slot2;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (vt_dest_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    twd_vtable_obj_t o1 = {g_vtbl}, o2 = {g_vtbl};
    twd_vtable_obj_t *cases[4] = {NULL, &o1, &o2, NULL};
    const int total = 4;
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; ++i) {
        g_calls = 0; g_last_self = NULL; g_last_flag = -1;
        baseline(cases[i]);
        int cw = g_calls, fw = g_last_flag; void *sw = g_last_self;

        g_calls = 0; g_last_self = NULL; g_last_flag = -1;
        twd_engine_vtable_destroy_slot2(cases[i]);
        int cg = g_calls, fg = g_last_flag; void *sg = g_last_self;

        g_calls = 0; g_last_self = NULL; g_last_flag = -1;
        mutant_wrong_flag(cases[i]);
        int fm = g_last_flag;

        if (cw == cg && fw == fg && sw == sg) passed++; else failed++;
        if (cases[i] && fm != fw) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
