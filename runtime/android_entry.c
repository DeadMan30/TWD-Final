/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stddef.h>
#include <stdint.h>

uint64_t twd_android_run_frame_loop(
    twd_window_t *win, twd_gfx_device_t *gfx, const twd_app_callbacks_t *cb) {
    if (!cb) return 0ULL;
    if (cb->on_init && cb->on_init(cb->user) != 0) {
        return 0ULL;
    }
    uint64_t last_ns = twd_time_now_ns();
    uint64_t frames = 0ULL;
    for (;;) {
        twd_win_event_t ev;
        while (twd_window_poll_event(&ev) > 0) {
            if (ev.kind == TWD_EVT_QUIT || ev.kind == TWD_EVT_CLOSE) {
                if (ev.kind == TWD_EVT_CLOSE) twd_post_quit(0);
            }
        }
        int qcode = 0;
        if (twd_quit_requested(&qcode)) {
            break;
        }
        if (cb->max_frames > 0u && frames >= (uint64_t)cb->max_frames) {
            break;
        }
        uint64_t now_ns = twd_time_now_ns();
        uint64_t dt_ns = (now_ns >= last_ns) ? (now_ns - last_ns) : 0ULL;
        if (dt_ns > 100000000ULL) dt_ns = 100000000ULL;
        last_ns = now_ns;

        if (!twd_android_has_focus()) {
            twd_sleep_ms(1u);
            frames++;
            continue;
        }
        if (cb->on_tick) cb->on_tick(cb->user, dt_ns);
        if (gfx) twd_gfx_begin_frame(gfx);
        if (cb->on_render) cb->on_render(cb->user, gfx);
        if (gfx) twd_gfx_present(gfx, 1u);
        (void)win;
        frames++;
    }
    if (cb->on_shutdown) cb->on_shutdown(cb->user);
    return frames;
}

#ifdef __ANDROID__
#include <android/input.h>
#include <android/native_activity.h>

static void on_native_destroy(ANativeActivity *activity) {
    (void)activity;
    twd_post_quit(0);
    twd_platform_shutdown();
}

static void on_window_focus_changed(ANativeActivity *activity, int has_focus) {
    (void)activity;
    twd_android_on_focus(has_focus);
}

static void on_native_window_created(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    twd_android_set_native_window(window);
}

static void on_native_window_resized(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    if (window) {
        int32_t w = ANativeWindow_getWidth(window);
        int32_t h = ANativeWindow_getHeight(window);
        if (w > 0 && h > 0) {
            twd_android_on_resize(NULL, NULL, (uint32_t)w, (uint32_t)h);
        }
    }
}

static void on_native_window_destroyed(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    (void)window;
    twd_android_set_native_window(NULL);
}

JNIEXPORT void ANativeActivity_onCreate(
    ANativeActivity *activity, void *saved_state, size_t saved_state_size) {
    (void)saved_state;
    (void)saved_state_size;
    if (!activity) return;
    activity->callbacks->onDestroy = on_native_destroy;
    activity->callbacks->onWindowFocusChanged = on_window_focus_changed;
    activity->callbacks->onNativeWindowCreated = on_native_window_created;
    activity->callbacks->onNativeWindowResized = on_native_window_resized;
    activity->callbacks->onNativeWindowDestroyed = on_native_window_destroyed;
    twd_android_set_asset_manager(activity->assetManager);
    twd_android_set_data_dirs(activity->internalDataPath, activity->externalDataPath);
    twd_platform_init();
}
#endif
