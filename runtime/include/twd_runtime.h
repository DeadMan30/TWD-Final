/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TWD_RUNTIME_H
#define TWD_RUNTIME_H

#include <stddef.h>
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

typedef union twd_mutex {
    uint64_t align;
    uint8_t bytes[64];
} twd_mutex_t;

typedef union twd_event {
    uint64_t align;
    uint8_t bytes[128];
} twd_event_t;

typedef union twd_sem {
    uint64_t align;
    uint8_t bytes[128];
} twd_sem_t;

typedef struct twd_thread {
    uintptr_t handle;
    void *ctx;
} twd_thread_t;

typedef int (*twd_thread_fn)(void *arg);

typedef struct twd_file {
    intptr_t handle;
} twd_file_t;

#define TWD_FILE_OPEN_READ   1u
#define TWD_FILE_OPEN_WRITE  2u
#define TWD_FILE_CREATE      4u

#define TWD_SEEK_SET 0
#define TWD_SEEK_CUR 1
#define TWD_SEEK_END 2

#define TWD_WAIT_OK      0
#define TWD_WAIT_TIMEOUT 1
#define TWD_WAIT_ERROR   -1
#define TWD_WAIT_INFINITE 0xffffffffu

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
uint32_t twd_last_error(void);
void twd_set_last_error(uint32_t err);
void twd_debug_output(const char *msg);
void twd_log(const char *tag, const char *fmt, ...);

void *twd_vm_alloc(size_t size, int exec);
int twd_vm_protect(void *ptr, size_t size, int read_write, int exec);
void twd_vm_free(void *ptr, size_t size);
void *twd_heap_alloc(size_t size);
void *twd_heap_realloc(void *ptr, size_t size);
void twd_heap_free(void *ptr);

int twd_mutex_init(twd_mutex_t *m);
void twd_mutex_destroy(twd_mutex_t *m);
void twd_mutex_lock(twd_mutex_t *m);
void twd_mutex_unlock(twd_mutex_t *m);

int twd_event_init(twd_event_t *ev, int manual_reset, int initial_state);
void twd_event_destroy(twd_event_t *ev);
int twd_event_set(twd_event_t *ev);
int twd_event_reset(twd_event_t *ev);
int twd_event_wait(twd_event_t *ev, uint32_t timeout_ms);

uint32_t twd_tls_alloc(void);
int twd_tls_free(uint32_t key);
void *twd_tls_get(uint32_t key);
int twd_tls_set(uint32_t key, void *value);

int twd_file_open(const char *path, uint32_t flags, twd_file_t *out);
int64_t twd_file_read(twd_file_t *f, void *buf, size_t len);
int64_t twd_file_write(twd_file_t *f, const void *buf, size_t len);
int64_t twd_file_seek(twd_file_t *f, int64_t offset, int whence);
int twd_file_flush(twd_file_t *f);
int twd_file_close(twd_file_t *f);
int twd_file_delete(const char *path);
int twd_mkdir(const char *path);

int twd_utf8_to_utf16(const char *src, int src_len, uint16_t *dst, int dst_cap);
int twd_utf16_to_utf8(const uint16_t *src, int src_len, char *dst, int dst_cap);

int twd_exe_path(char *buf, size_t cap);

int twd_sem_init(twd_sem_t *s, uint32_t initial, uint32_t max_count);
void twd_sem_destroy(twd_sem_t *s);
int twd_sem_post(twd_sem_t *s, uint32_t count);
int twd_sem_wait(twd_sem_t *s, uint32_t timeout_ms);

int twd_thread_create(twd_thread_t *th, twd_thread_fn fn, void *arg);
int twd_thread_join(twd_thread_t *th, int *out_rc);

void *twd_dynlib_open(const char *path_or_null);
void *twd_dynlib_sym(void *lib, const char *name);
void twd_dynlib_close(void *lib);

void twd_alert(const char *title, const char *msg);

void twd_register_hook(uint64_t address, const char *name, void *replacement);
const twd_hook_entry_t *twd_find_hook(uint64_t address);
const twd_hook_entry_t *twd_hook_at(uint32_t index);
uint32_t twd_hook_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TWD_RUNTIME_H */
