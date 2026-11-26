/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"
#include "twd_runtime.h"

#ifdef __ANDROID__
#include <android/native_activity.h>

JNIEXPORT void ANativeActivity_onCreate(
    ANativeActivity *activity, void *saved_state, size_t saved_state_size) {
    (void)saved_state;
    (void)saved_state_size;
    twd_android_attach_native_activity(activity, twd_game_register_hooks);
}
#endif
