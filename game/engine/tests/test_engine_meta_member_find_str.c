/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef twd_meta_member_t *(*find_str_fn_t)(const twd_meta_class_desc_t *desc,
                                             const twd_string_t *name);

static twd_meta_member_t *mutant_first_only(const twd_meta_class_desc_t *desc,
                                            const twd_string_t *name) {
    (void)name;
    return desc->members;
}

static twd_meta_member_t *ref_member_find_str(const twd_meta_class_desc_t *desc,
                                              const twd_string_t *name) {
    const char *s = (name->cap < 0x10u) ? name->u.buf : name->u.ptr;
    twd_meta_member_t *m = desc->members;
    while (m != NULL && strcmp(m->name, s) != 0) {
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
    find_str_fn_t baseline = ref_member_find_str;
    int live_orig = 0;
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    void *exec_mem = NULL;
    if (argc >= 2 && argv[1][0] != '\0') {
        exec_mem = load_exec_code(argv[1]);
        if (!exec_mem) return 2;
        baseline = (find_str_fn_t)exec_mem;
        live_orig = 1;
    }
#else
    (void)argc;
    (void)argv;
#endif

    twd_meta_member_t m[3];
    memset(m, 0, sizeof(m));
    m[0].name = "mName";
    m[0].next = &m[1];
    m[1].name = "mLongMemberIdentifierOver16";
    m[1].next = &m[2];
    m[2].name = "";
    m[2].next = NULL;

    twd_meta_class_desc_t empty_desc;
    memset(&empty_desc, 0, sizeof(empty_desc));
    twd_meta_class_desc_t full_desc;
    memset(&full_desc, 0, sizeof(full_desc));
    full_desc.members = &m[0];

    twd_string_t s_short;
    memset(&s_short, 0, sizeof(s_short));
    strcpy(s_short.u.buf, "mName");
    s_short.len = 5;
    s_short.cap = 15;

    twd_string_t s_heap;
    memset(&s_heap, 0, sizeof(s_heap));
    s_heap.u.ptr = "mLongMemberIdentifierOver16";
    s_heap.len = 27;
    s_heap.cap = 31;

    twd_string_t s_empty;
    memset(&s_empty, 0, sizeof(s_empty));
    s_empty.cap = 15;

    twd_string_t s_miss;
    memset(&s_miss, 0, sizeof(s_miss));
    s_miss.u.ptr = "mMissingMemberIdentifier";
    s_miss.len = 24;
    s_miss.cap = 16;

    const struct {
        const twd_meta_class_desc_t *desc;
        const twd_string_t *str;
    } cases[] = {
        {&empty_desc, &s_short},
        {&full_desc,  &s_short},
        {&full_desc,  &s_heap},
        {&full_desc,  &s_empty},
        {&full_desc,  &s_miss},
        {&empty_desc, &s_miss},
    };

    const int total = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0, mutant_killed = 0;

    for (int i = 0; i < total; i++) {
        twd_meta_member_t *want = baseline(cases[i].desc, cases[i].str);
        twd_meta_member_t *got = twd_engine_meta_member_find_str(cases[i].desc, cases[i].str);
        twd_meta_member_t *mut = mutant_first_only(cases[i].desc, cases[i].str);
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
