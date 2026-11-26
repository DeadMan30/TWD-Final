/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*mat4_ts_vec_fn_t)(float mat[16], const float s[3], const float t[3]);

static void mutant_swap_args(float mat[16], const float s[3], const float t[3]) {
    memset(mat, 0, 16 * sizeof(float));
    mat[0] = t[0]; mat[5] = t[1]; mat[10] = t[2];
    mat[12] = s[0]; mat[13] = s[1]; mat[14] = s[2];
    mat[15] = 1.0f;
}

static void ref_ts_vec(float mat[16], const float s[3], const float t[3]) {
    memset(mat, 0, 16 * sizeof(float));
    mat[0] = s[0]; mat[5] = s[1]; mat[10] = s[2];
    mat[12] = t[0]; mat[13] = t[1]; mat[14] = t[2];
    mat[15] = 1.0f;
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
    mat4_ts_vec_fn_t baseline = ref_ts_vec;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (mat4_ts_vec_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const struct { float s[3]; float t[3]; } cases[] = {
        {{1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}},
        {{2.0f, 3.0f, 4.0f}, {10.0f, -20.0f, 30.5f}},
        {{-1.0f, 0.5f, -0.25f}, {-1.25f, 0.0f, 99.0f}},
        {{0.0f, -0.0f, 1.0f}, {1.0f, 2.0f, 3.0f}},
    };
    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        uint8_t bw[96], bg[96], bm[96];
        memset(bw, 0x77, sizeof(bw));
        memcpy(bg, bw, sizeof(bg));
        memcpy(bm, bw, sizeof(bm));

        baseline((float *)(void *)(bw + 16), cases[i].s, cases[i].t);
        twd_engine_mat4_from_scale_translation_vec((float *)(void *)(bg + 16), cases[i].s, cases[i].t);
        mutant_swap_args((float *)(void *)(bm + 16), cases[i].s, cases[i].t);

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
