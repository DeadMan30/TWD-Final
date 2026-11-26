/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

/* Portable UTF-8 <-> UTF-16 helpers shared identically across all targets. */
int twd_utf8_to_utf16(const char *src, int src_len, uint16_t *dst, int dst_cap) {
    if (!src) return 0;
    size_t slen = (src_len < 0) ? strlen(src) : (size_t)src_len;
    int out = 0;
    size_t i = 0;
    while (i < slen) {
        uint8_t c = (uint8_t)src[i];
        uint32_t cp = 0xfffdu;
        if (c < 0x80u) {
            cp = c;
            i += 1;
        } else if ((c & 0xe0u) == 0xc0u && i + 1 < slen) {
            cp = ((uint32_t)(c & 0x1fu) << 6) | ((uint32_t)src[i + 1] & 0x3fu);
            i += 2;
        } else if ((c & 0xf0u) == 0xe0u && i + 2 < slen) {
            cp = ((uint32_t)(c & 0x0fu) << 12) | (((uint32_t)src[i + 1] & 0x3fu) << 6) |
                 ((uint32_t)src[i + 2] & 0x3fu);
            i += 3;
        } else if ((c & 0xf8u) == 0xf0u && i + 3 < slen) {
            cp = ((uint32_t)(c & 0x07u) << 18) | (((uint32_t)src[i + 1] & 0x3fu) << 12) |
                 (((uint32_t)src[i + 2] & 0x3fu) << 6) | ((uint32_t)src[i + 3] & 0x3fu);
            i += 4;
        } else {
            i += 1;
        }
        if (cp <= 0xffffu) {
            if (dst && out < dst_cap) dst[out] = (uint16_t)cp;
            out += 1;
        } else {
            uint32_t adj = cp - 0x10000u;
            if (dst && out + 1 < dst_cap) {
                dst[out] = (uint16_t)(0xd800u + (adj >> 10));
                dst[out + 1] = (uint16_t)(0xdc00u + (adj & 0x3ffu));
            }
            out += 2;
        }
    }
    if (src_len < 0) {
        if (dst && out < dst_cap) dst[out] = 0;
        out += 1;
    }
    return (dst && out > dst_cap) ? 0 : out;
}

int twd_utf16_to_utf8(const uint16_t *src, int src_len, char *dst, int dst_cap) {
    if (!src) return 0;
    size_t slen = 0;
    if (src_len < 0) {
        while (src[slen] != 0) slen++;
    } else {
        slen = (size_t)src_len;
    }
    int out = 0;
    size_t i = 0;
    while (i < slen) {
        uint32_t cp = src[i++];
        if (cp >= 0xd800u && cp <= 0xdbffu && i < slen) {
            uint32_t lo = src[i];
            if (lo >= 0xdc00u && lo <= 0xdfffu) {
                cp = 0x10000u + (((cp - 0xd800u) << 10) | (lo - 0xdc00u));
                i++;
            }
        }
        if (cp < 0x80u) {
            if (dst && out < dst_cap) dst[out] = (char)cp;
            out += 1;
        } else if (cp < 0x800u) {
            if (dst && out + 1 < dst_cap) {
                dst[out] = (char)(0xc0u | (cp >> 6));
                dst[out + 1] = (char)(0x80u | (cp & 0x3fu));
            }
            out += 2;
        } else if (cp < 0x10000u) {
            if (dst && out + 2 < dst_cap) {
                dst[out] = (char)(0xe0u | (cp >> 12));
                dst[out + 1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
                dst[out + 2] = (char)(0x80u | (cp & 0x3fu));
            }
            out += 3;
        } else {
            if (dst && out + 3 < dst_cap) {
                dst[out] = (char)(0xf0u | (cp >> 18));
                dst[out + 1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
                dst[out + 2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
                dst[out + 3] = (char)(0x80u | (cp & 0x3fu));
            }
            out += 4;
        }
    }
    if (src_len < 0) {
        if (dst && out < dst_cap) dst[out] = '\0';
        out += 1;
    }
    return (dst && out > dst_cap) ? 0 : out;
}

void *twd_heap_alloc(size_t size) {
    return calloc(1, size ? size : 1);
}

void *twd_heap_realloc(void *ptr, size_t size) {
    return realloc(ptr, size ? size : 1);
}

void twd_heap_free(void *ptr) {
    free(ptr);
}

void twd_alert(const char *title, const char *msg) {
    twd_log("ALERT", "%s: %s", title ? title : "TWD-Final", msg ? msg : "");
}

int twd_alert_box(const char *title, const char *msg, uint32_t flags) {
    twd_alert(title, msg);
    if ((flags & 0x0fu) == 4u) return TWD_ALERT_YES;
    return TWD_ALERT_OK;
}

size_t twd_utf16_len(const uint16_t *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n] != 0) n++;
    return n;
}

int twd_utf16_cmp(const uint16_t *a, const uint16_t *b, int ignore_case) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a != 0 && *b != 0) {
        uint16_t ca = *a++;
        uint16_t cb = *b++;
        if (ignore_case) {
            if (ca >= 'A' && ca <= 'Z') ca = (uint16_t)(ca + ('a' - 'A'));
            if (cb >= 'A' && cb <= 'Z') cb = (uint16_t)(cb + ('a' - 'A'));
        }
        if (ca != cb) return (ca < cb) ? -1 : 1;
    }
    if (*a == *b) return 0;
    return (*a < *b) ? -1 : 1;
}

#define TWD_WINDOW_MAGIC 0x54574457u
#define TWD_EVT_RING_CAP 64u

static twd_win_event_t g_evt_ring[TWD_EVT_RING_CAP];
static uint32_t g_evt_head = 0;
static uint32_t g_evt_tail = 0;
static int g_quit_flag = 0;
static int g_quit_code = 0;
static twd_mutex_t g_evt_mtx;
static int g_evt_mtx_ready = 0;

static void ensure_evt_lock(void) {
    if (!g_evt_mtx_ready) {
        twd_mutex_init(&g_evt_mtx);
        g_evt_mtx_ready = 1;
    }
}

twd_window_t *twd_window_create(const char *title, int x, int y, int width, int height, uint32_t style) {
    twd_window_t *w = (twd_window_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->magic = TWD_WINDOW_MAGIC;
    w->x = x;
    w->y = y;
    w->width = (width > 0) ? width : 1280;
    w->height = (height > 0) ? height : 720;
    w->style = style;
    w->visible = 0;
    twd_window_set_title(w, title ? title : "The Walking Dead: The Final Season");
    return w;
}

void twd_window_destroy(twd_window_t *win) {
    if (!twd_window_is_valid(win)) return;
    win->magic = 0;
    free(win);
}

int twd_window_is_valid(const twd_window_t *win) {
    return (win && win->magic == TWD_WINDOW_MAGIC) ? 1 : 0;
}

void twd_window_show(twd_window_t *win, int visible) {
    if (twd_window_is_valid(win)) win->visible = (visible != 0) ? 1 : 0;
}

void twd_window_set_title(twd_window_t *win, const char *title) {
    if (!twd_window_is_valid(win)) return;
    if (!title) {
        win->title[0] = '\0';
        return;
    }
    size_t n = strlen(title);
    if (n >= sizeof(win->title)) n = sizeof(win->title) - 1;
    memcpy(win->title, title, n);
    win->title[n] = '\0';
}

int twd_window_get_title(const twd_window_t *win, char *buf, size_t cap) {
    if (!twd_window_is_valid(win)) return -1;
    size_t n = strlen(win->title);
    if (!buf || cap == 0) return (int)n;
    size_t copy = (n < cap - 1) ? n : (cap - 1);
    memcpy(buf, win->title, copy);
    buf[copy] = '\0';
    return (int)copy;
}

void twd_window_set_rect(twd_window_t *win, int x, int y, int width, int height) {
    if (!twd_window_is_valid(win)) return;
    win->x = x;
    win->y = y;
    if (width >= 0) win->width = width;
    if (height >= 0) win->height = height;
}

void twd_window_get_client_rect(const twd_window_t *win, int *out_w, int *out_h) {
    if (!twd_window_is_valid(win)) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }
    if (out_w) *out_w = win->width;
    if (out_h) *out_h = win->height;
}

void twd_window_push_event(const twd_win_event_t *evt) {
    if (!evt) return;
    ensure_evt_lock();
    twd_mutex_lock(&g_evt_mtx);
    uint32_t next = (g_evt_tail + 1u) % TWD_EVT_RING_CAP;
    if (next == g_evt_head) {
        g_evt_head = (g_evt_head + 1u) % TWD_EVT_RING_CAP;
    }
    g_evt_ring[g_evt_tail] = *evt;
    g_evt_tail = next;
    twd_mutex_unlock(&g_evt_mtx);
}

int twd_window_poll_event(twd_win_event_t *out_evt) {
    ensure_evt_lock();
    twd_mutex_lock(&g_evt_mtx);
    if (g_evt_head == g_evt_tail) {
        twd_mutex_unlock(&g_evt_mtx);
        return 0;
    }
    twd_win_event_t ev = g_evt_ring[g_evt_head];
    g_evt_head = (g_evt_head + 1u) % TWD_EVT_RING_CAP;
    twd_mutex_unlock(&g_evt_mtx);
    if (ev.kind == TWD_EVT_QUIT) {
        g_quit_flag = 1;
        g_quit_code = (int)ev.param2;
    }
    if (out_evt) *out_evt = ev;
    return 1;
}

void twd_post_quit(int exit_code) {
    twd_win_event_t ev;
    ev.kind = TWD_EVT_QUIT;
    ev.window_id = 0;
    ev.param1 = 0;
    ev.param2 = (int64_t)exit_code;
    g_quit_flag = 1;
    g_quit_code = exit_code;
    twd_window_push_event(&ev);
}

int twd_quit_requested(int *out_code) {
    if (out_code) *out_code = g_quit_code;
    return g_quit_flag;
}

void twd_clear_quit(void) {
    g_quit_flag = 0;
    g_quit_code = 0;
}

typedef struct twd_thread_ctx {
    twd_thread_fn fn;
    void *arg;
    int rc;
} twd_thread_ctx_t;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct win_event_wrap {
    HANDLE h;
} win_event_wrap_t;

_Static_assert(sizeof(CRITICAL_SECTION) <= sizeof(((twd_mutex_t *)0)->bytes),
               "twd_mutex_t storage too small for CRITICAL_SECTION");
_Static_assert(sizeof(win_event_wrap_t) <= sizeof(((twd_event_t *)0)->bytes),
               "twd_event_t storage too small");

uint64_t twd_qpc_now(void) {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return (uint64_t)v.QuadPart;
}

uint64_t twd_qpc_freq(void) {
    LARGE_INTEGER f;
    return QueryPerformanceFrequency(&f) ? (uint64_t)f.QuadPart : 10000000ULL;
}

uint32_t twd_ticks_ms(void) {
    return (uint32_t)GetTickCount();
}

void twd_sleep_ms(uint32_t ms) {
    Sleep(ms);
}

void twd_thread_yield(void) {
    SwitchToThread();
}

uint32_t twd_pid(void) {
    return (uint32_t)GetCurrentProcessId();
}

uint32_t twd_tid(void) {
    return (uint32_t)GetCurrentThreadId();
}

uint32_t twd_last_error(void) {
    return (uint32_t)GetLastError();
}

void twd_set_last_error(uint32_t err) {
    SetLastError((DWORD)err);
}

void twd_debug_output(const char *msg) {
    if (msg) OutputDebugStringA(msg);
}

void *twd_vm_alloc(size_t size, int exec) {
    DWORD prot = exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    return VirtualAlloc(NULL, size ? size : 4096, MEM_COMMIT | MEM_RESERVE, prot);
}

int twd_vm_protect(void *ptr, size_t size, int read_write, int exec) {
    DWORD prot = exec ? (read_write ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                      : (read_write ? PAGE_READWRITE : PAGE_READONLY);
    DWORD old = 0;
    return VirtualProtect(ptr, size, prot, &old) ? 0 : -1;
}

void twd_vm_free(void *ptr, size_t size) {
    (void)size;
    if (ptr) VirtualFree(ptr, 0, MEM_RELEASE);
}

int twd_mutex_init(twd_mutex_t *m) {
    if (!m) return -1;
    return InitializeCriticalSectionAndSpinCount((CRITICAL_SECTION *)(void *)m->bytes, 4000) ? 0 : -1;
}

void twd_mutex_destroy(twd_mutex_t *m) {
    if (m) DeleteCriticalSection((CRITICAL_SECTION *)(void *)m->bytes);
}

void twd_mutex_lock(twd_mutex_t *m) {
    if (m) EnterCriticalSection((CRITICAL_SECTION *)(void *)m->bytes);
}

void twd_mutex_unlock(twd_mutex_t *m) {
    if (m) LeaveCriticalSection((CRITICAL_SECTION *)(void *)m->bytes);
}

int twd_event_init(twd_event_t *ev, int manual_reset, int initial_state) {
    if (!ev) return -1;
    HANDLE h = CreateEventW(NULL, manual_reset != 0, initial_state != 0, NULL);
    if (!h) return -1;
    memcpy(ev->bytes, &h, sizeof(h));
    return 0;
}

void twd_event_destroy(twd_event_t *ev) {
    if (!ev) return;
    HANDLE h = NULL;
    memcpy(&h, ev->bytes, sizeof(h));
    if (h) {
        CloseHandle(h);
        h = NULL;
        memcpy(ev->bytes, &h, sizeof(h));
    }
}

int twd_event_set(twd_event_t *ev) {
    if (!ev) return -1;
    HANDLE h = NULL;
    memcpy(&h, ev->bytes, sizeof(h));
    return SetEvent(h) ? 0 : -1;
}

int twd_event_reset(twd_event_t *ev) {
    if (!ev) return -1;
    HANDLE h = NULL;
    memcpy(&h, ev->bytes, sizeof(h));
    return ResetEvent(h) ? 0 : -1;
}

int twd_event_wait(twd_event_t *ev, uint32_t timeout_ms) {
    if (!ev) return TWD_WAIT_ERROR;
    HANDLE h = NULL;
    memcpy(&h, ev->bytes, sizeof(h));
    DWORD rc = WaitForSingleObject(h, (timeout_ms == TWD_WAIT_INFINITE) ? INFINITE : (DWORD)timeout_ms);
    if (rc == WAIT_OBJECT_0) return TWD_WAIT_OK;
    if (rc == WAIT_TIMEOUT) return TWD_WAIT_TIMEOUT;
    return TWD_WAIT_ERROR;
}

uint32_t twd_tls_alloc(void) {
    DWORD idx = TlsAlloc();
    return (idx == TLS_OUT_OF_INDEXES) ? TWD_TLS_INVALID : (uint32_t)idx;
}

int twd_tls_free(uint32_t key) {
    return (key != TWD_TLS_INVALID && TlsFree((DWORD)key)) ? 0 : -1;
}

void *twd_tls_get(uint32_t key) {
    return (key == TWD_TLS_INVALID) ? NULL : TlsGetValue((DWORD)key);
}

int twd_tls_set(uint32_t key, void *value) {
    return (key != TWD_TLS_INVALID && TlsSetValue((DWORD)key, value)) ? 0 : -1;
}

int twd_file_open(const char *path, uint32_t flags, twd_file_t *out) {
    if (!path || !out) return -1;
    DWORD access = 0;
    if (flags & TWD_FILE_OPEN_READ) access |= GENERIC_READ;
    if (flags & TWD_FILE_OPEN_WRITE) access |= GENERIC_WRITE;
    DWORD disp = (flags & TWD_FILE_CREATE) ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        out->handle = -1;
        return -1;
    }
    out->handle = (intptr_t)h;
    return 0;
}

int64_t twd_file_read(twd_file_t *f, void *buf, size_t len) {
    if (!f || f->handle == -1) return -1;
    DWORD got = 0;
    if (!ReadFile((HANDLE)f->handle, buf, (DWORD)len, &got, NULL)) return -1;
    return (int64_t)got;
}

int64_t twd_file_write(twd_file_t *f, const void *buf, size_t len) {
    if (!f || f->handle == -1) return -1;
    DWORD put = 0;
    if (!WriteFile((HANDLE)f->handle, buf, (DWORD)len, &put, NULL)) return -1;
    return (int64_t)put;
}

int64_t twd_file_seek(twd_file_t *f, int64_t offset, int whence) {
    if (!f || f->handle == -1) return -1;
    DWORD mm = (whence == TWD_SEEK_CUR) ? FILE_CURRENT
             : (whence == TWD_SEEK_END) ? FILE_END : FILE_BEGIN;
    LARGE_INTEGER dist, pos;
    dist.QuadPart = offset;
    if (!SetFilePointerEx((HANDLE)f->handle, dist, &pos, mm)) return -1;
    return (int64_t)pos.QuadPart;
}

int twd_file_flush(twd_file_t *f) {
    if (!f || f->handle == -1) return -1;
    return FlushFileBuffers((HANDLE)f->handle) ? 0 : -1;
}

int twd_file_close(twd_file_t *f) {
    if (!f || f->handle == -1) return -1;
    BOOL ok = CloseHandle((HANDLE)f->handle);
    f->handle = -1;
    return ok ? 0 : -1;
}

int twd_file_delete(const char *path) {
    return (path && DeleteFileA(path)) ? 0 : -1;
}

int twd_mkdir(const char *path) {
    if (!path) return -1;
    if (CreateDirectoryA(path, NULL)) return 0;
    return (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
}

int twd_rmdir(const char *path) {
    return (path && RemoveDirectoryA(path)) ? 0 : -1;
}

int twd_exe_path(char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)cap);
    if (n == 0 || n >= cap) return -1;
    return (int)n;
}

int twd_sem_init(twd_sem_t *s, uint32_t initial, uint32_t max_count) {
    if (!s || max_count == 0) return -1;
    HANDLE h = CreateSemaphoreW(NULL, (LONG)initial, (LONG)max_count, NULL);
    if (!h) return -1;
    memcpy(s->bytes, &h, sizeof(h));
    return 0;
}

void twd_sem_destroy(twd_sem_t *s) {
    if (!s) return;
    HANDLE h = NULL;
    memcpy(&h, s->bytes, sizeof(h));
    if (h) {
        CloseHandle(h);
        h = NULL;
        memcpy(s->bytes, &h, sizeof(h));
    }
}

int twd_sem_post(twd_sem_t *s, uint32_t count) {
    if (!s) return -1;
    HANDLE h = NULL;
    memcpy(&h, s->bytes, sizeof(h));
    return ReleaseSemaphore(h, (LONG)count, NULL) ? 0 : -1;
}

int twd_sem_wait(twd_sem_t *s, uint32_t timeout_ms) {
    if (!s) return TWD_WAIT_ERROR;
    HANDLE h = NULL;
    memcpy(&h, s->bytes, sizeof(h));
    DWORD rc = WaitForSingleObject(h, (timeout_ms == TWD_WAIT_INFINITE) ? INFINITE : (DWORD)timeout_ms);
    if (rc == WAIT_OBJECT_0) return TWD_WAIT_OK;
    if (rc == WAIT_TIMEOUT) return TWD_WAIT_TIMEOUT;
    return TWD_WAIT_ERROR;
}

static DWORD WINAPI win_thread_tramp(LPVOID param) {
    twd_thread_ctx_t *ctx = (twd_thread_ctx_t *)param;
    ctx->rc = ctx->fn(ctx->arg);
    return 0;
}

int twd_thread_create(twd_thread_t *th, twd_thread_fn fn, void *arg) {
    if (!th || !fn) return -1;
    twd_thread_ctx_t *ctx = (twd_thread_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    HANDLE h = CreateThread(NULL, 0, win_thread_tramp, ctx, 0, NULL);
    if (!h) {
        free(ctx);
        return -1;
    }
    th->handle = (uintptr_t)h;
    th->ctx = ctx;
    return 0;
}

int twd_thread_join(twd_thread_t *th, int *out_rc) {
    if (!th || !th->handle) return -1;
    WaitForSingleObject((HANDLE)th->handle, INFINITE);
    CloseHandle((HANDLE)th->handle);
    twd_thread_ctx_t *ctx = (twd_thread_ctx_t *)th->ctx;
    if (out_rc && ctx) *out_rc = ctx->rc;
    free(ctx);
    th->handle = 0;
    th->ctx = NULL;
    return 0;
}

void *twd_dynlib_open(const char *path_or_null) {
    if (!path_or_null) return (void *)GetModuleHandleW(NULL);
    return (void *)LoadLibraryA(path_or_null);
}

void *twd_dynlib_sym(void *lib, const char *name) {
    if (!lib || !name) return NULL;
    return (void *)(uintptr_t)GetProcAddress((HMODULE)lib, name);
}

void twd_dynlib_close(void *lib) {
    if (!lib || lib == (void *)GetModuleHandleW(NULL)) return;
    FreeLibrary((HMODULE)lib);
}

int twd_getcwd(char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    DWORD n = GetCurrentDirectoryA((DWORD)cap, buf);
    return (n > 0 && n < cap) ? (int)n : -1;
}

int twd_chdir(const char *path) {
    return (path && SetCurrentDirectoryA(path)) ? 0 : -1;
}

int twd_getenv(const char *name, char *buf, size_t cap) {
    if (!name || !buf || cap == 0) return -1;
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)cap);
    if (n == 0 || n >= cap) return -1;
    return (int)n;
}

int twd_setenv(const char *name, const char *val) {
    if (!name) return -1;
    return SetEnvironmentVariableA(name, val) ? 0 : -1;
}

int twd_file_exists(const char *path) {
    if (!path) return 0;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
}

int twd_path_is_dir(const char *path) {
    if (!path) return 0;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) ? 1 : 0;
}

int twd_file_rename(const char *src, const char *dst) {
    if (!src || !dst) return -1;
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

int twd_file_truncate(twd_file_t *f, int64_t size) {
    if (!f || f->handle == -1 || size < 0) return -1;
    if (twd_file_seek(f, size, TWD_SEEK_SET) < 0) return -1;
    return SetEndOfFile((HANDLE)f->handle) ? 0 : -1;
}

int twd_dir_open(const char *path, twd_dir_t *out) {
    if (!path || !out) return -1;
    char pat[300];
    size_t n = strlen(path);
    if (n + 3 >= sizeof(pat)) return -1;
    memcpy(pat, path, n);
    if (n > 0 && pat[n - 1] != '/' && pat[n - 1] != '\\') pat[n++] = '/';
    pat[n++] = '*';
    pat[n] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        out->handle = 0;
        out->first_pending = 0;
        return -1;
    }
    out->handle = (uintptr_t)h;
    out->first_pending = 1;
    size_t fn = strlen(fd.cFileName);
    if (fn >= sizeof(out->pending_name)) fn = sizeof(out->pending_name) - 1;
    memcpy(out->pending_name, fd.cFileName, fn);
    out->pending_name[fn] = '\0';
    return 0;
}

int twd_dir_next(twd_dir_t *d, twd_dir_entry_t *out) {
    if (!d || !d->handle || !out) return -1;
    WIN32_FIND_DATAA fd;
    for (;;) {
        if (d->first_pending) {
            d->first_pending = 0;
            size_t fn = strlen(d->pending_name);
            memcpy(out->name, d->pending_name, fn + 1);
            out->is_dir = twd_path_is_dir(out->name);
            out->size = 0;
        } else {
            if (!FindNextFileA((HANDLE)d->handle, &fd)) return 0;
            size_t fn = strlen(fd.cFileName);
            if (fn >= sizeof(out->name)) fn = sizeof(out->name) - 1;
            memcpy(out->name, fd.cFileName, fn);
            out->name[fn] = '\0';
            out->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? 1 : 0;
            out->size = ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
        }
        if (strcmp(out->name, ".") == 0 || strcmp(out->name, "..") == 0) continue;
        return 1;
    }
}

void twd_dir_close(twd_dir_t *d) {
    if (d && d->handle) {
        FindClose((HANDLE)d->handle);
        d->handle = 0;
        d->first_pending = 0;
    }
}

uint32_t twd_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (uint32_t)si.dwNumberOfProcessors : 1u;
}

uint32_t twd_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? (uint32_t)si.dwPageSize : 4096u;
}

uint64_t twd_unix_time_ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    const uint64_t epoch_diff_100ns = 116444736000000000ULL;
    return (t > epoch_diff_100ns) ? (t - epoch_diff_100ns) * 100ULL : 0ULL;
}

void twd_icache_flush(void *addr, size_t size) {
    if (addr && size) FlushInstructionCache(GetCurrentProcess(), addr, size);
}

int twd_timer_begin_period(uint32_t ms) {
    typedef UINT (WINAPI *mm_fn_t)(UINT);
    void *mod = twd_dynlib_open("winmm.dll");
    mm_fn_t fn = mod ? (mm_fn_t)(uintptr_t)twd_dynlib_sym(mod, "timeBeginPeriod") : NULL;
    return (fn && fn((UINT)ms) == 0) ? 0 : -1;
}

int twd_timer_end_period(uint32_t ms) {
    typedef UINT (WINAPI *mm_fn_t)(UINT);
    void *mod = twd_dynlib_open("winmm.dll");
    mm_fn_t fn = mod ? (mm_fn_t)(uintptr_t)twd_dynlib_sym(mod, "timeEndPeriod") : NULL;
    return (fn && fn((UINT)ms) == 0) ? 0 : -1;
}

int twd_system_metric(int metric_id) {
    typedef int (WINAPI *sm_fn_t)(int);
    void *mod = twd_dynlib_open("user32.dll");
    sm_fn_t fn = mod ? (sm_fn_t)(uintptr_t)twd_dynlib_sym(mod, "GetSystemMetrics") : NULL;
    if (fn) return fn(metric_id);
    if (metric_id == TWD_METRIC_SCREEN_W) return 1920;
    if (metric_id == TWD_METRIC_SCREEN_H) return 1080;
    if (metric_id == TWD_METRIC_MOUSE_PRESENT) return 1;
    return 0;
}

#else
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct posix_event {
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    int manual_reset;
    int signaled;
} posix_event_t;

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(((twd_mutex_t *)0)->bytes),
               "twd_mutex_t storage too small for pthread_mutex_t");
_Static_assert(sizeof(posix_event_t) <= sizeof(((twd_event_t *)0)->bytes),
               "twd_event_t storage too small for posix_event_t");
_Static_assert(sizeof(pthread_key_t) <= sizeof(uint32_t),
               "uint32_t too small for pthread_key_t");

static _Thread_local uint32_t g_last_err = 0;

uint64_t twd_qpc_now(void) { return twd_time_now_ns(); }
uint64_t twd_qpc_freq(void) { return 1000000000ULL; }
uint32_t twd_ticks_ms(void) { return (uint32_t)(twd_time_now_ns() / 1000000ULL); }

void twd_sleep_ms(uint32_t ms) {
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&req, NULL);
}

void twd_thread_yield(void) { sched_yield(); }
uint32_t twd_pid(void) { return (uint32_t)getpid(); }

uint32_t twd_tid(void) {
#ifdef __ANDROID__
    return (uint32_t)gettid();
#elif defined(SYS_gettid)
    return (uint32_t)syscall(SYS_gettid);
#else
    return (uint32_t)pthread_self();
#endif
}

uint32_t twd_last_error(void) { return g_last_err; }
void twd_set_last_error(uint32_t err) { g_last_err = err; }

void twd_debug_output(const char *msg) {
    if (msg) twd_log("dbg", "%s", msg);
}

void *twd_vm_alloc(size_t size, int exec) {
    int prot = PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0);
    size_t len = size ? size : 4096;
    void *p = mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

int twd_vm_protect(void *ptr, size_t size, int read_write, int exec) {
    int prot = PROT_READ | (read_write ? PROT_WRITE : 0) | (exec ? PROT_EXEC : 0);
    return mprotect(ptr, size, prot) == 0 ? 0 : -1;
}

void twd_vm_free(void *ptr, size_t size) {
    if (ptr) munmap(ptr, size ? size : 4096);
}

int twd_mutex_init(twd_mutex_t *m) {
    if (!m) return -1;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) return -1;
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int rc = pthread_mutex_init((pthread_mutex_t *)(void *)m->bytes, &attr);
    pthread_mutexattr_destroy(&attr);
    return rc == 0 ? 0 : -1;
}

void twd_mutex_destroy(twd_mutex_t *m) {
    if (m) pthread_mutex_destroy((pthread_mutex_t *)(void *)m->bytes);
}

void twd_mutex_lock(twd_mutex_t *m) {
    if (m) pthread_mutex_lock((pthread_mutex_t *)(void *)m->bytes);
}

void twd_mutex_unlock(twd_mutex_t *m) {
    if (m) pthread_mutex_unlock((pthread_mutex_t *)(void *)m->bytes);
}

int twd_event_init(twd_event_t *ev, int manual_reset, int initial_state) {
    if (!ev) return -1;
    posix_event_t *pe = (posix_event_t *)(void *)ev->bytes;
    if (pthread_mutex_init(&pe->mtx, NULL) != 0) return -1;
    pthread_condattr_t cattr;
    if (pthread_condattr_init(&cattr) != 0) {
        pthread_mutex_destroy(&pe->mtx);
        return -1;
    }
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    int rc = pthread_cond_init(&pe->cv, &cattr);
    pthread_condattr_destroy(&cattr);
    if (rc != 0) {
        pthread_mutex_destroy(&pe->mtx);
        return -1;
    }
    pe->manual_reset = manual_reset != 0;
    pe->signaled = initial_state != 0;
    return 0;
}

void twd_event_destroy(twd_event_t *ev) {
    if (!ev) return;
    posix_event_t *pe = (posix_event_t *)(void *)ev->bytes;
    pthread_cond_destroy(&pe->cv);
    pthread_mutex_destroy(&pe->mtx);
}

int twd_event_set(twd_event_t *ev) {
    if (!ev) return -1;
    posix_event_t *pe = (posix_event_t *)(void *)ev->bytes;
    pthread_mutex_lock(&pe->mtx);
    pe->signaled = 1;
    if (pe->manual_reset) pthread_cond_broadcast(&pe->cv);
    else pthread_cond_signal(&pe->cv);
    pthread_mutex_unlock(&pe->mtx);
    return 0;
}

int twd_event_reset(twd_event_t *ev) {
    if (!ev) return -1;
    posix_event_t *pe = (posix_event_t *)(void *)ev->bytes;
    pthread_mutex_lock(&pe->mtx);
    pe->signaled = 0;
    pthread_mutex_unlock(&pe->mtx);
    return 0;
}

int twd_event_wait(twd_event_t *ev, uint32_t timeout_ms) {
    if (!ev) return TWD_WAIT_ERROR;
    posix_event_t *pe = (posix_event_t *)(void *)ev->bytes;
    pthread_mutex_lock(&pe->mtx);
    int res = TWD_WAIT_OK;
    if (!pe->signaled) {
        if (timeout_ms == 0) {
            res = TWD_WAIT_TIMEOUT;
        } else if (timeout_ms == TWD_WAIT_INFINITE) {
            while (!pe->signaled) {
                pthread_cond_wait(&pe->cv, &pe->mtx);
            }
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += (time_t)(timeout_ms / 1000u);
            ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            while (!pe->signaled) {
                int rc = pthread_cond_timedwait(&pe->cv, &pe->mtx, &ts);
                if (rc == ETIMEDOUT) {
                    res = pe->signaled ? TWD_WAIT_OK : TWD_WAIT_TIMEOUT;
                    break;
                }
            }
        }
    }
    if (res == TWD_WAIT_OK && !pe->manual_reset) {
        pe->signaled = 0;
    }
    pthread_mutex_unlock(&pe->mtx);
    return res;
}

uint32_t twd_tls_alloc(void) {
    pthread_key_t k = 0;
    if (pthread_key_create(&k, NULL) != 0) return TWD_TLS_INVALID;
    return (uint32_t)k;
}

int twd_tls_free(uint32_t key) {
    if (key == TWD_TLS_INVALID) return -1;
    return pthread_key_delete((pthread_key_t)key) == 0 ? 0 : -1;
}

void *twd_tls_get(uint32_t key) {
    return (key == TWD_TLS_INVALID) ? NULL : pthread_getspecific((pthread_key_t)key);
}

int twd_tls_set(uint32_t key, void *value) {
    if (key == TWD_TLS_INVALID) return -1;
    return pthread_setspecific((pthread_key_t)key, value) == 0 ? 0 : -1;
}

int twd_file_open(const char *path, uint32_t flags, twd_file_t *out) {
    if (!path || !out) return -1;
    int oflags = O_RDONLY;
    if ((flags & TWD_FILE_OPEN_READ) && (flags & TWD_FILE_OPEN_WRITE)) oflags = O_RDWR;
    else if (flags & TWD_FILE_OPEN_WRITE) oflags = O_WRONLY;
    if (flags & TWD_FILE_CREATE) oflags |= O_CREAT;
    int fd = open(path, oflags, 0644);
    if (fd < 0) {
        out->handle = -1;
        return -1;
    }
    out->handle = (intptr_t)fd;
    return 0;
}

int64_t twd_file_read(twd_file_t *f, void *buf, size_t len) {
    if (!f || f->handle < 0) return -1;
    return (int64_t)read((int)f->handle, buf, len);
}

int64_t twd_file_write(twd_file_t *f, const void *buf, size_t len) {
    if (!f || f->handle < 0) return -1;
    return (int64_t)write((int)f->handle, buf, len);
}

int64_t twd_file_seek(twd_file_t *f, int64_t offset, int whence) {
    if (!f || f->handle < 0) return -1;
    int w = (whence == TWD_SEEK_CUR) ? SEEK_CUR : (whence == TWD_SEEK_END) ? SEEK_END : SEEK_SET;
    return (int64_t)lseek((int)f->handle, (off_t)offset, w);
}

int twd_file_flush(twd_file_t *f) {
    if (!f || f->handle < 0) return -1;
    return fsync((int)f->handle) == 0 ? 0 : -1;
}

int twd_file_close(twd_file_t *f) {
    if (!f || f->handle < 0) return -1;
    int rc = close((int)f->handle);
    f->handle = -1;
    return rc == 0 ? 0 : -1;
}

int twd_file_delete(const char *path) {
    return (path && unlink(path) == 0) ? 0 : -1;
}

int twd_mkdir(const char *path) {
    if (!path) return -1;
    if (mkdir(path, 0755) == 0) return 0;
    return (errno == EEXIST) ? 0 : -1;
}

int twd_rmdir(const char *path) {
    return (path && rmdir(path) == 0) ? 0 : -1;
}

int twd_exe_path(char *buf, size_t cap) {
    if (!buf || cap < 2) return -1;
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0 || (size_t)n >= cap) return -1;
    buf[n] = '\0';
    return (int)n;
}

typedef struct posix_sem {
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    uint32_t count;
    uint32_t max_count;
} posix_sem_t;

_Static_assert(sizeof(posix_sem_t) <= sizeof(((twd_sem_t *)0)->bytes),
               "twd_sem_t storage too small for posix_sem_t");
_Static_assert(sizeof(pthread_t) <= sizeof(uintptr_t),
               "uintptr_t too small for pthread_t");

int twd_sem_init(twd_sem_t *s, uint32_t initial, uint32_t max_count) {
    if (!s || max_count == 0 || initial > max_count) return -1;
    posix_sem_t *ps = (posix_sem_t *)(void *)s->bytes;
    if (pthread_mutex_init(&ps->mtx, NULL) != 0) return -1;
    pthread_condattr_t cattr;
    if (pthread_condattr_init(&cattr) != 0) {
        pthread_mutex_destroy(&ps->mtx);
        return -1;
    }
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    int rc = pthread_cond_init(&ps->cv, &cattr);
    pthread_condattr_destroy(&cattr);
    if (rc != 0) {
        pthread_mutex_destroy(&ps->mtx);
        return -1;
    }
    ps->count = initial;
    ps->max_count = max_count;
    return 0;
}

void twd_sem_destroy(twd_sem_t *s) {
    if (!s) return;
    posix_sem_t *ps = (posix_sem_t *)(void *)s->bytes;
    pthread_cond_destroy(&ps->cv);
    pthread_mutex_destroy(&ps->mtx);
}

int twd_sem_post(twd_sem_t *s, uint32_t count) {
    if (!s || count == 0) return -1;
    posix_sem_t *ps = (posix_sem_t *)(void *)s->bytes;
    pthread_mutex_lock(&ps->mtx);
    if (ps->count > ps->max_count - count) {
        pthread_mutex_unlock(&ps->mtx);
        return -1;
    }
    ps->count += count;
    pthread_cond_broadcast(&ps->cv);
    pthread_mutex_unlock(&ps->mtx);
    return 0;
}

int twd_sem_wait(twd_sem_t *s, uint32_t timeout_ms) {
    if (!s) return TWD_WAIT_ERROR;
    posix_sem_t *ps = (posix_sem_t *)(void *)s->bytes;
    pthread_mutex_lock(&ps->mtx);
    int res = TWD_WAIT_OK;
    if (ps->count == 0) {
        if (timeout_ms == 0) {
            res = TWD_WAIT_TIMEOUT;
        } else if (timeout_ms == TWD_WAIT_INFINITE) {
            while (ps->count == 0) pthread_cond_wait(&ps->cv, &ps->mtx);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_sec += (time_t)(timeout_ms / 1000u);
            ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            while (ps->count == 0) {
                if (pthread_cond_timedwait(&ps->cv, &ps->mtx, &ts) == ETIMEDOUT) {
                    res = (ps->count > 0) ? TWD_WAIT_OK : TWD_WAIT_TIMEOUT;
                    break;
                }
            }
        }
    }
    if (res == TWD_WAIT_OK) ps->count--;
    pthread_mutex_unlock(&ps->mtx);
    return res;
}

static void *posix_thread_tramp(void *param) {
    twd_thread_ctx_t *ctx = (twd_thread_ctx_t *)param;
    ctx->rc = ctx->fn(ctx->arg);
    return NULL;
}

int twd_thread_create(twd_thread_t *th, twd_thread_fn fn, void *arg) {
    if (!th || !fn) return -1;
    twd_thread_ctx_t *ctx = (twd_thread_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fn = fn;
    ctx->arg = arg;
    pthread_t pt;
    if (pthread_create(&pt, NULL, posix_thread_tramp, ctx) != 0) {
        free(ctx);
        return -1;
    }
    th->handle = (uintptr_t)pt;
    th->ctx = ctx;
    return 0;
}

int twd_thread_join(twd_thread_t *th, int *out_rc) {
    if (!th || !th->handle) return -1;
    pthread_join((pthread_t)th->handle, NULL);
    twd_thread_ctx_t *ctx = (twd_thread_ctx_t *)th->ctx;
    if (out_rc && ctx) *out_rc = ctx->rc;
    free(ctx);
    th->handle = 0;
    th->ctx = NULL;
    return 0;
}

void *twd_dynlib_open(const char *path_or_null) {
    return dlopen(path_or_null, RTLD_NOW);
}

void *twd_dynlib_sym(void *lib, const char *name) {
    return (lib && name) ? dlsym(lib, name) : NULL;
}

void twd_dynlib_close(void *lib) {
    if (lib) dlclose(lib);
}

int twd_getcwd(char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
    return getcwd(buf, cap) ? (int)strlen(buf) : -1;
}

int twd_chdir(const char *path) {
    return (path && chdir(path) == 0) ? 0 : -1;
}

int twd_getenv(const char *name, char *buf, size_t cap) {
    if (!name || !buf || cap == 0) return -1;
    const char *v = getenv(name);
    if (!v) return -1;
    size_t n = strlen(v);
    if (n >= cap) return -1;
    memcpy(buf, v, n + 1);
    return (int)n;
}

int twd_setenv(const char *name, const char *val) {
    if (!name) return -1;
    return val ? setenv(name, val, 1) : unsetenv(name);
}

int twd_file_exists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0) ? 1 : 0;
}

int twd_path_is_dir(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

int twd_file_rename(const char *src, const char *dst) {
    return (src && dst && rename(src, dst) == 0) ? 0 : -1;
}

int twd_file_truncate(twd_file_t *f, int64_t size) {
    if (!f || f->handle < 0 || size < 0) return -1;
    return ftruncate((int)f->handle, (off_t)size) == 0 ? 0 : -1;
}

int twd_dir_open(const char *path, twd_dir_t *out) {
    if (!path || !out) return -1;
    DIR *dp = opendir(path);
    if (!dp) {
        out->handle = 0;
        return -1;
    }
    out->handle = (uintptr_t)dp;
    out->first_pending = 0;
    return 0;
}

int twd_dir_next(twd_dir_t *d, twd_dir_entry_t *out) {
    if (!d || !d->handle || !out) return -1;
    DIR *dp = (DIR *)d->handle;
    for (;;) {
        struct dirent *ent = readdir(dp);
        if (!ent) return 0;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        size_t n = strlen(ent->d_name);
        if (n >= sizeof(out->name)) n = sizeof(out->name) - 1;
        memcpy(out->name, ent->d_name, n);
        out->name[n] = '\0';
        out->is_dir = (ent->d_type == DT_DIR) ? 1 : 0;
        out->size = 0;
        return 1;
    }
}

void twd_dir_close(twd_dir_t *d) {
    if (d && d->handle) {
        closedir((DIR *)d->handle);
        d->handle = 0;
    }
}

uint32_t twd_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 1u;
}

uint32_t twd_page_size(void) {
    long n = sysconf(_SC_PAGESIZE);
    return (n > 0) ? (uint32_t)n : 4096u;
}

uint64_t twd_unix_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void twd_icache_flush(void *addr, size_t size) {
    if (addr && size) {
#if defined(__GNUC__) || defined(__clang__)
        __builtin___clear_cache((char *)addr, (char *)addr + size);
#else
        (void)addr;
        (void)size;
#endif
    }
}

int twd_timer_begin_period(uint32_t ms) {
    (void)ms;
    return 0;
}

int twd_timer_end_period(uint32_t ms) {
    (void)ms;
    return 0;
}

int twd_system_metric(int metric_id) {
    if (metric_id == TWD_METRIC_SCREEN_W) return 1920;
    if (metric_id == TWD_METRIC_SCREEN_H) return 1080;
    if (metric_id == TWD_METRIC_MOUSE_PRESENT) return 0;
    return 0;
}
#endif
