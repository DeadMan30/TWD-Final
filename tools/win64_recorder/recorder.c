/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Win64 in-process hook-swap & call-recording shim for WalkingDead4.exe.
 * - Capture mode (default): attaches lightweight pass-through detours and logs
 *   live in-game inputs/outputs to TWD_CAPTURE_OUT (JSONL).
 * - Swap mode (when TWD_SWAP_DLL is set): loads twdgame.dll, calls
 *   twd_game_register_hooks(), and routes hooked WalkingDead4.exe functions
 *   directly into the compiled C replacements.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_SAMPLES_PER_FN 32
#define LINE_BUF_SIZE 2048

typedef struct twd_hook_entry {
    uint64_t address;
    const char *name;
    void *replacement;
} twd_hook_entry_t;

typedef struct twd_meta_op_node {
    int32_t op_id;
    uint32_t _pad;
    void *fn;
    struct twd_meta_op_node *next;
} twd_meta_op_node_t;

typedef struct twd_meta_class_desc {
    uint8_t opaque[0x30];
    twd_meta_op_node_t *ops;
} twd_meta_class_desc_t;

typedef struct twd_handle_obj {
    uint8_t opaque[0x28];
    uint64_t key;
} twd_handle_obj_t;

typedef struct twd_dlist_node {
    struct twd_dlist_node *prev;
    struct twd_dlist_node *next;
} twd_dlist_node_t;

typedef struct twd_dlist {
    int32_t count;
    int32_t _pad;
    twd_dlist_node_t *head;
    twd_dlist_node_t *tail;
} twd_dlist_t;

typedef struct twd_rbtree_node {
    struct twd_rbtree_node *left;
    struct twd_rbtree_node *parent;
    struct twd_rbtree_node *right;
    uint8_t color;
    uint8_t is_nil;
    uint8_t _pad[6];
} twd_rbtree_node_t;

typedef struct twd_lua_tvalue {
    union {
        uint64_t u64;
        uint32_t u32;
        float f;
    } v;
    int32_t tt;
    uint32_t _pad;
} twd_lua_tvalue_t;

typedef struct twd_lua_state {
    uint8_t _pad0[0x10];
    twd_lua_tvalue_t *top;
    uint8_t _pad1[0x08];
    twd_lua_tvalue_t **base_ptr;
} twd_lua_state_t;

typedef void (*orig_meta_reg_t)(twd_meta_class_desc_t *, twd_meta_op_node_t *);
typedef void *(*orig_meta_find_t)(const twd_meta_class_desc_t *, int32_t);
typedef void (*orig_mat4_id_t)(float *);
typedef void (*orig_rbtree_rot_t)(twd_rbtree_node_t **, twd_rbtree_node_t *);
typedef uint8_t (*orig_handle_eq_t)(const twd_handle_obj_t *const *, const twd_handle_obj_t *const *);
typedef uint8_t (*orig_dlist_rm_t)(twd_dlist_t *, twd_dlist_node_t *);
typedef uint32_t (*orig_xorshift_t)(uint32_t *);
typedef void (*orig_lua_settop_t)(twd_lua_state_t *, int32_t);
typedef void (*orig_lua_pushint_t)(twd_lua_state_t *, int64_t);
typedef void (*orig_lua_pushbool_t)(twd_lua_state_t *, int32_t);

static orig_meta_reg_t g_orig_meta_reg = NULL;
static orig_meta_find_t g_orig_meta_find = NULL;
static orig_mat4_id_t g_orig_mat4_id = NULL;
static orig_rbtree_rot_t g_orig_rbtree_rot = NULL;
static orig_handle_eq_t g_orig_handle_eq = NULL;
static orig_dlist_rm_t g_orig_dlist_rm = NULL;
static orig_xorshift_t g_orig_xorshift = NULL;
static orig_lua_settop_t g_orig_lua_settop = NULL;
static orig_lua_pushint_t g_orig_lua_pushint = NULL;
static orig_lua_pushbool_t g_orig_lua_pushbool = NULL;

static SRWLOCK g_log_lock = SRWLOCK_INIT;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static LONG g_cnt_meta_reg = 0;
static LONG g_cnt_meta_find = 0;
static LONG g_cnt_mat4_id = 0;
static LONG g_cnt_rbtree_rot = 0;
static LONG g_cnt_handle_eq = 0;
static LONG g_cnt_dlist_rm = 0;
static LONG g_cnt_xorshift = 0;
static LONG g_cnt_lua_settop = 0;
static LONG g_cnt_lua_pushint = 0;
static LONG g_cnt_lua_pushbool = 0;

typedef struct fmt_buf {
    char data[LINE_BUF_SIZE];
    size_t len;
} fmt_buf_t;

static void fb_str(fmt_buf_t *b, const char *s) {
    while (*s && b->len + 1 < LINE_BUF_SIZE) {
        b->data[b->len++] = *s++;
    }
}

static void fb_ch(fmt_buf_t *b, char c) {
    if (b->len + 1 < LINE_BUF_SIZE) {
        b->data[b->len++] = c;
    }
}

static void fb_u64(fmt_buf_t *b, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        fb_ch(b, '0');
        return;
    }
    while (v > 0 && n < 24) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        fb_ch(b, tmp[--n]);
    }
}

static void fb_i64(fmt_buf_t *b, int64_t v) {
    if (v < 0) {
        fb_ch(b, '-');
        fb_u64(b, (uint64_t)(-(v + 1)) + 1ULL);
    } else {
        fb_u64(b, (uint64_t)v);
    }
}

static void fb_write_line(const fmt_buf_t *b) {
    if (g_log_file == INVALID_HANDLE_VALUE) return;
    AcquireSRWLockExclusive(&g_log_lock);
    DWORD written = 0;
    WriteFile(g_log_file, b->data, (DWORD)b->len, &written, NULL);
    FlushFileBuffers(g_log_file);
    ReleaseSRWLockExclusive(&g_log_lock);
}

static void detour_meta_op_register(twd_meta_class_desc_t *desc, twd_meta_op_node_t *node) {
    LONG idx = InterlockedIncrement(&g_cnt_meta_reg);
    int rec = (idx <= MAX_SAMPLES_PER_FN && desc != NULL && node != NULL);
    int32_t existing[32];
    int n_existing = 0;
    int32_t new_id = 0;
    if (rec) {
        new_id = node->op_id;
        const twd_meta_op_node_t *cur = desc->ops;
        while (cur != NULL && n_existing < 32) {
            existing[n_existing++] = cur->op_id;
            cur = cur->next;
        }
    }
    g_orig_meta_reg(desc, node);
    if (rec) {
        int32_t after[32];
        int n_after = 0;
        const twd_meta_op_node_t *cur = desc->ops;
        while (cur != NULL && n_after < 32) {
            after[n_after++] = cur->op_id;
            cur = cur->next;
        }
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_meta_op_register\",\"existing\":[");
        for (int i = 0; i < n_existing; i++) { if (i > 0) fb_ch(&b, ','); fb_i64(&b, existing[i]); }
        fb_str(&b, "],\"insert\":"); fb_i64(&b, new_id);
        fb_str(&b, ",\"after\":[");
        for (int i = 0; i < n_after; i++) { if (i > 0) fb_ch(&b, ','); fb_i64(&b, after[i]); }
        fb_str(&b, "]}\n");
        fb_write_line(&b);
    }
}

static void *detour_meta_op_find(const twd_meta_class_desc_t *desc, int32_t op_id) {
    LONG idx = InterlockedIncrement(&g_cnt_meta_find);
    int rec = (idx <= MAX_SAMPLES_PER_FN && desc != NULL);
    int32_t chain[32];
    uint64_t fns[32];
    int n = 0;
    if (rec) {
        const twd_meta_op_node_t *cur = desc->ops;
        while (cur != NULL && n < 32) {
            chain[n] = cur->op_id;
            fns[n] = (uint64_t)(uintptr_t)cur->fn;
            n++;
            cur = cur->next;
        }
    }
    void *ret = g_orig_meta_find(desc, op_id);
    if (rec) {
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_meta_op_find\",\"query\":"); fb_i64(&b, op_id);
        fb_str(&b, ",\"ids\":[");
        for (int i = 0; i < n; i++) { if (i > 0) fb_ch(&b, ','); fb_i64(&b, chain[i]); }
        fb_str(&b, "],\"vals\":[");
        for (int i = 0; i < n; i++) { if (i > 0) fb_ch(&b, ','); fb_u64(&b, fns[i]); }
        fb_str(&b, "],\"ret\":"); fb_u64(&b, (uint64_t)(uintptr_t)ret);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
    return ret;
}

static void detour_mat4_identity(float *mat) {
    LONG idx = InterlockedIncrement(&g_cnt_mat4_id);
    int rec = (idx <= MAX_SAMPLES_PER_FN && mat != NULL);
    uint32_t in_bits[16], out_bits[16];
    if (rec) {
        const volatile uint32_t *u = (const volatile uint32_t *)(const void *)mat;
        for (int i = 0; i < 16; i++) in_bits[i] = u[i];
    }
    g_orig_mat4_id(mat);
    if (rec) {
        const volatile uint32_t *u = (const volatile uint32_t *)(const void *)mat;
        for (int i = 0; i < 16; i++) out_bits[i] = u[i];
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_mat4_identity\",\"in\":[");
        for (int i = 0; i < 16; i++) { if (i > 0) fb_ch(&b, ','); fb_u64(&b, in_bits[i]); }
        fb_str(&b, "],\"out\":[");
        for (int i = 0; i < 16; i++) { if (i > 0) fb_ch(&b, ','); fb_u64(&b, out_bits[i]); }
        fb_str(&b, "]}\n");
        fb_write_line(&b);
    }
}

static void detour_rbtree_rotate_right(twd_rbtree_node_t **head_ptr, twd_rbtree_node_t *where) {
    LONG idx = InterlockedIncrement(&g_cnt_rbtree_rot);
    int rec = (idx <= MAX_SAMPLES_PER_FN && head_ptr != NULL && *head_ptr != NULL && where != NULL);
    int is_root = 0, is_right_child = 0, subright_nil = 1;
    if (rec) {
        twd_rbtree_node_t *head = *head_ptr;
        is_root = (where == head->parent) ? 1 : 0;
        if (!is_root && where->parent != NULL) {
            is_right_child = (where == where->parent->right) ? 1 : 0;
        }
        if (where->left != NULL && where->left->right != NULL) {
            subright_nil = where->left->right->is_nil ? 1 : 0;
        }
    }
    twd_rbtree_node_t *old_left = rec ? where->left : NULL;
    g_orig_rbtree_rot(head_ptr, where);
    if (rec) {
        int rotated_ok = (old_left != NULL && old_left->right == where && where->parent == old_left) ? 1 : 0;
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_rbtree_rotate_right\",\"is_root\":"); fb_i64(&b, is_root);
        fb_str(&b, ",\"is_right_child\":"); fb_i64(&b, is_right_child);
        fb_str(&b, ",\"subright_nil\":"); fb_i64(&b, subright_nil);
        fb_str(&b, ",\"rotated_ok\":"); fb_i64(&b, rotated_ok);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
}

static uint8_t detour_handle_equals(const twd_handle_obj_t *const *lhs,
                                    const twd_handle_obj_t *const *rhs) {
    LONG idx = InterlockedIncrement(&g_cnt_handle_eq);
    int rec = (idx <= MAX_SAMPLES_PER_FN && lhs != NULL && rhs != NULL);
    const twd_handle_obj_t *a = NULL, *b_ptr = NULL;
    uint64_t a_key = 0, b_key = 0;
    if (rec) {
        a = *lhs; b_ptr = *rhs;
        if (a != NULL) a_key = a->key;
        if (b_ptr != NULL) b_key = b_ptr->key;
    }
    uint8_t ret = g_orig_handle_eq(lhs, rhs);
    if (rec) {
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_handle_equals\",\"a_null\":"); fb_u64(&b, a == NULL ? 1 : 0);
        fb_str(&b, ",\"b_null\":"); fb_u64(&b, b_ptr == NULL ? 1 : 0);
        fb_str(&b, ",\"same_ptr\":"); fb_u64(&b, a == b_ptr ? 1 : 0);
        fb_str(&b, ",\"a_key\":"); fb_u64(&b, a_key);
        fb_str(&b, ",\"b_key\":"); fb_u64(&b, b_key);
        fb_str(&b, ",\"ret\":"); fb_u64(&b, ret ? 1 : 0);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
    return ret;
}

static uint8_t detour_dlist_remove(twd_dlist_t *list, twd_dlist_node_t *node) {
    LONG idx = InterlockedIncrement(&g_cnt_dlist_rm);
    int rec = (idx <= MAX_SAMPLES_PER_FN && list != NULL && node != NULL);
    int32_t count_before = 0;
    int len = 0, rm_pos = -1, has_prev = 0, has_next = 0;
    if (rec) {
        count_before = list->count;
        has_prev = (node->prev != NULL) ? 1 : 0;
        has_next = (node->next != NULL) ? 1 : 0;
        const twd_dlist_node_t *cur = list->head;
        while (cur != NULL && len < 32) {
            if (cur == node) rm_pos = len;
            len++;
            if (cur == list->tail) break;
            cur = cur->next;
        }
        if (len != count_before) rec = 0;
    }
    uint8_t ret = g_orig_dlist_rm(list, node);
    if (rec) {
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_dlist_remove\",\"n\":"); fb_i64(&b, len);
        fb_str(&b, ",\"rm_pos\":"); fb_i64(&b, rm_pos);
        fb_str(&b, ",\"has_prev\":"); fb_i64(&b, has_prev);
        fb_str(&b, ",\"has_next\":"); fb_i64(&b, has_next);
        fb_str(&b, ",\"count_after\":"); fb_i64(&b, list->count);
        fb_str(&b, ",\"ret\":"); fb_u64(&b, ret ? 1 : 0);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
    return ret;
}

static uint32_t detour_xorshift128_next(uint32_t *state) {
    LONG idx = InterlockedIncrement(&g_cnt_xorshift);
    int rec = (idx <= MAX_SAMPLES_PER_FN && state != NULL);
    uint32_t in_s[4] = {0, 0, 0, 0};
    if (rec) {
        for (int i = 0; i < 4; i++) in_s[i] = state[i];
    }
    uint32_t ret = g_orig_xorshift(state);
    if (rec) {
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_xorshift128_next\",\"in\":[");
        for (int i = 0; i < 4; i++) { if (i > 0) fb_ch(&b, ','); fb_u64(&b, in_s[i]); }
        fb_str(&b, "],\"out\":[");
        for (int i = 0; i < 4; i++) { if (i > 0) fb_ch(&b, ','); fb_u64(&b, state[i]); }
        fb_str(&b, "],\"ret\":"); fb_u64(&b, ret);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
    return ret;
}

static void detour_lua_settop(twd_lua_state_t *L, int32_t arg_idx) {
    LONG idx = InterlockedIncrement(&g_cnt_lua_settop);
    int rec = (idx <= MAX_SAMPLES_PER_FN && L != NULL && L->base_ptr != NULL && *L->base_ptr != NULL);
    ptrdiff_t start_slots = 0;
    if (rec) {
        start_slots = L->top - *L->base_ptr;
        if (start_slots < 0 || start_slots > 32) rec = 0;
    }
    g_orig_lua_settop(L, arg_idx);
    if (rec) {
        ptrdiff_t end_slots = L->top - *L->base_ptr;
        int nil_count = 0;
        if (arg_idx >= 0 && end_slots > start_slots) {
            for (ptrdiff_t k = start_slots; k < end_slots; k++) {
                if ((*L->base_ptr)[k].tt == 0) nil_count++;
            }
        }
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_lua_settop\",\"start\":"); fb_i64(&b, (int64_t)start_slots);
        fb_str(&b, ",\"idx\":"); fb_i64(&b, arg_idx);
        fb_str(&b, ",\"end\":"); fb_i64(&b, (int64_t)end_slots);
        fb_str(&b, ",\"nil_filled\":"); fb_i64(&b, nil_count);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
}

static void detour_lua_pushinteger(twd_lua_state_t *L, int64_t n) {
    LONG idx = InterlockedIncrement(&g_cnt_lua_pushint);
    int rec = (idx <= MAX_SAMPLES_PER_FN && L != NULL && L->top != NULL);
    g_orig_lua_pushint(L, n);
    if (rec) {
        const twd_lua_tvalue_t *slot = L->top - 1;
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_lua_pushinteger\",\"n\":"); fb_i64(&b, n);
        fb_str(&b, ",\"u32\":"); fb_u64(&b, slot->v.u32);
        fb_str(&b, ",\"tt\":"); fb_i64(&b, slot->tt);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
}

static void detour_lua_pushboolean(twd_lua_state_t *L, int32_t v) {
    LONG idx = InterlockedIncrement(&g_cnt_lua_pushbool);
    int rec = (idx <= MAX_SAMPLES_PER_FN && L != NULL && L->top != NULL);
    g_orig_lua_pushbool(L, v);
    if (rec) {
        const twd_lua_tvalue_t *slot = L->top - 1;
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_lua_pushboolean\",\"b\":"); fb_i64(&b, v);
        fb_str(&b, ",\"u32\":"); fb_u64(&b, slot->v.u32);
        fb_str(&b, ",\"tt\":"); fb_i64(&b, slot->tt);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
}

static uint8_t *alloc_near_slab(uintptr_t target) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000ULL;
    uintptr_t base = target & ~(gran - 1);

    for (uintptr_t step = gran; step <= 0x20000000ULL; step += gran) {
        uintptr_t cands[2] = {base + step, base > step ? base - step : 0};
        for (int k = 0; k < 2; k++) {
            uintptr_t cand = cands[k];
            if (cand < (uintptr_t)si.lpMinimumApplicationAddress ||
                cand > (uintptr_t)si.lpMaximumApplicationAddress) {
                continue;
            }
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)cand, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                mbi.State == MEM_FREE && mbi.RegionSize >= 0x10000ULL) {
                void *mem = VirtualAlloc((LPVOID)cand, 0x10000ULL,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (mem != NULL) return (uint8_t *)mem;
            }
        }
    }
    return NULL;
}

static void write_abs_jmp(uint8_t *dst, uintptr_t target) {
    dst[0] = 0xff; dst[1] = 0x25;
    dst[2] = 0x00; dst[3] = 0x00; dst[4] = 0x00; dst[5] = 0x00;
    for (int i = 0; i < 8; i++) dst[6 + i] = (uint8_t)((target >> (i * 8)) & 0xffu);
}

static int patch_e9(uintptr_t fn_addr, size_t patch_len, void *target_fn, uint8_t **slab_cursor) {
    uint8_t *relay = *slab_cursor;
    write_abs_jmp(relay, (uintptr_t)target_fn);
    *slab_cursor = relay + 14;

    int64_t rel = (int64_t)(uintptr_t)relay - (int64_t)(fn_addr + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return 0;

    uint8_t *code = (uint8_t *)fn_addr;
    DWORD old_prot = 0;
    if (!VirtualProtect(code, patch_len, PAGE_EXECUTE_READWRITE, &old_prot)) return 0;
    int32_t rel32 = (int32_t)rel;
    code[0] = 0xe9;
    for (int i = 0; i < 4; i++) code[1 + i] = (uint8_t)(((uint32_t)rel32 >> (i * 8)) & 0xffu);
    for (size_t i = 5; i < patch_len; i++) code[i] = 0x90;
    VirtualProtect(code, patch_len, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), code, patch_len);
    FlushInstructionCache(GetCurrentProcess(), relay, 14);
    return 1;
}

/* jcc_off >= 0 relocates one 2-byte short Jcc (0x70..0x7f disp8) inside stolen bytes into Jcc rel32. */
static int install_detour(uintptr_t fn_addr, size_t stolen_len, ptrdiff_t jcc_off, void *detour,
                          void **out_tramp, uint8_t **slab_cursor) {
    uint8_t *code = (uint8_t *)fn_addr;
    uint8_t *tramp = *slab_cursor;
    size_t tlen = 0;

    if (jcc_off < 0) {
        for (size_t i = 0; i < stolen_len; i++) tramp[tlen++] = code[i];
    } else {
        size_t j = (size_t)jcc_off;
        for (size_t i = 0; i < j; i++) tramp[tlen++] = code[i];
        uint8_t op = code[j];
        int8_t disp8 = (int8_t)code[j + 1];
        uintptr_t orig_dest = fn_addr + j + 2 + (ptrdiff_t)disp8;
        int64_t new_rel = (int64_t)orig_dest - (int64_t)(uintptr_t)(tramp + tlen + 6);
        if (new_rel < INT32_MIN || new_rel > INT32_MAX) return 0;
        tramp[tlen++] = 0x0f;
        tramp[tlen++] = (uint8_t)(0x80u | (op & 0x0fu));
        int32_t r32 = (int32_t)new_rel;
        for (int k = 0; k < 4; k++) tramp[tlen++] = (uint8_t)(((uint32_t)r32 >> (k * 8)) & 0xffu);
        for (size_t i = j + 2; i < stolen_len; i++) tramp[tlen++] = code[i];
    }
    write_abs_jmp(tramp + tlen, fn_addr + stolen_len);
    tlen += 14;
    FlushInstructionCache(GetCurrentProcess(), tramp, tlen);
    *slab_cursor = tramp + tlen;

    if (!patch_e9(fn_addr, stolen_len, detour, slab_cursor)) return 0;
    *out_tramp = tramp;
    return 1;
}

static int install_swap_hooks(uintptr_t base, const char *swap_dll, uint8_t **slab_cursor) {
    HMODULE mod = LoadLibraryA(swap_dll);
    if (!mod) return 0;
    typedef void (*reg_fn_t)(void);
    typedef uint32_t (*cnt_fn_t)(void);
    typedef const twd_hook_entry_t *(*at_fn_t)(uint32_t);
    reg_fn_t reg_fn = (reg_fn_t)(void *)GetProcAddress(mod, "twd_game_register_hooks");
    cnt_fn_t cnt_fn = (cnt_fn_t)(void *)GetProcAddress(mod, "twd_game_hook_count");
    at_fn_t at_fn = (at_fn_t)(void *)GetProcAddress(mod, "twd_game_hook_at");
    if (!reg_fn || !cnt_fn || !at_fn) return 0;

    reg_fn();
    uint32_t count = cnt_fn();
    int installed = 0;
    fmt_buf_t b;
    b.len = 0;
    fb_str(&b, "{\"swap_mode\":1,\"hooks\":[");
    for (uint32_t i = 0; i < count; i++) {
        const twd_hook_entry_t *h = at_fn(i);
        if (!h || h->address < 0x140000000ULL || !h->replacement) continue;
        uintptr_t target = base + (uintptr_t)(h->address - 0x140000000ULL);
        if (patch_e9(target, 5, h->replacement, slab_cursor)) {
            if (installed > 0) fb_ch(&b, ',');
            fb_ch(&b, '"'); fb_str(&b, h->name); fb_ch(&b, '"');
            installed++;
        }
    }
    fb_str(&b, "],\"count\":"); fb_i64(&b, installed); fb_str(&b, "}\n");
    fb_write_line(&b);
    return installed;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char out_path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("TWD_CAPTURE_OUT", out_path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return TRUE;

        g_log_file = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log_file == INVALID_HANDLE_VALUE) return TRUE;

        uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
        uint8_t *slab = alloc_near_slab(base);
        if (!slab) return TRUE;

        char swap_dll[MAX_PATH];
        DWORD sn = GetEnvironmentVariableA("TWD_SWAP_DLL", swap_dll, MAX_PATH);
        if (sn > 0 && sn < MAX_PATH) {
            install_swap_hooks(base, swap_dll, &slab);
            return TRUE;
        }

        install_detour(base + 0x00032410ULL, 7, -1, (void *)detour_meta_op_register, (void **)&g_orig_meta_reg, &slab);
        install_detour(base + 0x00032620ULL, 7, -1, (void *)detour_meta_op_find, (void **)&g_orig_meta_find, &slab);
        install_detour(base + 0x000359f0ULL, 9, -1, (void *)detour_mat4_identity, (void **)&g_orig_mat4_id, &slab);
        install_detour(base + 0x00057650ULL, 7, -1, (void *)detour_rbtree_rotate_right, (void **)&g_orig_rbtree_rot, &slab);
        install_detour(base + 0x00074930ULL, 6, -1, (void *)detour_handle_equals, (void **)&g_orig_handle_eq, &slab);
        install_detour(base + 0x000b1970ULL, 7, -1, (void *)detour_dlist_remove, (void **)&g_orig_dlist_rm, &slab);
        install_detour(base + 0x0034e1e0ULL, 6, -1, (void *)detour_xorshift128_next, (void **)&g_orig_xorshift, &slab);
        install_detour(base + 0x009519a0ULL, 8,  2, (void *)detour_lua_settop, (void **)&g_orig_lua_settop, &slab);
        install_detour(base + 0x009523c0ULL, 7, -1, (void *)detour_lua_pushinteger, (void **)&g_orig_lua_pushint, &slab);
        install_detour(base + 0x009526a0ULL, 6, -1, (void *)detour_lua_pushboolean, (void **)&g_orig_lua_pushbool, &slab);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log_file != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_log_file);
            CloseHandle(g_log_file);
            g_log_file = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}
