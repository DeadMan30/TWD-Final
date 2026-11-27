/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stddef.h>
#include <stdint.h>

static void pump_android_input(void);

uint64_t twd_android_run_frame_loop(
    twd_window_t *win, twd_gfx_device_t *gfx, const twd_app_callbacks_t *cb) {
    if (!cb) return 0ULL;
    if (cb->on_init && cb->on_init(cb->user) != 0) {
        return 0ULL;
    }
    uint64_t last_ns = twd_time_now_ns();
    uint64_t frames = 0ULL;
    for (;;) {
        pump_android_input();
        twd_win_event_t ev;
        while (twd_window_poll_event(&ev) > 0) {
            if (ev.kind == TWD_EVT_CLOSE) {
                twd_post_quit(0);
            } else if (ev.kind == TWD_EVT_KEY) {
                twd_input_set_key((uint32_t)ev.param1, (ev.param2 != 0) ? 1 : 0);
            } else if (ev.kind == TWD_EVT_POINTER) {
                int32_t px = (int32_t)(ev.param2 >> 32);
                int32_t py = (int32_t)(uint32_t)(ev.param2 & 0xffffffffu);
                uint32_t action = (uint32_t)(ev.param1 & 0xffffffffu);
                twd_input_set_pointer((float)px, (float)py,
                    (action == TWD_TOUCH_DOWN || action == TWD_TOUCH_MOVE) ? 1u : 0u);
            }
            if (cb->on_event) cb->on_event(cb->user, &ev);
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
#include <android/native_window.h>

static AInputQueue *g_input_queue = NULL;

static int map_android_keycode(int32_t code) {
    if (code == AKEYCODE_DPAD_UP) return 38;
    if (code == AKEYCODE_DPAD_DOWN) return 40;
    if (code == AKEYCODE_DPAD_LEFT) return 37;
    if (code == AKEYCODE_DPAD_RIGHT) return 39;
    if (code == AKEYCODE_DPAD_CENTER || code == AKEYCODE_ENTER || code == AKEYCODE_BUTTON_A) return 13;
    if (code == AKEYCODE_SPACE || code == AKEYCODE_BUTTON_X) return 32;
    if (code == AKEYCODE_BACK || code == AKEYCODE_DEL || code == AKEYCODE_ESCAPE || code == AKEYCODE_BUTTON_B) return 8;
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) return '0' + (code - AKEYCODE_0);
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) return 'A' + (code - AKEYCODE_A);
    return (int)code;
}

static void pump_android_input(void) {
    if (!g_input_queue) return;
    AInputEvent *ev = NULL;
    while (AInputQueue_getEvent(g_input_queue, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(g_input_queue, ev)) {
            continue;
        }
        int handled = 0;
        int32_t etype = AInputEvent_getType(ev);
        if (etype == AINPUT_EVENT_TYPE_KEY) {
            int32_t act = AKeyEvent_getAction(ev);
            int32_t code = AKeyEvent_getKeyCode(ev);
            if (act == AKEY_EVENT_ACTION_DOWN || act == AKEY_EVENT_ACTION_UP) {
                twd_android_on_key(map_android_keycode(code), act == AKEY_EVENT_ACTION_DOWN);
                handled = 1;
            }
        } else if (etype == AINPUT_EVENT_TYPE_MOTION) {
            int32_t src = AInputEvent_getSource(ev);
            if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
                float lx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0);
                float ly = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
                float rx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Z, 0);
                float ry = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RZ, 0);
                float lt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0);
                float rt = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0);
                twd_gamepad_state_t cur;
                uint16_t btns = (twd_input_get_gamepad_state(0u, &cur) == 0u) ? cur.buttons : 0u;
                twd_android_on_motion_axis(0u, lx, ly, rx, ry, lt, rt, btns);
                handled = 1;
            } else {
                int32_t raw_act = AMotionEvent_getAction(ev);
                int32_t action = raw_act & AMOTION_EVENT_ACTION_MASK;
                size_t ptr_idx = (size_t)((raw_act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                                          >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
                int32_t ptr_id = AMotionEvent_getPointerId(ev, ptr_idx);
                float x = AMotionEvent_getX(ev, ptr_idx);
                float y = AMotionEvent_getY(ev, ptr_idx);
                int mapped = -1;
                if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                    mapped = TWD_TOUCH_DOWN;
                } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP) {
                    mapped = TWD_TOUCH_UP;
                } else if (action == AMOTION_EVENT_ACTION_MOVE) {
                    mapped = TWD_TOUCH_MOVE;
                } else if (action == AMOTION_EVENT_ACTION_CANCEL) {
                    mapped = TWD_TOUCH_CANCEL;
                }
                if (mapped >= 0) {
                    twd_android_on_touch(mapped, ptr_id, x, y);
                    handled = 1;
                }
            }
        }
        AInputQueue_finishEvent(g_input_queue, ev, handled);
    }
}

static void on_native_destroy(ANativeActivity *activity) {
    (void)activity;
    g_input_queue = NULL;
    twd_post_quit(0);
    twd_platform_shutdown();
}

static void on_native_pause(ANativeActivity *activity) {
    (void)activity;
    twd_android_on_focus(0);
}

static void on_native_resume(ANativeActivity *activity) {
    (void)activity;
    twd_android_on_focus(1);
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

static void on_input_queue_created(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    g_input_queue = queue;
}

static void on_input_queue_destroyed(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    if (g_input_queue == queue) g_input_queue = NULL;
}

void twd_android_attach_native_activity(void *native_activity, void (*on_bind_hooks)(void)) {
    ANativeActivity *activity = (ANativeActivity *)native_activity;
    if (!activity) return;
    activity->callbacks->onDestroy = on_native_destroy;
    activity->callbacks->onPause = on_native_pause;
    activity->callbacks->onResume = on_native_resume;
    activity->callbacks->onWindowFocusChanged = on_window_focus_changed;
    activity->callbacks->onNativeWindowCreated = on_native_window_created;
    activity->callbacks->onNativeWindowResized = on_native_window_resized;
    activity->callbacks->onNativeWindowDestroyed = on_native_window_destroyed;
    activity->callbacks->onInputQueueCreated = on_input_queue_created;
    activity->callbacks->onInputQueueDestroyed = on_input_queue_destroyed;
    twd_android_set_asset_manager(activity->assetManager);
    twd_android_set_data_dirs(activity->internalDataPath, activity->externalDataPath);
    if (on_bind_hooks) on_bind_hooks();
    twd_platform_init();
}
#else
static void pump_android_input(void) {
}

void twd_android_attach_native_activity(void *native_activity, void (*on_bind_hooks)(void)) {
    (void)native_activity;
    if (on_bind_hooks) on_bind_hooks();
}
#endif
