/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stdio.h>

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

    twd_platform_shutdown();
    printf("test_runtime: ok\n");
    return 0;
}
