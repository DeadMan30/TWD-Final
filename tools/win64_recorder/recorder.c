/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Win64 in-process detour & call-recording shim for WalkingDead4.exe.
 * Uses only Win32 kernel32 APIs so it installs safely from DLL_PROCESS_ATTACH
 * before target CRT / static initializers execute.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_SAMPLES_PER_FN 32
#define LINE_BUF_SIZE 2048

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

typedef void (*orig_meta_reg_t)(twd_meta_class_desc_t *, twd_meta_op_node_t *);
typedef void *(*orig_meta_find_t)(const twd_meta_class_desc_t *, int32_t);
typedef void (*orig_mat4_id_t)(float *);
typedef uint8_t (*orig_handle_eq_t)(const twd_handle_obj_t *const *, const twd_handle_obj_t *const *);
typedef uint8_t (*orig_dlist_rm_t)(twd_dlist_t *, twd_dlist_node_t *);
typedef uint32_t (*orig_xorshift_t)(uint32_t *);

static orig_meta_reg_t g_orig_meta_reg = NULL;
static orig_meta_find_t g_orig_meta_find = NULL;
static orig_mat4_id_t g_orig_mat4_id = NULL;
static orig_handle_eq_t g_orig_handle_eq = NULL;
static orig_dlist_rm_t g_orig_dlist_rm = NULL;
static orig_xorshift_t g_orig_xorshift = NULL;

static SRWLOCK g_log_lock = SRWLOCK_INIT;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static LONG g_cnt_meta_reg = 0;
static LONG g_cnt_meta_find = 0;
static LONG g_cnt_mat4_id = 0;
static LONG g_cnt_handle_eq = 0;
static LONG g_cnt_dlist_rm = 0;
static LONG g_cnt_xorshift = 0;

/* Tiny formatting helpers (zero CRT dependency) */
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
        for (int i = 0; i < n_existing; i++) {
            if (i > 0) fb_ch(&b, ',');
            fb_i64(&b, existing[i]);
        }
        fb_str(&b, "],\"insert\":");
        fb_i64(&b, new_id);
        fb_str(&b, ",\"after\":[");
        for (int i = 0; i < n_after; i++) {
            if (i > 0) fb_ch(&b, ',');
            fb_i64(&b, after[i]);
        }
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
        fb_str(&b, "{\"fn\":\"engine_meta_op_find\",\"query\":");
        fb_i64(&b, op_id);
        fb_str(&b, ",\"ids\":[");
        for (int i = 0; i < n; i++) {
            if (i > 0) fb_ch(&b, ',');
            fb_i64(&b, chain[i]);
        }
        fb_str(&b, "],\"vals\":[");
        for (int i = 0; i < n; i++) {
            if (i > 0) fb_ch(&b, ',');
            fb_u64(&b, fns[i]);
        }
        fb_str(&b, "],\"ret\":");
        fb_u64(&b, (uint64_t)(uintptr_t)ret);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
    return ret;
}

static void detour_mat4_identity(float *mat) {
    LONG idx = InterlockedIncrement(&g_cnt_mat4_id);
    int rec = (idx <= MAX_SAMPLES_PER_FN && mat != NULL);
    uint32_t in_bits[16];
    uint32_t out_bits[16];
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
        for (int i = 0; i < 16; i++) {
            if (i > 0) fb_ch(&b, ',');
            fb_u64(&b, in_bits[i]);
        }
        fb_str(&b, "],\"out\":[");
        for (int i = 0; i < 16; i++) {
            if (i > 0) fb_ch(&b, ',');
            fb_u64(&b, out_bits[i]);
        }
        fb_str(&b, "]}\n");
        fb_write_line(&b);
    }
}

static uint8_t detour_handle_equals(const twd_handle_obj_t *const *lhs,
                                    const twd_handle_obj_t *const *rhs) {
    LONG idx = InterlockedIncrement(&g_cnt_handle_eq);
    int rec = (idx <= MAX_SAMPLES_PER_FN && lhs != NULL && rhs != NULL);
    const twd_handle_obj_t *a = NULL;
    const twd_handle_obj_t *b_ptr = NULL;
    uint64_t a_key = 0;
    uint64_t b_key = 0;
    if (rec) {
        a = *lhs;
        b_ptr = *rhs;
        if (a != NULL) a_key = a->key;
        if (b_ptr != NULL) b_key = b_ptr->key;
    }
    uint8_t ret = g_orig_handle_eq(lhs, rhs);
    if (rec) {
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_handle_equals\",\"a_null\":");
        fb_u64(&b, a == NULL ? 1 : 0);
        fb_str(&b, ",\"b_null\":");
        fb_u64(&b, b_ptr == NULL ? 1 : 0);
        fb_str(&b, ",\"same_ptr\":");
        fb_u64(&b, a == b_ptr ? 1 : 0);
        fb_str(&b, ",\"a_key\":");
        fb_u64(&b, a_key);
        fb_str(&b, ",\"b_key\":");
        fb_u64(&b, b_key);
        fb_str(&b, ",\"ret\":");
        fb_u64(&b, ret ? 1 : 0);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
    return ret;
}

static uint8_t detour_dlist_remove(twd_dlist_t *list, twd_dlist_node_t *node) {
    LONG idx = InterlockedIncrement(&g_cnt_dlist_rm);
    int rec = (idx <= MAX_SAMPLES_PER_FN && list != NULL && node != NULL);
    int32_t count_before = 0;
    int len = 0;
    int rm_pos = -1;
    int has_prev = 0;
    int has_next = 0;
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
        /* Only replay exact topologies where full chain <= 32 nodes is known cleanly */
        if (len != count_before) rec = 0;
    }
    uint8_t ret = g_orig_dlist_rm(list, node);
    if (rec) {
        fmt_buf_t b;
        b.len = 0;
        fb_str(&b, "{\"fn\":\"engine_dlist_remove\",\"n\":");
        fb_i64(&b, len);
        fb_str(&b, ",\"rm_pos\":");
        fb_i64(&b, rm_pos);
        fb_str(&b, ",\"has_prev\":");
        fb_i64(&b, has_prev);
        fb_str(&b, ",\"has_next\":");
        fb_i64(&b, has_next);
        fb_str(&b, ",\"count_after\":");
        fb_i64(&b, list->count);
        fb_str(&b, ",\"ret\":");
        fb_u64(&b, ret ? 1 : 0);
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
        for (int i = 0; i < 4; i++) {
            if (i > 0) fb_ch(&b, ',');
            fb_u64(&b, in_s[i]);
        }
        fb_str(&b, "],\"out\":[");
        for (int i = 0; i < 4; i++) {
            if (i > 0) fb_ch(&b, ',');
            fb_u64(&b, state[i]);
        }
        fb_str(&b, "],\"ret\":");
        fb_u64(&b, ret);
        fb_str(&b, "}\n");
        fb_write_line(&b);
    }
    return ret;
}

static uint8_t *alloc_near_slab(uintptr_t target) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000ULL;
    uintptr_t base = target & ~(gran - 1);

    /* Probe downward and upward within +/- 512 MiB for a free RWX slab inside rel32 range. */
    for (uintptr_t step = gran; step <= 0x20000000ULL; step += gran) {
        uintptr_t cands[2] = {base + step, base > step ? base - step : 0};
        for (int k = 0; k < 2; k++) {
            uintptr_t cand = cands[k];
            if (cand < (uintptr_t)si.lpMinimumApplicationAddress ||
                cand >(uintptr_t)si.lpMaximumApplicationAddress) {
                continue;
            }
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)cand, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                mbi.State == MEM_FREE && mbi.RegionSize >= 0x10000ULL) {
                void *mem = VirtualAlloc((LPVOID)cand, 0x10000ULL,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (mem != NULL) {
                    return (uint8_t *)mem;
                }
            }
        }
    }
    return NULL;
}

static void write_abs_jmp(uint8_t *dst, uintptr_t target) {
    dst[0] = 0xff;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    for (int i = 0; i < 8; i++) {
        dst[6 + i] = (uint8_t)((target >> (i * 8)) & 0xffu);
    }
}

static int install_hook(uintptr_t fn_addr, size_t stolen_len, void *detour,
                        void **out_tramp, uint8_t **slab_cursor) {
    uint8_t *code = (uint8_t *)fn_addr;
    uint8_t *tramp = *slab_cursor;
    for (size_t i = 0; i < stolen_len; i++) {
        tramp[i] = code[i];
    }
    write_abs_jmp(tramp + stolen_len, fn_addr + stolen_len);

    uint8_t *relay = tramp + stolen_len + 14;
    write_abs_jmp(relay, (uintptr_t)detour);
    *slab_cursor = relay + 14;

    int64_t rel = (int64_t)(uintptr_t)relay - (int64_t)(fn_addr + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        return 0;
    }

    DWORD old_prot = 0;
    if (!VirtualProtect(code, stolen_len, PAGE_EXECUTE_READWRITE, &old_prot)) {
        return 0;
    }
    int32_t rel32 = (int32_t)rel;
    code[0] = 0xe9;
    for (int i = 0; i < 4; i++) {
        code[1 + i] = (uint8_t)(((uint32_t)rel32 >> (i * 8)) & 0xffu);
    }
    for (size_t i = 5; i < stolen_len; i++) {
        code[i] = 0x90;
    }
    VirtualProtect(code, stolen_len, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), code, stolen_len);
    FlushInstructionCache(GetCurrentProcess(), tramp, (SIZE_T)(stolen_len + 28));
    *out_tramp = tramp;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char out_path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("TWD_CAPTURE_OUT", out_path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return TRUE;
        }
        g_log_file = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log_file == INVALID_HANDLE_VALUE) {
            return TRUE;
        }

        uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
        uint8_t *slab = alloc_near_slab(base);
        if (!slab) {
            return TRUE;
        }

        /* RVA + exact instruction-boundary stolen length (>=5, zero branches/RIP-rel): */
        install_hook(base + 0x00032410ULL, 7, (void *)detour_meta_op_register,
                     (void **)&g_orig_meta_reg, &slab);
        install_hook(base + 0x00032620ULL, 7, (void *)detour_meta_op_find,
                     (void **)&g_orig_meta_find, &slab);
        install_hook(base + 0x000359f0ULL, 9, (void *)detour_mat4_identity,
                     (void **)&g_orig_mat4_id, &slab);
        install_hook(base + 0x00074930ULL, 6, (void *)detour_handle_equals,
                     (void **)&g_orig_handle_eq, &slab);
        install_hook(base + 0x000b1970ULL, 7, (void *)detour_dlist_remove,
                     (void **)&g_orig_dlist_rm, &slab);
        install_hook(base + 0x0034e1e0ULL, 6, (void *)detour_xorshift128_next,
                     (void **)&g_orig_xorshift, &slab);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log_file != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(g_log_file);
            CloseHandle(g_log_file);
            g_log_file = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}
