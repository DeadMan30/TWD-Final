/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef int64_t (*slot_init_fn_t)(twd_atomic_slot_t *obj, int64_t val);

static int64_t mutant_always_overwrite(twd_atomic_slot_t *obj, int64_t val) {
    obj->val = val;
    return val;
}

static int64_t ref_slot_init(twd_atomic_slot_t *obj, int64_t val) {
    if (obj->val == 0) {
        obj->val = val;
        return val;
    }
    return obj->val;
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
    slot_init_fn_t baseline = ref_slot_init;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (slot_init_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const struct {
        int64_t init;
        int64_t new_val;
    } cases[] = {
        {0, 0x140001234LL},
        {0, -1LL},
        {0x7fff00001111LL, 0x22223333LL},
        {-42LL, 0},
    };
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_atomic_slot_t sw, sg, sm;
        memset(&sw, 0x5a, sizeof(sw));
        sw.val = cases[i].init;
        sg = sw;
        sm = sw;

        int64_t rw = baseline(&sw, cases[i].new_val);
        int64_t rg = twd_engine_meta_slot_init_ptr(&sg, cases[i].new_val);
        int64_t rm = mutant_always_overwrite(&sm, cases[i].new_val);

        if (rw != rg || memcmp(&sw, &sg, sizeof(sw)) != 0) failed++; else passed++;
        if (rm != rw || sm.val != sw.val) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
