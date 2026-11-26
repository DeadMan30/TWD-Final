/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"
#include "twd_runtime.h"

/*
 * Stubs for non-essential PC-only subsystems along the WinMain -> GameEngine::Loop
 * critical path. Marked `replaced` (not `verified`) because upstream telemetry,
 * launcher DRM, crash upload, and Windows shell integration are disabled on Android.
 */

/* Original: WalkingDead4.exe @ 0x1401e60c0 (1,120 bytes, 1 caller: GameEngine::Loop) */
TWD_EXPORT
void twd_engine_telemetry_cohort_init(void *ctx) {
    (void)ctx;
    (void)twd_telemetry_enabled();
}

/* Original: WalkingDead4.exe @ 0x14035b640 (1,000 bytes, 1 caller: GameEngine::Loop) */
TWD_EXPORT
void twd_engine_rail_sdk_init(void) {
    /* No-op: Tencent WeGame Rail SDK is PC-launcher specific. */
}

/* Original: WalkingDead4.exe @ 0x14057f750 (439 bytes, vtable slot) */
TWD_EXPORT
uint8_t twd_engine_pc_shortcut_init(void *ctx) {
    (void)ctx;
    return 0u;
}

/* Original: WalkingDead4.exe @ 0x140bbb400 (1,504 bytes, 1 caller: 0x140bba370) */
TWD_EXPORT
uint8_t twd_engine_breakpad_http_upload(void) {
    (void)twd_wininet_offline_status();
    return 0u;
}
