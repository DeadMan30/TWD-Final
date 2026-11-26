/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <android/asset_manager.h>
#endif

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

/* ---------- GUID, COM Memory Stream & Factory Stub ---------- */

static uint64_t g_guid_rng = 0x9e3779b97f4a7c15ULL;

static uint32_t next_u32(void) {
    uint64_t x = g_guid_rng ^ twd_time_now_ns();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_guid_rng = x ? x : 0x517cc1b727220a95ULL;
    return (uint32_t)((g_guid_rng * 0x2545F4914F6CDD1DULL) >> 32);
}

int twd_guid_generate(twd_guid_t *out) {
    if (!out) return -1;
    uint32_t r0 = next_u32();
    uint32_t r1 = next_u32();
    uint32_t r2 = next_u32();
    uint32_t r3 = next_u32();
    out->data1 = r0;
    out->data2 = (uint16_t)(r1 & 0xffffu);
    out->data3 = (uint16_t)(((r1 >> 16) & 0x0fffu) | 0x4000u); /* v4 */
    out->data4[0] = (uint8_t)((r2 & 0x3fu) | 0x80u);           /* RFC 4122 variant */
    out->data4[1] = (uint8_t)((r2 >> 8) & 0xffu);
    out->data4[2] = (uint8_t)((r2 >> 16) & 0xffu);
    out->data4[3] = (uint8_t)((r2 >> 24) & 0xffu);
    out->data4[4] = (uint8_t)(r3 & 0xffu);
    out->data4[5] = (uint8_t)((r3 >> 8) & 0xffu);
    out->data4[6] = (uint8_t)((r3 >> 16) & 0xffu);
    out->data4[7] = (uint8_t)((r3 >> 24) & 0xffu);
    return 0;
}

static void put_hex_byte(char *dst, uint8_t b) {
    static const char hx[] = "0123456789ABCDEF";
    dst[0] = hx[(b >> 4) & 0xfu];
    dst[1] = hx[b & 0xfu];
}

int twd_guid_to_string(const twd_guid_t *g, char *buf, size_t cap) {
    if (!g || !buf || cap < 39u) return -1;
    buf[0] = '{';
    put_hex_byte(buf + 1, (uint8_t)(g->data1 >> 24));
    put_hex_byte(buf + 3, (uint8_t)(g->data1 >> 16));
    put_hex_byte(buf + 5, (uint8_t)(g->data1 >> 8));
    put_hex_byte(buf + 7, (uint8_t)g->data1);
    buf[9] = '-';
    put_hex_byte(buf + 10, (uint8_t)(g->data2 >> 8));
    put_hex_byte(buf + 12, (uint8_t)g->data2);
    buf[14] = '-';
    put_hex_byte(buf + 15, (uint8_t)(g->data3 >> 8));
    put_hex_byte(buf + 17, (uint8_t)g->data3);
    buf[19] = '-';
    put_hex_byte(buf + 20, g->data4[0]);
    put_hex_byte(buf + 22, g->data4[1]);
    buf[24] = '-';
    for (int i = 0; i < 6; i++) {
        put_hex_byte(buf + 25 + i * 2, g->data4[2 + i]);
    }
    buf[37] = '}';
    buf[38] = '\0';
    return 38;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int twd_guid_from_string(const char *str, twd_guid_t *out) {
    if (!str || !out) return -1;
    if (str[0] == '{') str++;
    if (strlen(str) < 36u || str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') {
        return -1;
    }
    uint8_t raw[16];
    size_t pos = 0;
    for (int i = 0; i < 16; i++) {
        while (str[pos] == '-') pos++;
        int hi = hex_val(str[pos]);
        int lo = hex_val(str[pos + 1]);
        if (hi < 0 || lo < 0) return -1;
        raw[i] = (uint8_t)((hi << 4) | lo);
        pos += 2;
    }
    out->data1 = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
                 ((uint32_t)raw[2] << 8) | (uint32_t)raw[3];
    out->data2 = (uint16_t)(((uint16_t)raw[4] << 8) | raw[5]);
    out->data3 = (uint16_t)(((uint16_t)raw[6] << 8) | raw[7]);
    memcpy(out->data4, raw + 8, 8);
    return 0;
}

struct twd_stream {
    int32_t refs;
    uint8_t *data;
    size_t size;
    size_t cap;
    size_t pos;
};

twd_stream_t *twd_stream_create(const void *init_data, size_t len) {
    twd_stream_t *st = (twd_stream_t *)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->refs = 1;
    if (len > 0u) {
        st->data = (uint8_t *)malloc(len);
        if (!st->data) {
            free(st);
            return NULL;
        }
        st->cap = len;
        st->size = len;
        if (init_data) memcpy(st->data, init_data, len);
        else memset(st->data, 0, len);
    }
    return st;
}

void twd_stream_retain(twd_stream_t *st) {
    if (st) st->refs++;
}

uint32_t twd_stream_release(twd_stream_t *st) {
    if (!st) return 0u;
    int32_t rc = --st->refs;
    if (rc > 0) return (uint32_t)rc;
    free(st->data);
    free(st);
    return 0u;
}

size_t twd_stream_read(twd_stream_t *st, void *dst, size_t len) {
    if (!st || !dst || len == 0u || st->pos >= st->size) return 0u;
    size_t avail = st->size - st->pos;
    size_t n = (len < avail) ? len : avail;
    memcpy(dst, st->data + st->pos, n);
    st->pos += n;
    return n;
}

size_t twd_stream_write(twd_stream_t *st, const void *src, size_t len) {
    if (!st || !src || len == 0u) return 0u;
    size_t need = st->pos + len;
    if (need < st->pos) return 0u;
    if (need > st->cap) {
        size_t new_cap = st->cap ? st->cap * 2u : 256u;
        while (new_cap < need) new_cap *= 2u;
        uint8_t *nb = (uint8_t *)realloc(st->data, new_cap);
        if (!nb) return 0u;
        st->data = nb;
        st->cap = new_cap;
    }
    memcpy(st->data + st->pos, src, len);
    st->pos = need;
    if (need > st->size) st->size = need;
    return len;
}

int64_t twd_stream_seek(twd_stream_t *st, int64_t offset, int whence) {
    if (!st) return -1;
    int64_t base = 0;
    if (whence == TWD_SEEK_CUR) base = (int64_t)st->pos;
    else if (whence == TWD_SEEK_END) base = (int64_t)st->size;
    int64_t np = base + offset;
    if (np < 0) return -1;
    st->pos = (size_t)np;
    return np;
}

size_t twd_stream_size(const twd_stream_t *st) {
    return st ? st->size : 0u;
}

int32_t twd_com_create_instance_stub(const twd_guid_t *clsid, void **out_obj) {
    (void)clsid;
    if (out_obj) *out_obj = NULL;
    return (int32_t)0x80040154u; /* REGDB_E_CLASSNOTREG for PC-only WMI/COM classes */
}

/* ---------- Memory Status, Multi-Wait, File Info & Locale PAL ---------- */

int twd_memory_status(twd_memory_status_t *out) {
    if (!out) return -1;
    uint64_t fallback = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    out->total_phys = fallback;
    out->avail_phys = fallback / 2ULL;
    out->total_virt = 128ULL * 1024ULL * 1024ULL * 1024ULL;
    out->avail_virt = out->total_virt / 2ULL;
    return 0;
}

int twd_wait_any(twd_event_t *const *events, uint32_t count, uint32_t timeout_ms) {
    if (!events || count == 0u) return TWD_WAIT_ERROR;
    uint64_t start_ns = twd_time_now_ns();
    for (;;) {
        for (uint32_t i = 0; i < count; i++) {
            if (events[i] && twd_event_wait(events[i], 0u) == TWD_WAIT_OK) {
                return (int)i;
            }
        }
        if (timeout_ms == 0u) return TWD_WAIT_TIMEOUT;
        if (timeout_ms != TWD_WAIT_INFINITE) {
            uint64_t elapsed_ms = (twd_time_now_ns() - start_ns) / 1000000ULL;
            if (elapsed_ms >= (uint64_t)timeout_ms) return TWD_WAIT_TIMEOUT;
        }
        twd_sleep_ms(1u);
    }
}

int twd_file_info(twd_file_t *f, uint64_t *out_size, uint64_t *out_mtime_ns) {
    if (!f || f->handle < 0) return -1;
    int64_t cur = twd_file_seek(f, 0, TWD_SEEK_CUR);
    int64_t end = twd_file_seek(f, 0, TWD_SEEK_END);
    if (cur >= 0) twd_file_seek(f, cur, TWD_SEEK_SET);
    if (end < 0) return -1;
    if (out_size) *out_size = (uint64_t)end;
    if (out_mtime_ns) *out_mtime_ns = twd_unix_time_ns();
    return 0;
}

uint16_t twd_lang_id(void) {
    return 0x0409u; /* en-US default LCID */
}

/* ---------- Networking / Sockets PAL ---------- */

static int g_net_init_count = 0;

int twd_net_init(void) {
    g_net_init_count++;
    return 0;
}

void twd_net_shutdown(void) {
    if (g_net_init_count > 0) g_net_init_count--;
}

int twd_net_last_error(void) {
    return 0;
}

int twd_net_is_initialized(void) {
    return g_net_init_count > 0 ? 1 : 0;
}

/* ---------- 2D Surface, DPI & Paint Invalidation PAL ---------- */

#define TWD_PAINT_FLAG_DIRTY 0x80000000u

twd_surface_t *twd_surface_create(uint32_t width, uint32_t height) {
    if (width == 0u || height == 0u) return NULL;
    if ((size_t)width > ((size_t)-1) / sizeof(uint32_t) / (size_t)height) return NULL;
    twd_surface_t *s = (twd_surface_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->pixels = (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    s->width = width;
    s->height = height;
    return s;
}

void twd_surface_destroy(twd_surface_t *surf) {
    if (!surf) return;
    free(surf->pixels);
    free(surf);
}

void twd_surface_fill(twd_surface_t *surf, uint32_t argb) {
    if (!surf || !surf->pixels) return;
    size_t count = (size_t)surf->width * (size_t)surf->height;
    for (size_t i = 0; i < count; i++) {
        surf->pixels[i] = argb;
    }
}

int twd_surface_blit(twd_surface_t *dst, uint32_t dx, uint32_t dy,
                     const twd_surface_t *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
    if (!dst || !src || !dst->pixels || !src->pixels) return -1;
    if (dx >= dst->width || dy >= dst->height || sx >= src->width || sy >= src->height) return 0;
    uint32_t cw = w;
    uint32_t ch = h;
    if (cw > dst->width - dx) cw = dst->width - dx;
    if (cw > src->width - sx) cw = src->width - sx;
    if (ch > dst->height - dy) ch = dst->height - dy;
    if (ch > src->height - sy) ch = src->height - sy;
    for (uint32_t row = 0; row < ch; row++) {
        memcpy(dst->pixels + (size_t)(dy + row) * dst->width + dx,
               src->pixels + (size_t)(sy + row) * src->width + sx,
               (size_t)cw * sizeof(uint32_t));
    }
    return 0;
}

void twd_display_dpi(int *out_dpi_x, int *out_dpi_y) {
    if (out_dpi_x) *out_dpi_x = 96;
    if (out_dpi_y) *out_dpi_y = 96;
}

void twd_window_invalidate(twd_window_t *win) {
    if (twd_window_is_valid(win)) {
        win->style |= TWD_PAINT_FLAG_DIRTY;
    }
}

int twd_window_needs_paint(const twd_window_t *win) {
    if (!twd_window_is_valid(win)) return 0;
    return (win->style & TWD_PAINT_FLAG_DIRTY) ? 1 : 0;
}

void twd_window_clear_paint(twd_window_t *win) {
    if (twd_window_is_valid(win)) {
        win->style &= ~TWD_PAINT_FLAG_DIRTY;
    }
}

/* ---------- Android GameActivity Lifecycle, Surface & Touch Bridge ---------- */

static void *g_android_win = NULL;
static void *g_android_assets = NULL;
static char g_android_internal_dir[512];
static char g_android_external_dir[512];
static int g_android_focus = 1;

void twd_android_set_native_window(void *native_window) {
    g_android_win = native_window;
}

void *twd_android_get_native_window(void) {
    return g_android_win;
}

void twd_android_set_asset_manager(void *asset_manager) {
    g_android_assets = asset_manager;
}

void *twd_android_get_asset_manager(void) {
    return g_android_assets;
}

void twd_android_set_data_dirs(const char *internal_dir, const char *external_dir) {
    if (internal_dir) {
        bounded_copy(g_android_internal_dir, sizeof(g_android_internal_dir), internal_dir);
    }
    if (external_dir) {
        bounded_copy(g_android_external_dir, sizeof(g_android_external_dir), external_dir);
    }
    const char *save = (g_android_external_dir[0] != '\0') ? g_android_external_dir : g_android_internal_dir;
    if (save[0] != '\0') {
        twd_setenv("TWD_SAVE_DIR", save);
    }
}

int twd_android_get_data_dir(int external, char *buf, size_t cap) {
    const char *src = external ? g_android_external_dir : g_android_internal_dir;
    if (!src || src[0] == '\0') return -1;
    return copy_path_str(src, buf, cap);
}

void twd_android_on_touch(int action, int pointer_id, float x, float y) {
    uint32_t btn = (action == TWD_TOUCH_DOWN || action == TWD_TOUCH_MOVE) ? 1u : 0u;
    twd_input_set_pointer(x, y, btn);
    twd_win_event_t ev;
    ev.kind = TWD_EVT_POINTER;
    ev.window_id = (uintptr_t)g_android_win;
    ev.param1 = ((uint64_t)(uint32_t)pointer_id << 32) | ((uint64_t)(uint32_t)action & 0xffffffffu);
    int32_t ix = (int32_t)x;
    int32_t iy = (int32_t)y;
    ev.param2 = ((int64_t)ix << 32) | (int64_t)(uint32_t)iy;
    twd_window_push_event(&ev);
}

void twd_android_on_key(int keycode, int down) {
    uint32_t vk = (uint32_t)keycode;
    twd_input_set_key(vk, down);
    twd_win_event_t ev;
    ev.kind = TWD_EVT_KEY;
    ev.window_id = (uintptr_t)g_android_win;
    ev.param1 = (uint64_t)vk;
    ev.param2 = down ? 1 : 0;
    twd_window_push_event(&ev);
}

void twd_android_on_focus(int has_focus) {
    g_android_focus = has_focus ? 1 : 0;
    twd_audio_set_focus(g_android_focus);
}

int twd_android_has_focus(void) {
    return g_android_focus;
}

void twd_android_on_resize(twd_window_t *win, twd_gfx_device_t *dev, uint32_t width, uint32_t height) {
    if (win && width > 0u && height > 0u) {
        twd_window_set_rect(win, win->x, win->y, (int)width, (int)height);
        twd_window_invalidate(win);
    }
    if (dev && width > 0u && height > 0u) {
        twd_gfx_resize(dev, width, height);
    }
    twd_win_event_t ev;
    ev.kind = TWD_EVT_RESIZE;
    ev.window_id = (uintptr_t)win;
    ev.param1 = width;
    ev.param2 = (int64_t)height;
    twd_window_push_event(&ev);
}

static int16_t clamp_axis_i16(float v) {
    if (v <= -1.0f) return -32767;
    if (v >= 1.0f) return 32767;
    return (int16_t)(v * 32767.0f);
}

static uint8_t clamp_trig_u8(float v) {
    if (v <= 0.0f) return 0u;
    if (v >= 1.0f) return 255u;
    return (uint8_t)(v * 255.0f);
}

void twd_android_on_motion_axis(
    uint32_t pad_index, float lx, float ly, float rx, float ry, float lt, float rt, uint16_t buttons) {
    twd_gamepad_state_t st;
    if (twd_input_get_gamepad_state(pad_index, &st) != 0u) {
        memset(&st, 0, sizeof(st));
    }
    st.packet_number++;
    st.buttons = buttons;
    st.thumb_lx = clamp_axis_i16(lx);
    st.thumb_ly = clamp_axis_i16(ly);
    st.thumb_rx = clamp_axis_i16(rx);
    st.thumb_ry = clamp_axis_i16(ry);
    st.left_trigger = clamp_trig_u8(lt);
    st.right_trigger = clamp_trig_u8(rt);
    twd_input_push_gamepad_state(pad_index, &st);
}

static void *read_local_file_alloc(const char *path, size_t *out_size) {
    twd_file_t f;
    if (!path || twd_file_open(path, TWD_FILE_OPEN_READ, &f) != 0) return NULL;
    uint64_t fsz = 0ULL;
    if (twd_file_info(&f, &fsz, NULL) != 0 || fsz > (uint64_t)64u * 1024u * 1024u) {
        twd_file_close(&f);
        return NULL;
    }
    size_t sz = (size_t)fsz;
    uint8_t *buf = (uint8_t *)malloc(sz + 1u);
    if (!buf) {
        twd_file_close(&f);
        return NULL;
    }
    size_t got = 0u;
    while (got < sz) {
        int64_t n = twd_file_read(&f, buf + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    twd_file_close(&f);
    buf[got] = 0u;
    if (out_size) *out_size = got;
    return buf;
}

void *twd_asset_read_alloc(const char *rel_path, size_t *out_size) {
    if (out_size) *out_size = 0u;
    if (!rel_path || rel_path[0] == '\0') return NULL;
    char dir[512];
    char full[768];
    if (twd_app_data_dir(TWD_DIR_SAVES, dir, sizeof(dir)) > 0) {
        size_t dl = strlen(dir);
        size_t rl = strlen(rel_path);
        if (dl + 1u + rl < sizeof(full)) {
            memcpy(full, dir, dl);
            full[dl] = '/';
            memcpy(full + dl + 1u, rel_path, rl + 1u);
            void *p = read_local_file_alloc(full, out_size);
            if (p) return p;
        }
    }
    void *direct = read_local_file_alloc(rel_path, out_size);
    if (direct) return direct;
    void *packed = twd_ttarch2_lookup_mounted_alloc(rel_path, out_size);
    if (packed) return packed;
#ifdef __ANDROID__
    AAssetManager *mgr = (AAssetManager *)twd_android_get_asset_manager();
    if (mgr) {
        AAsset *asset = AAssetManager_open(mgr, rel_path, AASSET_MODE_BUFFER);
        if (asset) {
            off_t len = AAsset_getLength(asset);
            if (len >= 0) {
                size_t sz = (size_t)len;
                uint8_t *buf = (uint8_t *)malloc(sz + 1u);
                if (buf) {
                    int n = (sz > 0u) ? AAsset_read(asset, buf, sz) : 0;
                    if (n >= 0) {
                        buf[(size_t)n] = 0u;
                        if (out_size) *out_size = (size_t)n;
                        AAsset_close(asset);
                        return buf;
                    }
                    free(buf);
                }
            }
            AAsset_close(asset);
        }
    }
#endif
    return NULL;
}

void twd_asset_free(void *ptr) {
    free(ptr);
}

/* ---------- OodleLZ Compression / Archive Block Loader Bridge ---------- */

typedef int64_t (*twd_oodle_decompress_fn)(
    const void *comp_buf, size_t comp_len, void *raw_buf, size_t raw_len,
    int fuzz_safe, int check_crc, int verbosity,
    void *dec_buf_base, size_t dec_buf_size,
    void *fp_cb, void *cb_user, void *scratch, size_t scratch_size, int thread_phase);

static void *g_oodle_lib = NULL;
static twd_oodle_decompress_fn g_oodle_decomp = NULL;

int twd_oodle_init(const char *library_path_or_null) {
    if (g_oodle_lib) return 0;
    const char *candidates[4];
    int n = 0;
    if (library_path_or_null && library_path_or_null[0] != '\0') {
        candidates[n++] = library_path_or_null;
    }
#ifdef _WIN32
    candidates[n++] = "oo2core_5_win64.dll";
    candidates[n++] = "game/oo2core_5_win64.dll";
#else
    candidates[n++] = "liboo2core.so";
    candidates[n++] = "liboo2core_5.so";
#endif
    for (int i = 0; i < n; i++) {
        void *mod = twd_dynlib_open(candidates[i]);
        if (mod) {
            void *sym = twd_dynlib_sym(mod, "OodleLZ_Decompress");
            if (sym) {
                g_oodle_lib = mod;
                g_oodle_decomp = (twd_oodle_decompress_fn)(uintptr_t)sym;
                return 0;
            }
            twd_dynlib_close(mod);
        }
    }
    return -1;
}

void twd_oodle_shutdown(void) {
    if (g_oodle_lib) {
        twd_dynlib_close(g_oodle_lib);
        g_oodle_lib = NULL;
    }
    g_oodle_decomp = NULL;
}

int twd_oodle_is_available(void) {
    return (g_oodle_decomp != NULL) ? 1 : 0;
}

int64_t twd_oodle_decompress(
    const void *comp_buf, size_t comp_len, void *raw_buf, size_t raw_len) {
    if (!comp_buf || !raw_buf || raw_len == 0u) return -1;
    if (g_oodle_decomp) {
        return g_oodle_decomp(
            comp_buf, comp_len, raw_buf, raw_len,
            1, 0, 0, NULL, 0u, NULL, NULL, NULL, 0u, 0);
    }
    /* Pass-through fallback for uncompressed blocks when native library is absent. */
    if (comp_len == raw_len) {
        memcpy(raw_buf, comp_buf, raw_len);
        return (int64_t)raw_len;
    }
    return -1;
}

/* ---------- Video Playback PAL (TTMediaEngine64 replacement) ---------- */

struct twd_video_player {
    void *gfx_device;
    int playing;
    double current_time;
    double duration;
    double volume;
    uint32_t width;
    uint32_t height;
    char source_path[260];
};

twd_video_player_t *twd_video_create(void *gfx_device_or_null) {
    twd_video_player_t *vp = (twd_video_player_t *)calloc(1, sizeof(*vp));
    if (!vp) return NULL;
    vp->gfx_device = gfx_device_or_null;
    vp->volume = 1.0;
    vp->width = 1920u;
    vp->height = 1080u;
    return vp;
}

void twd_video_destroy(twd_video_player_t *vp) {
    free(vp);
}

int twd_video_set_source(twd_video_player_t *vp, const char *path) {
    if (!vp || !path) return -1;
    bounded_copy(vp->source_path, sizeof(vp->source_path), path);
    vp->current_time = 0.0;
    vp->duration = 0.0;
    vp->playing = 0;
    return 0;
}

int twd_video_play(twd_video_player_t *vp) {
    if (!vp) return -1;
    vp->playing = 1;
    return 0;
}

int twd_video_pause(twd_video_player_t *vp) {
    if (!vp) return -1;
    vp->playing = 0;
    return 0;
}

int twd_video_set_volume(twd_video_player_t *vp, double volume) {
    if (!vp) return -1;
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;
    vp->volume = volume;
    return 0;
}

int twd_video_set_time(twd_video_player_t *vp, double seconds) {
    if (!vp || seconds < 0.0) return -1;
    vp->current_time = seconds;
    return 0;
}

double twd_video_get_time(const twd_video_player_t *vp) {
    return vp ? vp->current_time : 0.0;
}

double twd_video_get_duration(const twd_video_player_t *vp) {
    return vp ? vp->duration : 0.0;
}

void twd_video_get_size(const twd_video_player_t *vp, uint32_t *out_w, uint32_t *out_h) {
    if (out_w) *out_w = vp ? vp->width : 0u;
    if (out_h) *out_h = vp ? vp->height : 0u;
}

int twd_video_tick(twd_video_player_t *vp, int64_t *out_pts_ns) {
    if (!vp) return 0;
    if (out_pts_ns) *out_pts_ns = (int64_t)(vp->current_time * 1000000000.0);
    if (vp->playing && vp->current_time >= vp->duration) {
        vp->playing = 0;
        return 0; /* Ready/completed immediately so menu skips unsupported codecs cleanly */
    }
    return vp->playing ? 1 : 0;
}

int twd_video_transfer_frame(twd_video_player_t *vp, void *dst_surface_or_tex) {
    (void)dst_surface_or_tex;
    return vp ? 0 : -1;
}

