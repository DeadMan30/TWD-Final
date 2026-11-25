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

#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
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
#endif
