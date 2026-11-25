/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <assert.h>
#include <stdio.h>

static int dummy_fn(void) {
    return 42;
}

int main(void) {
    assert(twd_platform_init() == 0);
    uint64_t t0 = twd_time_now_ns();
    uint64_t t1 = twd_time_now_ns();
    assert(t1 >= t0);

    twd_register_hook(0x14036f0d0ULL, "engine_qpc_scale", (void *)&dummy_fn);
    assert(twd_hook_count() == 1);
    const twd_hook_entry_t *e = twd_find_hook(0x14036f0d0ULL);
    assert(e != NULL);
    assert(e->replacement == (void *)&dummy_fn);
    assert(twd_find_hook(0x140000000ULL) == NULL);

    twd_platform_shutdown();
    printf("test_runtime: ok\n");
    return 0;
}
