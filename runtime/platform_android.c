/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <android/log.h>
#include <stdarg.h>
#include <time.h>

int twd_platform_init(void) {
    char buf[512];
    if (twd_android_get_data_dir(1, buf, sizeof(buf)) > 0 ||
        twd_android_get_data_dir(0, buf, sizeof(buf)) > 0) {
        twd_setenv("TWD_SAVE_DIR", buf);
    }
    twd_log("TWD-Final", "Android runtime initialized (window=%p, assets=%p)",
            twd_android_get_native_window(), twd_android_get_asset_manager());
    return 0;
}

void twd_platform_shutdown(void) {
    twd_log("TWD-Final", "Android runtime shutdown");
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
