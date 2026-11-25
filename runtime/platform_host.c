/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "twd_runtime.h"

#include <stdarg.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LARGE_INTEGER g_qpc_freq;

int twd_platform_init(void) {
    return QueryPerformanceFrequency(&g_qpc_freq) ? 0 : -1;
}

void twd_platform_shutdown(void) {
}

uint64_t twd_time_now_ns(void) {
    LARGE_INTEGER now;
    if (g_qpc_freq.QuadPart == 0) {
        QueryPerformanceFrequency(&g_qpc_freq);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000000000ULL) / (uint64_t)g_qpc_freq.QuadPart);
}
#else
#include <time.h>

int twd_platform_init(void) {
    return 0;
}

void twd_platform_shutdown(void) {
}

uint64_t twd_time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

void twd_log(const char *tag, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[%s] ", tag ? tag : "twd");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
