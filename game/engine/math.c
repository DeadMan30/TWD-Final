/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

/*
 * Original: WalkingDead4.exe @ 0x1400359f0 (54 bytes, 71 callers)
 *
 * Initializes a 4x4 row-major float matrix to the identity matrix, writing
 * exact IEEE-754 +0.0f / 1.0f bit patterns to all 16 lanes.
 */
TWD_EXPORT
void twd_engine_mat4_identity(float mat[16]) {
    for (int i = 0; i < 16; i++) {
        mat[i] = 0.0f;
    }
    mat[0] = 1.0f;
    mat[5] = 1.0f;
    mat[10] = 1.0f;
    mat[15] = 1.0f;
}
