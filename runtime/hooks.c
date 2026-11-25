/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stddef.h>

#define TWD_MAX_HOOKS 4096

static twd_hook_entry_t g_hooks[TWD_MAX_HOOKS];
static uint32_t g_hook_count = 0;

void twd_register_hook(uint64_t address, const char *name, void *replacement) {
    for (uint32_t i = 0; i < g_hook_count; i++) {
        if (g_hooks[i].address == address) {
            g_hooks[i].name = name;
            g_hooks[i].replacement = replacement;
            return;
        }
    }
    if (g_hook_count < TWD_MAX_HOOKS) {
        g_hooks[g_hook_count].address = address;
        g_hooks[g_hook_count].name = name;
        g_hooks[g_hook_count].replacement = replacement;
        g_hook_count++;
    }
}

const twd_hook_entry_t *twd_find_hook(uint64_t address) {
    for (uint32_t i = 0; i < g_hook_count; i++) {
        if (g_hooks[i].address == address) {
            return &g_hooks[i];
        }
    }
    return NULL;
}

uint32_t twd_hook_count(void) {
    return g_hook_count;
}
