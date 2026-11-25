/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef uint32_t (*xorshift_fn_t)(uint32_t state[4]);

struct rng_case {
    const char *label;
    uint32_t init[4];
    int steps;
};

/* Deliberately broken mutant (shift right by 18 instead of 19) to ensure the harness catches bugs. */
static uint32_t mutant_xorshift128_wrong_shift(uint32_t state[4]) {
    uint32_t t = state[0] ^ (state[0] << 11);
    uint32_t w = state[3];
    state[0] = state[1];
    state[1] = state[2];
    state[2] = w;
    w = w ^ (w >> 18) ^ t ^ (t >> 8);
    state[3] = w;
    return w;
}

/* Reference implementation independent of twd_engine_xorshift128_next bit folding for CI mode. */
static uint32_t ref_marsaglia_step(uint32_t state[4]) {
    uint32_t t = state[0] ^ (state[0] << 11);
    state[0] = state[1];
    state[1] = state[2];
    state[2] = state[3];
    state[3] = (state[3] ^ (state[3] >> 19)) ^ (t ^ (t >> 8));
    return state[3];
}

static int states_equal(const uint32_t a[4], const uint32_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
static void *load_exec_code(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "failed to open machine code file: %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 65536 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    void *mem = VirtualAlloc(NULL, (SIZE_T)sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) {
        fclose(f);
        return NULL;
    }
    if (fread(mem, 1, (size_t)sz, f) != (size_t)sz) {
        VirtualFree(mem, 0, MEM_RELEASE);
        fclose(f);
        return NULL;
    }
    fclose(f);
    FlushInstructionCache(GetCurrentProcess(), mem, (SIZE_T)sz);
    *out_len = (size_t)sz;
    return mem;
}
#endif

int main(int argc, char **argv) {
    static const struct rng_case cases[] = {
        {"zero_fixed_point",          {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}, 1},
        {"all_ones",                  {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}, 1},
        {"unit_lane0",                {0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u}, 4},
        {"unit_lane1",                {0x00000000u, 0x00000001u, 0x00000000u, 0x00000000u}, 4},
        {"unit_lane2",                {0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u}, 4},
        {"unit_lane3",                {0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u}, 4},
        {"sign_bit_lane0",            {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u}, 2},
        {"sign_bit_lane3",            {0x00000000u, 0x00000000u, 0x00000000u, 0x80000000u}, 2},
        {"shift11_msb_overflow",      {0x00200000u, 0x12345678u, 0x9abcdef0u, 0x00080000u}, 1},
        {"shift19_lsb_boundary",      {0xffe00000u, 0x000000ffu, 0xff000000u, 0x0007ffffu}, 1},
        {"shift8_carry_boundary",     {0x001fffffu, 0x00000100u, 0x00010000u, 0x7fffffffu}, 1},
        {"alternating_aa_55",         {0xaaaaaaaau, 0x55555555u, 0xa5a5a5a5u, 0x5a5a5a5au}, 3},
        {"mixed_bytes_pattern",       {0xdeadbeefu, 0xcafebabeu, 0x01020304u, 0xf0e0d0c0u}, 5},
        {"max_signed_boundaries",     {0x7fffffffu, 0x80000000u, 0x7fffffffu, 0x80000000u}, 3},
        {"sparse_high_bits",          {0xf0000000u, 0x0f000000u, 0x00f00000u, 0x0000000fu}, 8},
        {"marsaglia_1000_steps",      {123456789u,  362436069u,  521288629u,  88675123u},   1000},
    };

    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0;
    int failed = 0;
    int mutant_killed = 0;
    int live_orig = 0;

    xorshift_fn_t baseline = ref_marsaglia_step;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    size_t exec_len = 0;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1], &exec_len);
        if (!exec_mem) {
            return 2;
        }
        baseline = (xorshift_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    for (int i = 0; i < total; i++) {
        uint32_t s_orig[4];
        uint32_t s_got[4];
        uint32_t s_mut[4];
        memcpy(s_orig, cases[i].init, sizeof(s_orig));
        memcpy(s_got, cases[i].init, sizeof(s_got));
        memcpy(s_mut, cases[i].init, sizeof(s_mut));

        int ok = 1;
        for (int step = 0; step < cases[i].steps; step++) {
            uint32_t r_orig = baseline(s_orig);
            uint32_t r_got = twd_engine_xorshift128_next(s_got);
            uint32_t r_mut = mutant_xorshift128_wrong_shift(s_mut);

            if (r_orig != r_got || !states_equal(s_orig, s_got)) {
                fprintf(stderr,
                        "FAIL case %d (%s) step %d: orig=%08x [%08x %08x %08x %08x] "
                        "got=%08x [%08x %08x %08x %08x]\n",
                        i, cases[i].label, step,
                        r_orig, s_orig[0], s_orig[1], s_orig[2], s_orig[3],
                        r_got, s_got[0], s_got[1], s_got[2], s_got[3]);
                ok = 0;
                break;
            }
            if (r_mut != r_orig || !states_equal(s_mut, s_orig)) {
                mutant_killed = 1;
            }
        }
        if (ok) {
            passed++;
        } else {
            failed++;
        }
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) {
        VirtualFree(exec_mem, 0, MEM_RELEASE);
    }
#endif

    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed,
           live_orig ? "x86_64_live_exe" : "ci_reference");

    if (failed != 0 || passed != total || !mutant_killed) {
        return 1;
    }
    return 0;
}
