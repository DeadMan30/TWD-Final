/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stdio.h>
#include <string.h>

#define TWD_CHECK(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "test_runtime failed at %s:%d: %s\n",           \
                    __FILE__, __LINE__, #cond);                             \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static int dummy_fn(void) {
    return 42;
}

typedef struct thread_test_ctx {
    twd_sem_t *sem;
    int value;
} thread_test_ctx_t;

static int worker_thread_fn(void *arg) {
    thread_test_ctx_t *ctx = (thread_test_ctx_t *)arg;
    twd_sem_post(ctx->sem, 1);
    return ctx->value + 8;
}

int main(void) {
    TWD_CHECK(twd_platform_init() == 0);
    uint64_t t0 = twd_time_now_ns();
    uint64_t t1 = twd_time_now_ns();
    TWD_CHECK(t1 >= t0);

    twd_register_hook(0x14036f0d0ULL, "engine_qpc_scale", (void *)&dummy_fn);
    TWD_CHECK(twd_hook_count() == 1);
    const twd_hook_entry_t *e = twd_find_hook(0x14036f0d0ULL);
    TWD_CHECK(e != NULL);
    TWD_CHECK(e->replacement == (void *)&dummy_fn);
    TWD_CHECK(twd_find_hook(0x140000000ULL) == NULL);

    TWD_CHECK(twd_qpc_freq() > 0);
    uint64_t q0 = twd_qpc_now();
    uint32_t ms0 = twd_ticks_ms();
    twd_sleep_ms(1);
    twd_thread_yield();
    uint64_t q1 = twd_qpc_now();
    TWD_CHECK(q1 >= q0);
    (void)ms0;

    TWD_CHECK(twd_pid() != 0);
    TWD_CHECK(twd_tid() != 0);

    twd_mutex_t mtx;
    TWD_CHECK(twd_mutex_init(&mtx) == 0);
    twd_mutex_lock(&mtx);
    twd_mutex_lock(&mtx); /* verify recursive acquisition */
    twd_mutex_unlock(&mtx);
    twd_mutex_unlock(&mtx);
    twd_mutex_destroy(&mtx);

    uint32_t tls_key = twd_tls_alloc();
    TWD_CHECK(tls_key != TWD_TLS_INVALID);
    TWD_CHECK(twd_tls_get(tls_key) == NULL);
    int sentinel = 12345;
    TWD_CHECK(twd_tls_set(tls_key, &sentinel) == 0);
    TWD_CHECK(twd_tls_get(tls_key) == &sentinel);
    TWD_CHECK(twd_tls_free(tls_key) == 0);

    twd_debug_output("twd runtime selftest\n");

    twd_set_last_error(0x1234u);
    TWD_CHECK(twd_last_error() == 0x1234u);

    uint8_t *vm = (uint8_t *)twd_vm_alloc(4096, 0);
    TWD_CHECK(vm != NULL);
    vm[0] = 0x42u;
    vm[4095] = 0x99u;
    TWD_CHECK(twd_vm_protect(vm, 4096, 0, 0) == 0);
    TWD_CHECK(vm[0] == 0x42u);
    twd_vm_free(vm, 4096);

    char *hp = (char *)twd_heap_alloc(16);
    TWD_CHECK(hp != NULL);
    memcpy(hp, "twd", 4);
    hp = (char *)twd_heap_realloc(hp, 64);
    TWD_CHECK(hp != NULL && strcmp(hp, "twd") == 0);
    twd_heap_free(hp);

    twd_event_t ev;
    TWD_CHECK(twd_event_init(&ev, 0, 0) == 0);
    TWD_CHECK(twd_event_wait(&ev, 0) == TWD_WAIT_TIMEOUT);
    TWD_CHECK(twd_event_set(&ev) == 0);
    TWD_CHECK(twd_event_wait(&ev, 100) == TWD_WAIT_OK);
    TWD_CHECK(twd_event_wait(&ev, 0) == TWD_WAIT_TIMEOUT); /* auto-reset check */
    TWD_CHECK(twd_event_set(&ev) == 0);
    TWD_CHECK(twd_event_reset(&ev) == 0);
    TWD_CHECK(twd_event_wait(&ev, 0) == TWD_WAIT_TIMEOUT);
    twd_event_destroy(&ev);

    const char *tmp_file = "twd_pal_test.tmp";
    twd_file_t tf;
    TWD_CHECK(twd_file_open(tmp_file, TWD_FILE_OPEN_READ | TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &tf) == 0);
    TWD_CHECK(twd_file_write(&tf, "TWD4", 4) == 4);
    TWD_CHECK(twd_file_flush(&tf) == 0);
    TWD_CHECK(twd_file_seek(&tf, 0, TWD_SEEK_SET) == 0);
    char rbuf[5] = {0};
    TWD_CHECK(twd_file_read(&tf, rbuf, 4) == 4);
    TWD_CHECK(memcmp(rbuf, "TWD4", 4) == 0);
    TWD_CHECK(twd_file_close(&tf) == 0);
    TWD_CHECK(twd_file_delete(tmp_file) == 0);

    uint16_t u16[16];
    char u8[32];
    int n16 = twd_utf8_to_utf16("TWD\xE2\x9C\x93", -1, u16, 16);
    TWD_CHECK(n16 == 5 && u16[3] == 0x2713u && u16[4] == 0u);
    int n8 = twd_utf16_to_utf8(u16, -1, u8, 32);
    TWD_CHECK(n8 == 7 && strcmp(u8, "TWD\xE2\x9C\x93") == 0);

    char exe_buf[512];
    TWD_CHECK(twd_exe_path(exe_buf, sizeof(exe_buf)) > 0);

    twd_sem_t sem;
    TWD_CHECK(twd_sem_init(&sem, 0, 4) == 0);
    TWD_CHECK(twd_sem_wait(&sem, 0) == TWD_WAIT_TIMEOUT);

    thread_test_ctx_t tctx;
    tctx.sem = &sem;
    tctx.value = 34;
    twd_thread_t th;
    TWD_CHECK(twd_thread_create(&th, worker_thread_fn, &tctx) == 0);
    TWD_CHECK(twd_sem_wait(&sem, 5000) == TWD_WAIT_OK);
    int th_rc = 0;
    TWD_CHECK(twd_thread_join(&th, &th_rc) == 0);
    TWD_CHECK(th_rc == 42);
    twd_sem_destroy(&sem);

    void *self_mod = twd_dynlib_open(NULL);
    TWD_CHECK(self_mod != NULL);
    twd_dynlib_close(self_mod);

    TWD_CHECK(twd_alert_box("TWD-Final", "alert test", 0u) == TWD_ALERT_OK);
    TWD_CHECK(twd_alert_box("TWD-Final", "yesno test", 4u) == TWD_ALERT_YES);

    twd_window_t *win = twd_window_create("TWD4 Test Window", 10, 20, 1280, 720, 0u);
    TWD_CHECK(win != NULL && twd_window_is_valid(win) == 1);
    twd_window_show(win, 1);
    TWD_CHECK(win->visible == 1);
    twd_window_set_rect(win, 0, 0, 1920, 1080);
    int cw = 0, ch = 0;
    twd_window_get_client_rect(win, &cw, &ch);
    TWD_CHECK(cw == 1920 && ch == 1080);
    char wtitle[64];
    TWD_CHECK(twd_window_get_title(win, wtitle, sizeof(wtitle)) > 0);
    TWD_CHECK(strcmp(wtitle, "TWD4 Test Window") == 0);

    twd_win_event_t ev_in;
    ev_in.kind = TWD_EVT_KEY;
    ev_in.window_id = (uintptr_t)win;
    ev_in.param1 = 0x41u;
    ev_in.param2 = 1;
    twd_window_push_event(&ev_in);
    twd_win_event_t ev_out;
    TWD_CHECK(twd_window_poll_event(&ev_out) == 1);
    TWD_CHECK(ev_out.kind == TWD_EVT_KEY && ev_out.param1 == 0x41u);
    twd_post_quit(7);
    TWD_CHECK(twd_window_poll_event(&ev_out) == 1 && ev_out.kind == TWD_EVT_QUIT);
    int qcode = 0;
    TWD_CHECK(twd_quit_requested(&qcode) == 1 && qcode == 7);
    twd_window_destroy(win);
    TWD_CHECK(twd_window_is_valid(win) == 0);

    TWD_CHECK(twd_system_metric(TWD_METRIC_SCREEN_W) > 0);
    TWD_CHECK(twd_system_metric(TWD_METRIC_SCREEN_H) > 0);
    TWD_CHECK(twd_cpu_count() >= 1u);
    TWD_CHECK(twd_page_size() >= 4096u);
    TWD_CHECK(twd_unix_time_ns() > 1000000000000000000ULL);
    TWD_CHECK(twd_timer_begin_period(1u) == 0);
    TWD_CHECK(twd_timer_end_period(1u) == 0);

    uint8_t ic_buf[16];
    memset(ic_buf, 0x90, sizeof(ic_buf));
    twd_icache_flush(ic_buf, sizeof(ic_buf));

    char cwd_buf[512];
    TWD_CHECK(twd_getcwd(cwd_buf, sizeof(cwd_buf)) > 0);
    TWD_CHECK(twd_setenv("TWD_PAL_ENV_TEST", "ok42") == 0);
    char env_buf[32];
    TWD_CHECK(twd_getenv("TWD_PAL_ENV_TEST", env_buf, sizeof(env_buf)) == 4);
    TWD_CHECK(strcmp(env_buf, "ok42") == 0);

    const uint16_t u16_a[] = {'A', 'b', 'c', 0};
    const uint16_t u16_b[] = {'a', 'B', 'C', 0};
    TWD_CHECK(twd_utf16_len(u16_a) == 3u);
    TWD_CHECK(twd_utf16_cmp(u16_a, u16_b, 0) != 0);
    TWD_CHECK(twd_utf16_cmp(u16_a, u16_b, 1) == 0);

    TWD_CHECK(twd_mkdir("twd_pal_dir_tmp") == 0);
    TWD_CHECK(twd_path_is_dir("twd_pal_dir_tmp") == 1);
    twd_file_t df;
    TWD_CHECK(twd_file_open("twd_pal_dir_tmp/a.bin", TWD_FILE_OPEN_READ | TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &df) == 0);
    TWD_CHECK(twd_file_write(&df, "ABCDEFGH", 8) == 8);
    TWD_CHECK(twd_file_truncate(&df, 4) == 0);
    TWD_CHECK(twd_file_close(&df) == 0);
    TWD_CHECK(twd_file_rename("twd_pal_dir_tmp/a.bin", "twd_pal_dir_tmp/b.bin") == 0);
    TWD_CHECK(twd_file_exists("twd_pal_dir_tmp/b.bin") == 1);
    twd_dir_t dir;
    TWD_CHECK(twd_dir_open("twd_pal_dir_tmp", &dir) == 0);
    twd_dir_entry_t dent;
    int saw_b = 0;
    while (twd_dir_next(&dir, &dent) > 0) {
        if (strcmp(dent.name, "b.bin") == 0) saw_b = 1;
    }
    twd_dir_close(&dir);
    TWD_CHECK(saw_b == 1);
    TWD_CHECK(twd_file_delete("twd_pal_dir_tmp/b.bin") == 0);
    TWD_CHECK(twd_rmdir("twd_pal_dir_tmp") == 0);

    twd_alert("TWD-Final", "runtime alert check");

    twd_platform_shutdown();
    printf("test_runtime: ok\n");
    return 0;
}
