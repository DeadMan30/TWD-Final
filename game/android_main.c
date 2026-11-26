/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"
#include "twd_runtime.h"

#ifdef __ANDROID__
#include <android/native_activity.h>
#include <string.h>

static twd_thread_t g_boot_thread;
static int g_boot_thread_started = 0;

static int android_boot_worker(void *arg) {
    (void)arg;
    /* Wait briefly for ANativeActivity UI thread to attach ANativeWindow before entering loop. */
    for (uint32_t i = 0u; i < 200u; ++i) {
        int qcode = 0;
        if (twd_quit_requested(&qcode)) return 0;
        if (twd_android_get_native_window() != NULL) break;
        twd_sleep_ms(10u);
    }

    char data_dir[512];
    data_dir[0] = '\0';
    if (twd_android_get_data_dir(1, data_dir, sizeof(data_dir)) <= 0) {
        (void)twd_android_get_data_dir(0, data_dir, sizeof(data_dir));
    }

    twd_boot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.game_dir = (data_dir[0] != '\0') ? data_dir : NULL;
    cfg.preferred_backend = -1;

    twd_boot_stats_t stats;
    (void)twd_game_boot_run(&cfg, &stats);
    return 0;
}

JNIEXPORT void ANativeActivity_onCreate(
    ANativeActivity *activity, void *saved_state, size_t saved_state_size) {
    (void)saved_state;
    (void)saved_state_size;
    twd_android_attach_native_activity(activity, twd_game_register_hooks);
    if (!g_boot_thread_started) {
        if (twd_thread_create(&g_boot_thread, android_boot_worker, NULL) == 0) {
            g_boot_thread_started = 1;
        }
    }
}
#endif
