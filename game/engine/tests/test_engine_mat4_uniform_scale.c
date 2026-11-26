/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*mat4_uscale_fn_t)(float mat[16], float s);

static void mutant_all_s(float mat[16], float s) {
    memset(mat, 0, 16 * sizeof(float));
    mat[0] = s; mat[5] = s; mat[10] = s; mat[15] = s;
}

static void ref_uscale(float mat[16], float s) {
    memset(mat, 0, 16 * sizeof(float));
    mat[0] = s; mat[5] = s; mat[10] = s; mat[15] = 1.0f;
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
    mat4_uscale_fn_t baseline = ref_uscale;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (mat4_uscale_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const float scales[] = {0.0f, -0.0f, 1.0f, 2.5f, -3.25f};
    const int total = (int)(sizeof(scales) / sizeof(scales[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        uint8_t bw[96], bg[96], bm[96];
        memset(bw, 0xaa, sizeof(bw));
        memcpy(bg, bw, sizeof(bg));
        memcpy(bm, bw, sizeof(bm));

        baseline((float *)(void *)(bw + 16), scales[i]);
        twd_engine_mat4_uniform_scale((float *)(void *)(bg + 16), scales[i]);
        mutant_all_s((float *)(void *)(bm + 16), scales[i]);

        if (memcmp(bw, bg, sizeof(bw)) == 0) passed++; else failed++;
        if (memcmp(bw, bm, sizeof(bw)) != 0) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
