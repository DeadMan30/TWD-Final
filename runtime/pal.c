/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "twd_runtime.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

_Static_assert(sizeof(CRITICAL_SECTION) <= sizeof(((twd_mutex_t *)0)->opaque),
               "twd_mutex_t storage too small for CRITICAL_SECTION");

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

void twd_debug_output(const char *msg) {
    if (msg) {
        OutputDebugStringA(msg);
    }
}

int twd_mutex_init(twd_mutex_t *m) {
    if (!m) return -1;
    return InitializeCriticalSectionAndSpinCount((CRITICAL_SECTION *)(void *)m->opaque, 4000) ? 0 : -1;
}

void twd_mutex_destroy(twd_mutex_t *m) {
    if (m) DeleteCriticalSection((CRITICAL_SECTION *)(void *)m->opaque);
}

void twd_mutex_lock(twd_mutex_t *m) {
    if (m) EnterCriticalSection((CRITICAL_SECTION *)(void *)m->opaque);
}

void twd_mutex_unlock(twd_mutex_t *m) {
    if (m) LeaveCriticalSection((CRITICAL_SECTION *)(void *)m->opaque);
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

#else
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(((twd_mutex_t *)0)->opaque),
               "twd_mutex_t storage too small for pthread_mutex_t");
_Static_assert(sizeof(pthread_key_t) <= sizeof(uint32_t),
               "uint32_t too small for pthread_key_t");

uint64_t twd_qpc_now(void) {
    return twd_time_now_ns();
}

uint64_t twd_qpc_freq(void) {
    return 1000000000ULL;
}

uint32_t twd_ticks_ms(void) {
    return (uint32_t)(twd_time_now_ns() / 1000000ULL);
}

void twd_sleep_ms(uint32_t ms) {
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&req, NULL);
}

void twd_thread_yield(void) {
    sched_yield();
}

uint32_t twd_pid(void) {
    return (uint32_t)getpid();
}

uint32_t twd_tid(void) {
#ifdef __ANDROID__
    return (uint32_t)gettid();
#elif defined(SYS_gettid)
    return (uint32_t)syscall(SYS_gettid);
#else
    return (uint32_t)pthread_self();
#endif
}

void twd_debug_output(const char *msg) {
    if (msg) {
        twd_log("dbg", "%s", msg);
    }
}

int twd_mutex_init(twd_mutex_t *m) {
    if (!m) return -1;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) return -1;
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int rc = pthread_mutex_init((pthread_mutex_t *)(void *)m->opaque, &attr);
    pthread_mutexattr_destroy(&attr);
    return rc == 0 ? 0 : -1;
}

void twd_mutex_destroy(twd_mutex_t *m) {
    if (m) pthread_mutex_destroy((pthread_mutex_t *)(void *)m->opaque);
}

void twd_mutex_lock(twd_mutex_t *m) {
    if (m) pthread_mutex_lock((pthread_mutex_t *)(void *)m->opaque);
}

void twd_mutex_unlock(twd_mutex_t *m) {
    if (m) pthread_mutex_unlock((pthread_mutex_t *)(void *)m->opaque);
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
#endif
