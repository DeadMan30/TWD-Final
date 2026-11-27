/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdio.h>

int main(void) {
    twd_game_register_hooks();
    if (twd_game_hook_count() != 44u) {
        fprintf(stderr, "test_engine_pc_stubs: unexpected hook count %u\n", twd_game_hook_count());
        return 1;
    }
    twd_engine_telemetry_cohort_init(NULL);
    twd_engine_rail_sdk_init();
    if (twd_engine_pc_shortcut_init(NULL) != 0u) {
        fprintf(stderr, "test_engine_pc_stubs: shortcut stub returned non-zero\n");
        return 1;
    }
    if (twd_engine_breakpad_http_upload() != 0u) {
        fprintf(stderr, "test_engine_pc_stubs: breakpad stub returned non-zero\n");
        return 1;
    }
    printf("test_engine_pc_stubs: ok\n");
    return 0;
}
