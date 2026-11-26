/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef twd_meta_member_t *(*find_cstr_fn_t)(const twd_meta_class_desc_t *desc, const char *name);

static twd_meta_member_t *mutant_first_only(const twd_meta_class_desc_t *desc, const char *name) {
    (void)name;
    return desc->members;
}

static twd_meta_member_t *ref_member_find(const twd_meta_class_desc_t *desc, const char *name) {
    twd_meta_member_t *m = desc->members;
    while (m != NULL && strcmp(m->name, name) != 0) {
        m = m->next;
    }
    return m;
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
    find_cstr_fn_t baseline = ref_member_find;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (find_cstr_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    twd_meta_member_t m[3];
    memset(m, 0, sizeof(m));
    m[0].name = "mFlags";
    m[0].next = &m[1];
    m[1].name = "mTransform";
    m[1].next = &m[2];
    m[2].name = "";
    m[2].next = NULL;

    twd_meta_class_desc_t empty_desc;
    memset(&empty_desc, 0, sizeof(empty_desc));
    twd_meta_class_desc_t full_desc;
    memset(&full_desc, 0, sizeof(full_desc));
    full_desc.members = &m[0];

    const struct {
        const twd_meta_class_desc_t *desc;
        const char *name;
    } cases[] = {
        {&empty_desc, "mFlags"},
        {&full_desc,  "mFlags"},
        {&full_desc,  "mTransform"},
        {&full_desc,  ""},
        {&full_desc,  "mMissing"},
        {&full_desc,  "mFlag"},
    };

    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_meta_member_t *want = baseline(cases[i].desc, cases[i].name);
        twd_meta_member_t *got = twd_engine_meta_member_find(cases[i].desc, cases[i].name);
        twd_meta_member_t *mut = mutant_first_only(cases[i].desc, cases[i].name);
        if (want != got) {
            fprintf(stderr, "FAIL case %d\n", i);
            failed++;
        } else {
            passed++;
        }
        if (mut != want) mutant_killed = 1;
    }

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    if (exec_mem) VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif
    printf("VERIF_RESULT total=%d passed=%d failed=%d mutant_killed=%d mode=%s\n",
           total, passed, failed, mutant_killed, live_orig ? "x86_64_live_exe" : "ci_reference");
    return (failed != 0 || passed != total || !mutant_killed) ? 1 : 0;
}
