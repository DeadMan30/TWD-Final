/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TWD_GAME_H
#define TWD_GAME_H

#include "twd_runtime.h"

#ifdef _WIN32
#define TWD_EXPORT __declspec(dllexport)
#else
#define TWD_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

TWD_EXPORT void twd_game_register_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* TWD_GAME_H */
