/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <android/log.h>
#include <stdarg.h>
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

void twd_log(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, tag ? tag : "TWD-Final", fmt, ap);
    va_end(ap);
}
