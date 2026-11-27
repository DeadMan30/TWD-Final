/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef float *(*mat4_mul_s_fn_t)(const float src[16], float out[16], float s);

static float *mutant_skip_last(const float src[16], float out[16], float s) {
    for (int i = 0; i < 15; ++i) out[i] = src[i] * s;
    out[15] = src[15];
    return out;
}

static float *ref_mat4_mul_scalar(const float src[16], float out[16], float s) {
    for (int i = 0; i < 16; ++i) out[i] = src[i] * s;
    return out;
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
    mat4_mul_s_fn_t baseline = ref_mat4_mul_scalar;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (mat4_mul_s_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const float scales[] = {0.0f, 1.0f, -2.5f, 0.25f};
    const int total = (int)(sizeof(scales) / sizeof(scales[0])) + 1;
    int passed = 0, failed = 0, mutant_killed = 0;
    float src[16];
    for (int k = 0; k < 16; ++k) src[k] = (float)(k - 5) * 1.5f;

    for (int i = 0; i < (int)(sizeof(scales) / sizeof(scales[0])); ++i) {
        float ow[16], og[16], om[16];
        memset(ow, 0x5a, sizeof(ow));
        memcpy(og, ow, sizeof(og));
        memcpy(om, ow, sizeof(om));
        float *rw = baseline(src, ow, scales[i]);
        float *rg = twd_engine_mat4_mul_scalar(src, og, scales[i]);
        float *rm = mutant_skip_last(src, om, scales[i]);
        (void)rm;
        if (rw == ow && rg == og && memcmp(ow, og, sizeof(ow)) == 0) passed++; else failed++;
        if (memcmp(ow, om, sizeof(ow)) != 0) mutant_killed = 1;
    }

    {
        float io_w[16], io_g[16];
        memcpy(io_w, src, sizeof(src));
        memcpy(io_g, src, sizeof(src));
        float *rw = baseline(io_w, io_w, -3.0f);
        float *rg = twd_engine_mat4_mul_scalar(io_g, io_g, -3.0f);
        if (rw == io_w && rg == io_g && memcmp(io_w, io_g, sizeof(io_w)) == 0) passed++; else failed++;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
