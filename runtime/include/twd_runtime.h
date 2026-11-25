/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TWD_RUNTIME_H
#define TWD_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct twd_hook_entry {
    uint64_t address;
    const char *name;
    void *replacement;
} twd_hook_entry_t;

int twd_platform_init(void);
void twd_platform_shutdown(void);
uint64_t twd_time_now_ns(void);
void twd_log(const char *tag, const char *fmt, ...);

void twd_register_hook(uint64_t address, const char *name, void *replacement);
const twd_hook_entry_t *twd_find_hook(uint64_t address);
uint32_t twd_hook_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TWD_RUNTIME_H */
