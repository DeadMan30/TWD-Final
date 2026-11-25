/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TWD_RUNTIME_H
#define TWD_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TWD_TLS_INVALID 0xffffffffu

typedef struct twd_hook_entry {
    uint64_t address;
    const char *name;
    void *replacement;
} twd_hook_entry_t;

typedef struct twd_mutex {
    uint64_t opaque[8];
} twd_mutex_t;

int twd_platform_init(void);
void twd_platform_shutdown(void);
uint64_t twd_time_now_ns(void);
uint64_t twd_qpc_now(void);
uint64_t twd_qpc_freq(void);
uint32_t twd_ticks_ms(void);
void twd_sleep_ms(uint32_t ms);
void twd_thread_yield(void);
uint32_t twd_pid(void);
uint32_t twd_tid(void);
void twd_debug_output(const char *msg);
void twd_log(const char *tag, const char *fmt, ...);

int twd_mutex_init(twd_mutex_t *m);
void twd_mutex_destroy(twd_mutex_t *m);
void twd_mutex_lock(twd_mutex_t *m);
void twd_mutex_unlock(twd_mutex_t *m);

uint32_t twd_tls_alloc(void);
int twd_tls_free(uint32_t key);
void *twd_tls_get(uint32_t key);
int twd_tls_set(uint32_t key, void *value);

void twd_register_hook(uint64_t address, const char *name, void *replacement);
const twd_hook_entry_t *twd_find_hook(uint64_t address);
uint32_t twd_hook_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TWD_RUNTIME_H */
