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

    twd_platform_shutdown();
    printf("test_runtime: ok\n");
    return 0;
}
