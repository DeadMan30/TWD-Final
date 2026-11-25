/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef void (*mat4_id_fn_t)(float mat[16]);

static void mutant_skip_last(float mat[16]) {
    for (int i = 0; i < 15; i++) {
        mat[i] = 0.0f;
    }
    mat[0] = 1.0f;
    mat[5] = 1.0f;
    mat[10] = 1.0f;
}

static void ref_mat4_identity(float mat[16]) {
    memset(mat, 0, 16 * sizeof(float));
    mat[0] = 1.0f;
    mat[5] = 1.0f;
    mat[10] = 1.0f;
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
    mat4_id_fn_t baseline = ref_mat4_identity;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (mat4_id_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    const uint32_t fills[6] = {
        0x00000000u, /* +0.0f */
        0xffffffffu, /* NaN bit pattern */
        0x80000000u, /* -0.0f sign bit */
        0x3f800000u, /* all 1.0f */
        0xa5a5a5a5u, /* sentinel pattern */
        0x7f7fffffu, /* FLT_MAX */
    };

    const int total = 8;
    int passed = 0;
    int failed = 0;
    int mutant_killed = 0;

    for (int i = 0; i < 6; i++) {
        /* 16 guard bytes before and after the 64-byte matrix to verify zero OOB writes. */
        uint8_t buf_orig[96], buf_got[96], buf_mut[96];
        memset(buf_orig, 0x5a, sizeof(buf_orig));
        for (int lane = 0; lane < 16; lane++) {
            memcpy(buf_orig + 16 + lane * 4, &fills[i], 4);
        }
        memcpy(buf_got, buf_orig, sizeof(buf_got));
        memcpy(buf_mut, buf_orig, sizeof(buf_mut));

        baseline((float *)(void *)(buf_orig + 16));
        twd_engine_mat4_identity((float *)(void *)(buf_got + 16));
        mutant_skip_last((float *)(void *)(buf_mut + 16));

        if (memcmp(buf_orig, buf_got, sizeof(buf_orig)) == 0) {
            passed++;
        } else {
            failed++;
        }
        if (memcmp(buf_orig, buf_mut, sizeof(buf_orig)) != 0) {
            mutant_killed = 1;
        }
    }

    /* Case 6: Unaligned +4 byte offset pointer inside guard buffer. */
    {
        uint8_t buf_orig[96], buf_got[96];
        memset(buf_orig, 0xc3, sizeof(buf_orig));
        memcpy(buf_got, buf_orig, sizeof(buf_got));
        baseline((float *)(void *)(buf_orig + 4));
        twd_engine_mat4_identity((float *)(void *)(buf_got + 4));
        if (memcmp(buf_orig, buf_got, sizeof(buf_orig)) == 0) passed++; else failed++;
    }

    /* Case 7: Three contiguous matrices back-to-back (matching FUN_14048f9e0). */
    {
        uint8_t buf_orig[192], buf_got[192];
        memset(buf_orig, 0x77, sizeof(buf_orig));
        memcpy(buf_got, buf_orig, sizeof(buf_got));
        for (int k = 0; k < 3; k++) {
            baseline((float *)(void *)(buf_orig + k * 64));
            twd_engine_mat4_identity((float *)(void *)(buf_got + k * 64));
        }
        if (memcmp(buf_orig, buf_got, sizeof(buf_orig)) == 0) passed++; else failed++;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif

    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed,
           live_orig ? "x86_64_live_exe" : "ci_reference");

    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
