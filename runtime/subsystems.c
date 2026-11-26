/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

/* ---------- Input / Gamepad / Touch PAL ---------- */

#define TWD_ERR_SUCCESS              0u
#define TWD_ERR_DEVICE_NOT_CONNECTED 1167u

typedef struct pad_slot {
    int connected;
    twd_gamepad_state_t state;
    uint16_t rumble_left;
    uint16_t rumble_right;
} pad_slot_t;

static pad_slot_t g_pads[TWD_MAX_GAMEPADS];
static twd_pointer_state_t g_pointer = {0.0f, 0.0f, 0u, 1};
static uint8_t g_keys[256];

void twd_input_set_gamepad_connected(uint32_t user_index, int connected) {
    if (user_index >= TWD_MAX_GAMEPADS) return;
    g_pads[user_index].connected = (connected != 0);
}

void twd_input_push_gamepad_state(uint32_t user_index, const twd_gamepad_state_t *state) {
    if (user_index >= TWD_MAX_GAMEPADS || !state) return;
    g_pads[user_index].connected = 1;
    g_pads[user_index].state = *state;
    if (g_pads[user_index].state.packet_number == 0u) {
        g_pads[user_index].state.packet_number = 1u;
    }
}

uint32_t twd_input_get_gamepad_state(uint32_t user_index, twd_gamepad_state_t *out_state) {
    if (user_index >= TWD_MAX_GAMEPADS || !g_pads[user_index].connected) {
        if (out_state) memset(out_state, 0, sizeof(*out_state));
        return TWD_ERR_DEVICE_NOT_CONNECTED;
    }
    if (out_state) *out_state = g_pads[user_index].state;
    return TWD_ERR_SUCCESS;
}

uint32_t twd_input_set_rumble(uint32_t user_index, uint16_t left_motor, uint16_t right_motor) {
    if (user_index >= TWD_MAX_GAMEPADS || !g_pads[user_index].connected) {
        return TWD_ERR_DEVICE_NOT_CONNECTED;
    }
    g_pads[user_index].rumble_left = left_motor;
    g_pads[user_index].rumble_right = right_motor;
    return TWD_ERR_SUCCESS;
}

void twd_input_get_rumble(uint32_t user_index, uint16_t *out_left, uint16_t *out_right) {
    if (user_index >= TWD_MAX_GAMEPADS) {
        if (out_left) *out_left = 0u;
        if (out_right) *out_right = 0u;
        return;
    }
    if (out_left) *out_left = g_pads[user_index].rumble_left;
    if (out_right) *out_right = g_pads[user_index].rumble_right;
}

uint32_t twd_input_get_gamepad_caps(uint32_t user_index, uint16_t *out_type, uint16_t *out_subtype) {
    if (user_index >= TWD_MAX_GAMEPADS || !g_pads[user_index].connected) {
        return TWD_ERR_DEVICE_NOT_CONNECTED;
    }
    if (out_type) *out_type = 1u;     /* XINPUT_DEVTYPE_GAMEPAD */
    if (out_subtype) *out_subtype = 1u; /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return TWD_ERR_SUCCESS;
}

void twd_input_set_pointer(float x, float y, uint32_t buttons) {
    g_pointer.x = x;
    g_pointer.y = y;
    g_pointer.buttons = buttons;
}

void twd_input_get_pointer(twd_pointer_state_t *out) {
    if (out) *out = g_pointer;
}

void twd_input_set_key(uint32_t vkey, int down) {
    if (vkey < 256u) g_keys[vkey] = down ? 1u : 0u;
}

int twd_input_key_down(uint32_t vkey) {
    return (vkey < 256u) ? (int)g_keys[vkey] : 0;
}

typedef struct dinput8_stub {
    const void *const *lpVtbl;
    int32_t refcount;
} dinput8_stub_t;

static int32_t di8_qi(void *self, const void *riid, void **ppv) {
    (void)riid;
    if (!self || !ppv) return (int32_t)0x80070057u;
    ((dinput8_stub_t *)self)->refcount++;
    *ppv = self;
    return 0;
}

static uint32_t di8_addref(void *self) {
    return self ? (uint32_t)(++((dinput8_stub_t *)self)->refcount) : 0u;
}

static uint32_t di8_release(void *self) {
    if (!self) return 0u;
    dinput8_stub_t *o = (dinput8_stub_t *)self;
    int32_t rc = --o->refcount;
    if (rc <= 0) {
        free(o);
        return 0u;
    }
    return (uint32_t)rc;
}

static int32_t di8_fail_unsupported(void *self) {
    (void)self;
    return (int32_t)0x80004001u; /* E_NOTIMPL for PC DirectInput peripherals */
}

static const void *const g_di8_vtbl[8] = {
    (const void *)&di8_qi,
    (const void *)&di8_addref,
    (const void *)&di8_release,
    (const void *)&di8_fail_unsupported, /* CreateDevice */
    (const void *)&di8_fail_unsupported, /* EnumDevices -> 0 devices, falls back to XInput/touch */
    (const void *)&di8_fail_unsupported,
    (const void *)&di8_fail_unsupported,
    (const void *)&di8_fail_unsupported
};

int32_t twd_dinput8_create_stub(void **out_iface) {
    if (!out_iface) return (int32_t)0x80070057u;
    dinput8_stub_t *o = (dinput8_stub_t *)calloc(1, sizeof(*o));
    if (!o) return (int32_t)0x8007000Eu;
    o->lpVtbl = g_di8_vtbl;
    o->refcount = 1;
    *out_iface = o;
    return 0;
}

/* ---------- Media Foundation / Audio Focus PAL ---------- */

static int g_media_init_count = 0;
static int g_audio_focus = 1;

int32_t twd_media_startup(uint32_t version, uint32_t flags) {
    (void)version;
    (void)flags;
    g_media_init_count++;
    return 0;
}

int32_t twd_media_shutdown(void) {
    if (g_media_init_count > 0) g_media_init_count--;
    return 0;
}

int twd_media_is_initialized(void) {
    return g_media_init_count > 0 ? 1 : 0;
}

void twd_audio_set_focus(int active) {
    g_audio_focus = active ? 1 : 0;
}

int twd_audio_has_focus(void) {
    return g_audio_focus;
}

/* ---------- Save/AppData Directories, COM/KV & Telemetry Stubs ---------- */

static int copy_path_str(const char *src, char *buf, size_t cap) {
    if (!src || !buf || cap == 0u) return -1;
    size_t n = strlen(src);
    if (n >= cap) return -1;
    memcpy(buf, src, n + 1u);
    return (int)n;
}

int twd_app_data_dir(int kind, char *buf, size_t cap) {
    if (!buf || cap < 8u) return -1;
    char tmp[512];
    if (kind == TWD_DIR_TEMP) {
        if (twd_getenv("TMPDIR", tmp, sizeof(tmp)) > 0 ||
            twd_getenv("TEMP", tmp, sizeof(tmp)) > 0 ||
            twd_getenv("TMP", tmp, sizeof(tmp)) > 0) {
            return copy_path_str(tmp, buf, cap);
        }
        return twd_getcwd(buf, cap);
    }
    if (twd_getenv("TWD_SAVE_DIR", tmp, sizeof(tmp)) > 0 ||
        twd_getenv("ANDROID_DATA", tmp, sizeof(tmp)) > 0) {
        return copy_path_str(tmp, buf, cap);
    }
    if (twd_getenv("HOME", tmp, sizeof(tmp)) > 0 ||
        twd_getenv("USERPROFILE", tmp, sizeof(tmp)) > 0) {
        const char *sub = (kind == TWD_DIR_CONFIG) ? "/.config/twd-final" : "/.local/share/twd-final";
        size_t base_len = strlen(tmp);
        size_t sub_len = strlen(sub);
        if (base_len + sub_len < cap) {
            memcpy(buf, tmp, base_len);
            memcpy(buf + base_len, sub, sub_len + 1u);
            return (int)(base_len + sub_len);
        }
    }
    return twd_getcwd(buf, cap);
}

int twd_path_is_empty_dir(const char *path) {
    twd_dir_t d;
    if (!path || twd_dir_open(path, &d) != 0) return 0;
    twd_dir_entry_t ent;
    int has_any = twd_dir_next(&d, &ent);
    twd_dir_close(&d);
    return has_any == 0 ? 1 : 0;
}

static int g_com_depth = 0;

int32_t twd_com_init(uint32_t coinit_flags) {
    (void)coinit_flags;
    int32_t hr = (g_com_depth == 0) ? 0 : 1; /* S_OK then S_FALSE */
    g_com_depth++;
    return hr;
}

void twd_com_uninit(void) {
    if (g_com_depth > 0) g_com_depth--;
}

int twd_com_is_initialized(void) {
    return g_com_depth > 0 ? 1 : 0;
}

void *twd_com_alloc(size_t size) {
    return twd_heap_alloc(size);
}

void *twd_com_realloc(void *ptr, size_t size) {
    return twd_heap_realloc(ptr, size);
}

void twd_com_free(void *ptr) {
    twd_heap_free(ptr);
}

#define TWD_KV_MAX 32u

typedef struct kv_entry {
    int used;
    char section[64];
    char key[64];
    char value[128];
} kv_entry_t;

static kv_entry_t g_kv_store[TWD_KV_MAX];

static void bounded_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int twd_kv_set(const char *section, const char *key, const char *value) {
    if (!section || !key || !value) return -1;
    int free_idx = -1;
    for (uint32_t i = 0; i < TWD_KV_MAX; i++) {
        if (g_kv_store[i].used) {
            if (strcmp(g_kv_store[i].section, section) == 0 &&
                strcmp(g_kv_store[i].key, key) == 0) {
                bounded_copy(g_kv_store[i].value, sizeof(g_kv_store[i].value), value);
                return 0;
            }
        } else if (free_idx < 0) {
            free_idx = (int)i;
        }
    }
    if (free_idx < 0) return -1;
    kv_entry_t *e = &g_kv_store[free_idx];
    e->used = 1;
    bounded_copy(e->section, sizeof(e->section), section);
    bounded_copy(e->key, sizeof(e->key), key);
    bounded_copy(e->value, sizeof(e->value), value);
    return 0;
}

int twd_kv_get(const char *section, const char *key, char *buf, size_t cap) {
    if (!section || !key) return -1;
    for (uint32_t i = 0; i < TWD_KV_MAX; i++) {
        if (g_kv_store[i].used &&
            strcmp(g_kv_store[i].section, section) == 0 &&
            strcmp(g_kv_store[i].key, key) == 0) {
            return buf ? copy_path_str(g_kv_store[i].value, buf, cap)
                       : (int)strlen(g_kv_store[i].value);
        }
    }
    return -1;
}

int twd_kv_delete(const char *section, const char *key) {
    if (!section || !key) return -1;
    for (uint32_t i = 0; i < TWD_KV_MAX; i++) {
        if (g_kv_store[i].used &&
            strcmp(g_kv_store[i].section, section) == 0 &&
            strcmp(g_kv_store[i].key, key) == 0) {
            g_kv_store[i].used = 0;
            return 0;
        }
    }
    return -1;
}

int twd_telemetry_enabled(void) {
    return 0;
}

uint32_t twd_wininet_offline_status(void) {
    return 12007u; /* ERROR_INTERNET_NAME_NOT_RESOLVED / offline telemetry stub */
}

void twd_eventlog_stub(const char *source, const char *msg) {
    (void)source;
    (void)msg;
}
