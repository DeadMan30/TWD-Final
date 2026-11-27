/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"
#ifdef _MSC_VER
#include <xmmintrin.h>
#endif

/*
 * Original: WalkingDead4.exe @ 0x1400359f0 (54 bytes, 71 callers)
 *
 * Initializes a 4x4 row-major float matrix to the identity matrix, writing
 * exact IEEE-754 +0.0f / 1.0f bit patterns to all 16 lanes.
 */
TWD_EXPORT
void twd_engine_mat4_identity(float mat[16]) {
    mat[0] = 1.0f;
    mat[1] = 0.0f;
    mat[2] = 0.0f;
    mat[3] = 0.0f;
    mat[4] = 0.0f;
    mat[7] = 0.0f;
    mat[8] = 0.0f;
    mat[9] = 0.0f;
    mat[12] = 0.0f;
    mat[13] = 0.0f;
    mat[14] = 0.0f;
    mat[5] = 1.0f;
    mat[6] = 0.0f;
    mat[10] = 1.0f;
    mat[11] = 0.0f;
    mat[15] = 1.0f;
}

#ifdef _MSC_VER
#define TWD_FORCEINLINE static __forceinline
#else
#define TWD_FORCEINLINE static inline __attribute__((always_inline))
#endif

TWD_FORCEINLINE void mat4_diagonal_rev_zero(float mat[16], float sx, float sy, float sz) {
    mat[0] = sx;
    mat[13] = 0.0f;
    mat[14] = 0.0f;
    mat[11] = 0.0f;
    mat[12] = 0.0f;
    mat[1] = 0.0f;
    mat[2] = 0.0f;
    mat[3] = 0.0f;
    mat[4] = 0.0f;
    mat[6] = 0.0f;
    mat[7] = 0.0f;
    mat[8] = 0.0f;
    mat[9] = 0.0f;
    mat[5] = sy;
    mat[10] = sz;
    mat[15] = 1.0f;
}

/*
 * Original: WalkingDead4.exe @ 0x140037630 (48 bytes, 5 callers)
 *
 * Initializes a 4x4 matrix to uniform scale (s, s, s, 1).
 */
TWD_EXPORT
void twd_engine_mat4_uniform_scale(float mat[16], float s) {
    mat[0] = s;
    mat[1] = 0.0f;
    mat[2] = 0.0f;
    mat[3] = 0.0f;
    mat[4] = 0.0f;
    mat[6] = 0.0f;
    mat[7] = 0.0f;
    mat[8] = 0.0f;
    mat[9] = 0.0f;
    mat[11] = 0.0f;
    mat[12] = 0.0f;
    mat[13] = 0.0f;
    mat[14] = 0.0f;
    mat[5] = s;
    mat[10] = s;
    mat[15] = 1.0f;
}

/*
 * Original: WalkingDead4.exe @ 0x140037670 (48 bytes, 11 callers)
 *
 * Initializes a 4x4 matrix to diagonal scale (sx, sy, sz, 1).
 */
TWD_EXPORT
void twd_engine_mat4_diagonal_scale(float mat[16], float sx, float sy, float sz) {
    mat[0] = sx;
    mat[1] = 0.0f;
    mat[2] = 0.0f;
    mat[3] = 0.0f;
    mat[4] = 0.0f;
    mat[6] = 0.0f;
    mat[7] = 0.0f;
    mat[8] = 0.0f;
    mat[9] = 0.0f;
    mat[11] = 0.0f;
    mat[12] = 0.0f;
    mat[13] = 0.0f;
    mat[14] = 0.0f;
    mat[5] = sy;
    mat[10] = sz;
    mat[15] = 1.0f;
}

/*
 * Original: WalkingDead4.exe @ 0x140037960 (64 bytes, 15 callers)
 *
 * Initializes a 4x4 translation matrix with identity rotation/scale and row-3
 * translation t[0..2].
 */
TWD_EXPORT
void twd_engine_mat4_from_translation(float mat[16], const float t[3]) {
    mat[0] = 1.0f;
    mat[1] = 0.0f;
    mat[2] = 0.0f;
    mat[3] = 0.0f;
    mat[4] = 0.0f;
    mat[7] = 0.0f;
    mat[8] = 0.0f;
    mat[9] = 0.0f;
    mat[5] = 1.0f;
    mat[6] = 0.0f;
    mat[10] = 1.0f;
    mat[11] = 0.0f;
    mat[12] = t[0];
    mat[13] = t[1];
    mat[14] = t[2];
    mat[15] = 1.0f;
}

/*
 * Original: WalkingDead4.exe @ 0x1400379b0 (78 bytes, 9 callers)
 *
 * Initializes a 4x4 matrix with diagonal scale s[0..2] and translation t[0..2].
 */
TWD_EXPORT
void twd_engine_mat4_from_scale_translation_vec(float mat[16], const float s[3], const float t[3]) {
    mat4_diagonal_rev_zero(mat, s[0], s[1], s[2]);
    mat[12] = t[0];
    mat[13] = t[1];
    mat[14] = t[2];
}

/*
 * Original: WalkingDead4.exe @ 0x140037350 (88 bytes)
 *
 * Multiplies 4x4 matrix src by scalar s into out and returns out.
 */
TWD_EXPORT
float *twd_engine_mat4_mul_scalar(const float src[16], float out[16], float s) {
#ifdef _MSC_VER
    __m128 vs = _mm_set1_ps(s);
    __m128 r0 = _mm_mul_ps(_mm_loadu_ps(src + 0), vs);
    float m13 = src[13] * _mm_cvtss_f32(vs);
    _mm_storeu_ps(out + 0, r0);
    _mm_storeu_ps(out + 4, _mm_mul_ps(_mm_loadu_ps(src + 4), vs));
    _mm_storeu_ps(out + 8, _mm_mul_ps(_mm_loadu_ps(src + 8), vs));
    out[12] = src[12] * _mm_cvtss_f32(vs);
    float m14 = src[14] * _mm_cvtss_f32(vs);
    float m15 = src[15] * _mm_cvtss_f32(vs);
    out[13] = m13;
    out[14] = m14;
    out[15] = m15;
    return out;
#else
    for (int i = 0; i < 16; ++i) {
        out[i] = src[i] * s;
    }
    return out;
#endif
}

/*
 * Original: WalkingDead4.exe @ 0x1400375d0 (76 bytes)
 *
 * Initializes a 4x4 matrix with diagonal scale (sx, sy, sz) and translation (tx, ty, tz).
 */
TWD_EXPORT
void twd_engine_mat4_from_diag_translation(float mat[16], float tx, float ty, float tz,
                                           float sx, float sy, float sz) {
    mat[0] = sx;
    mat[5] = sy;
    mat[10] = sz;
    mat[12] = tx;
    mat[13] = ty;
    mat[14] = tz;
    mat[1] = 0.0f;
    mat[2] = 0.0f;
    mat[3] = 0.0f;
    mat[4] = 0.0f;
    mat[6] = 0.0f;
    mat[7] = 0.0f;
    mat[8] = 0.0f;
    mat[9] = 0.0f;
    mat[11] = 0.0f;
    mat[15] = 1.0f;
}
