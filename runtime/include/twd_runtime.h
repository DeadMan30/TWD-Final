/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TWD_RUNTIME_H
#define TWD_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#if !defined(__cplusplus) && defined(_MSC_VER) && _MSC_VER < 1928
#define _TWD_CAT2(a, b) a##b
#define _TWD_CAT(a, b) _TWD_CAT2(a, b)
#ifndef _Static_assert
#define _Static_assert(cond, msg) typedef char _TWD_CAT(_twd_sa_, __COUNTER__)[(cond) ? 1 : -1]
#endif
#endif

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

typedef struct twd_dir {
    uintptr_t handle;
    int first_pending;
    char pending_name[260];
} twd_dir_t;

typedef struct twd_dir_entry {
    char name[260];
    int is_dir;
    uint64_t size;
} twd_dir_entry_t;

typedef struct twd_window {
    uint32_t magic;
    int x;
    int y;
    int width;
    int height;
    uint32_t style;
    int visible;
    char title[128];
} twd_window_t;

typedef struct twd_win_event {
    uint32_t kind;
    uintptr_t window_id;
    uint64_t param1;
    int64_t param2;
} twd_win_event_t;

#define TWD_EVT_NONE    0u
#define TWD_EVT_CLOSE   1u
#define TWD_EVT_RESIZE  2u
#define TWD_EVT_KEY     3u
#define TWD_EVT_POINTER 4u
#define TWD_EVT_QUIT    5u

#define TWD_ALERT_OK     1
#define TWD_ALERT_CANCEL 2
#define TWD_ALERT_YES    6

#define TWD_METRIC_SCREEN_W      0
#define TWD_METRIC_SCREEN_H      1
#define TWD_METRIC_MOUSE_PRESENT 19

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
int twd_rmdir(const char *path);

int twd_utf8_to_utf16(const char *src, int src_len, uint16_t *dst, int dst_cap);
int twd_utf16_to_utf8(const uint16_t *src, int src_len, char *dst, int dst_cap);
size_t twd_utf16_len(const uint16_t *s);
int twd_utf16_cmp(const uint16_t *a, const uint16_t *b, int ignore_case);

int twd_exe_path(char *buf, size_t cap);
int twd_getcwd(char *buf, size_t cap);
int twd_chdir(const char *path);
int twd_getenv(const char *name, char *buf, size_t cap);
int twd_setenv(const char *name, const char *val);

int twd_file_exists(const char *path);
int twd_path_is_dir(const char *path);
int twd_file_rename(const char *src, const char *dst);
int twd_file_truncate(twd_file_t *f, int64_t size);
int twd_dir_open(const char *path, twd_dir_t *out);
int twd_dir_next(twd_dir_t *d, twd_dir_entry_t *out);
void twd_dir_close(twd_dir_t *d);

uint32_t twd_cpu_count(void);
uint32_t twd_page_size(void);
uint64_t twd_unix_time_ns(void);
void twd_icache_flush(void *addr, size_t size);
int twd_timer_begin_period(uint32_t ms);
int twd_timer_end_period(uint32_t ms);

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
int twd_alert_box(const char *title, const char *msg, uint32_t flags);

twd_window_t *twd_window_create(const char *title, int x, int y, int width, int height, uint32_t style);
void twd_window_destroy(twd_window_t *win);
int twd_window_is_valid(const twd_window_t *win);
void twd_window_show(twd_window_t *win, int visible);
void twd_window_set_title(twd_window_t *win, const char *title);
int twd_window_get_title(const twd_window_t *win, char *buf, size_t cap);
void twd_window_set_rect(twd_window_t *win, int x, int y, int width, int height);
void twd_window_get_client_rect(const twd_window_t *win, int *out_w, int *out_h);
void twd_window_push_event(const twd_win_event_t *evt);
int twd_window_poll_event(twd_win_event_t *out_evt);
void twd_post_quit(int exit_code);
int twd_quit_requested(int *out_code);
int twd_system_metric(int metric_id);

void twd_register_hook(uint64_t address, const char *name, void *replacement);
const twd_hook_entry_t *twd_find_hook(uint64_t address);
const twd_hook_entry_t *twd_hook_at(uint32_t index);
uint32_t twd_hook_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TWD_RUNTIME_H */
