/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Persistent Save / Load Manager (saveloadmanager.cpp @ 0x140222410):
 * manages save directory resolution via twd_app_data_dir(TWD_DIR_SAVES),
 * MSV6 PropertySet save serialization ("default.save", slot ".save" files),
 * runtime SaveLoad properties, and corruption detection.
 */
#include "twd_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct twd_save_prop {
    char key[64];
    uint64_t key_crc;
    twd_prop_kind_t kind;
    int b;
    int32_t i32;
    float f32;
    char str[128];
} twd_save_prop_t;

struct twd_save_mgr {
    char save_dir[512];
    char display_name[128];
    char active_scene[128];
    char last_slot[64];
    int32_t episode;
    int32_t chapter;
    int32_t save_seq;
    int is_saving;
    uint32_t prop_count;
    twd_save_prop_t props[TWD_SAVE_MAX_PROPS];
};

static void sv_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1u;
    if (n > 0u) memcpy(dst, src, n);
    dst[n] = '\0';
}

static void sv_wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void sv_wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void sv_wr_u64(uint8_t *p, uint64_t v) {
    sv_wr_u32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    sv_wr_u32(p + 4u, (uint32_t)((v >> 32) & 0xFFFFFFFFULL));
}

static uint16_t sv_rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static uint32_t sv_rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t sv_rd_u64(const uint8_t *p) {
    return (uint64_t)sv_rd_u32(p) | ((uint64_t)sv_rd_u32(p + 4u) << 32u);
}

static int sv_ensure_dir(const char *dir) {
    if (!dir || dir[0] == '\0') return -1;
    if (twd_path_is_dir(dir)) return 0;
    char tmp[512];
    sv_copy(tmp, sizeof(tmp), dir);
    size_t len = strlen(tmp);
    for (size_t i = 1u; i < len; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char ch = tmp[i];
            tmp[i] = '\0';
            if (!twd_path_is_dir(tmp)) (void)twd_mkdir(tmp);
            tmp[i] = ch;
        }
    }
    return twd_path_is_dir(tmp) ? 0 : twd_mkdir(tmp);
}

static void sv_normalize_slot(const char *slot_or_null, char *out_name, size_t cap) {
    const char *raw = (slot_or_null && slot_or_null[0] != '\0') ? slot_or_null : "default.save";
    char clean[64];
    size_t j = 0u;
    for (size_t i = 0u; raw[i] != '\0' && j + 1u < sizeof(clean); ++i) {
        char c = raw[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            if (c == '.' && i > 0u && raw[i - 1u] == '.') continue;
            clean[j++] = c;
        } else {
            clean[j++] = '_';
        }
    }
    clean[j] = '\0';
    if (j == 0u || (j == 1u && clean[0] == '.')) {
        sv_copy(clean, sizeof(clean), "default.save");
        j = strlen(clean);
    }
    int has_ext = (j >= 5u && strcmp(clean + j - 5u, ".save") == 0);
    if (has_ext) {
        sv_copy(out_name, cap, clean);
    } else {
        (void)snprintf(out_name, cap, "%s.save", clean);
    }
}

static int sv_slot_path(
    const twd_save_mgr_t *mgr, const char *slot_or_null,
    char *out_path, size_t cap, char *out_slot, size_t slot_cap) {
    if (!mgr || !out_path || cap == 0u) return -1;
    char slot[64];
    sv_normalize_slot(slot_or_null, slot, sizeof(slot));
    if (out_slot && slot_cap > 0u) sv_copy(out_slot, slot_cap, slot);
    int n = snprintf(out_path, cap, "%s/%s", mgr->save_dir, slot);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int sv_is_reserved_crc(uint64_t crc) {
    return crc == twd_crc64(0ULL, "SaveDisplayName") ||
           crc == twd_crc64(0ULL, "ActiveScene") ||
           crc == twd_crc64(0ULL, "SaveEpisode") ||
           crc == twd_crc64(0ULL, "SaveChapter") ||
           crc == twd_crc64(0ULL, "SaveSeq");
}

static twd_save_prop_t *sv_upsert_prop(twd_save_mgr_t *mgr, const char *key, uint64_t crc_hint) {
    if (!mgr) return NULL;
    uint64_t crc = crc_hint;
    if (key && key[0] != '\0') crc = twd_crc64(0ULL, key);
    if (crc == 0ULL) return NULL;
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (mgr->props[i].key_crc == crc) {
            if (key && key[0] != '\0') sv_copy(mgr->props[i].key, sizeof(mgr->props[i].key), key);
            return &mgr->props[i];
        }
    }
    if (mgr->prop_count >= TWD_SAVE_MAX_PROPS) return NULL;
    twd_save_prop_t *p = &mgr->props[mgr->prop_count++];
    memset(p, 0, sizeof(*p));
    p->key_crc = crc;
    if (key && key[0] != '\0') sv_copy(p->key, sizeof(p->key), key);
    return p;
}

typedef struct sv_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} sv_buf_t;

static int sv_buf_ensure(sv_buf_t *b, size_t add) {
    if (b->len + add <= b->cap) return 0;
    size_t nc = (b->cap == 0u) ? 512u : b->cap;
    while (nc < b->len + add) nc *= 2u;
    uint8_t *nd = (uint8_t *)realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd;
    b->cap = nc;
    return 0;
}

static int sv_buf_put(sv_buf_t *b, const void *src, size_t n) {
    if (sv_buf_ensure(b, n) != 0) return -1;
    if (n > 0u) memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int sv_buf_put_u16(sv_buf_t *b, uint16_t v) {
    uint8_t tmp[2];
    sv_wr_u16(tmp, v);
    return sv_buf_put(b, tmp, 2u);
}

static int sv_buf_put_u32(sv_buf_t *b, uint32_t v) {
    uint8_t tmp[4];
    sv_wr_u32(tmp, v);
    return sv_buf_put(b, tmp, 4u);
}

static int sv_buf_put_u64(sv_buf_t *b, uint64_t v) {
    uint8_t tmp[8];
    sv_wr_u64(tmp, v);
    return sv_buf_put(b, tmp, 8u);
}

static int sv_buf_put_str_val(sv_buf_t *b, const char *s) {
    size_t n = s ? strlen(s) : 0u;
    if (sv_buf_put_u32(b, (uint32_t)n) != 0) return -1;
    return (n > 0u) ? sv_buf_put(b, s, n) : 0;
}

twd_save_mgr_t *twd_save_mgr_create(const char *save_dir_or_null) {
    twd_save_mgr_t *mgr = (twd_save_mgr_t *)calloc(1u, sizeof(*mgr));
    if (!mgr) return NULL;
    if (save_dir_or_null && save_dir_or_null[0] != '\0') {
        sv_copy(mgr->save_dir, sizeof(mgr->save_dir), save_dir_or_null);
    } else if (twd_app_data_dir(TWD_DIR_SAVES, mgr->save_dir, sizeof(mgr->save_dir)) <= 0) {
        sv_copy(mgr->save_dir, sizeof(mgr->save_dir), ".");
    }
    (void)sv_ensure_dir(mgr->save_dir);
    sv_copy(mgr->display_name, sizeof(mgr->display_name), "Default Save");
    mgr->episode = 1;
    mgr->chapter = 1;
    return mgr;
}

void twd_save_mgr_destroy(twd_save_mgr_t *mgr) {
    free(mgr);
}

int twd_save_mgr_get_dir(const twd_save_mgr_t *mgr, char *buf, size_t cap) {
    if (!mgr || !buf || cap == 0u) return -1;
    sv_copy(buf, cap, mgr->save_dir);
    return (int)strlen(buf);
}

void twd_save_clear_props(twd_save_mgr_t *mgr) {
    if (!mgr) return;
    mgr->prop_count = 0u;
    memset(mgr->props, 0, sizeof(mgr->props));
}

int twd_save_set_display_name(twd_save_mgr_t *mgr, const char *display_name) {
    if (!mgr || !display_name) return -1;
    sv_copy(mgr->display_name, sizeof(mgr->display_name), display_name);
    return 0;
}

int twd_save_set_active_scene(twd_save_mgr_t *mgr, const char *scene_name) {
    if (!mgr || !scene_name) return -1;
    sv_copy(mgr->active_scene, sizeof(mgr->active_scene), scene_name);
    return 0;
}

int twd_save_set_prop_bool(twd_save_mgr_t *mgr, const char *key, int val) {
    twd_save_prop_t *p = sv_upsert_prop(mgr, key, 0ULL);
    if (!p) return -1;
    p->kind = TWD_PROP_BOOL;
    p->b = val ? 1 : 0;
    return 0;
}

int twd_save_set_prop_i32(twd_save_mgr_t *mgr, const char *key, int32_t val) {
    twd_save_prop_t *p = sv_upsert_prop(mgr, key, 0ULL);
    if (!p) return -1;
    p->kind = TWD_PROP_I32;
    p->i32 = val;
    return 0;
}

int twd_save_set_prop_f32(twd_save_mgr_t *mgr, const char *key, float val) {
    twd_save_prop_t *p = sv_upsert_prop(mgr, key, 0ULL);
    if (!p) return -1;
    p->kind = TWD_PROP_F32;
    p->f32 = val;
    return 0;
}

int twd_save_set_prop_str(twd_save_mgr_t *mgr, const char *key, const char *val) {
    twd_save_prop_t *p = sv_upsert_prop(mgr, key, 0ULL);
    if (!p || !val) return -1;
    p->kind = TWD_PROP_STR;
    sv_copy(p->str, sizeof(p->str), val);
    return 0;
}

int twd_save_get_prop_bool(const twd_save_mgr_t *mgr, const char *key, int default_val) {
    if (!mgr || !key) return default_val;
    uint64_t crc = twd_crc64(0ULL, key);
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (mgr->props[i].key_crc == crc && mgr->props[i].kind == TWD_PROP_BOOL) {
            return mgr->props[i].b;
        }
    }
    return default_val;
}

int32_t twd_save_get_prop_i32(const twd_save_mgr_t *mgr, const char *key, int32_t default_val) {
    if (!mgr || !key) return default_val;
    uint64_t crc = twd_crc64(0ULL, key);
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (mgr->props[i].key_crc == crc && mgr->props[i].kind == TWD_PROP_I32) {
            return mgr->props[i].i32;
        }
    }
    return default_val;
}

float twd_save_get_prop_f32(const twd_save_mgr_t *mgr, const char *key, float default_val) {
    if (!mgr || !key) return default_val;
    uint64_t crc = twd_crc64(0ULL, key);
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (mgr->props[i].key_crc == crc && mgr->props[i].kind == TWD_PROP_F32) {
            return mgr->props[i].f32;
        }
    }
    return default_val;
}

const char *twd_save_get_prop_str(const twd_save_mgr_t *mgr, const char *key, const char *default_val) {
    if (!mgr || !key) return default_val;
    uint64_t crc = twd_crc64(0ULL, key);
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (mgr->props[i].key_crc == crc && mgr->props[i].kind == TWD_PROP_STR) {
            return mgr->props[i].str;
        }
    }
    return default_val;
}

int twd_save_game(twd_save_mgr_t *mgr, const char *slot_or_null) {
    if (!mgr) return -1;
    char path[600];
    char slot[64];
    if (sv_slot_path(mgr, slot_or_null, path, sizeof(path), slot, sizeof(slot)) != 0) return -1;
    if (sv_ensure_dir(mgr->save_dir) != 0) return -1;

    mgr->is_saving = 1;
    mgr->save_seq++;

    uint32_t cnt_bool = 0u, cnt_i32 = 3u, cnt_f32 = 0u, cnt_str = 2u;
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (sv_is_reserved_crc(mgr->props[i].key_crc)) continue;
        if (mgr->props[i].kind == TWD_PROP_BOOL) cnt_bool++;
        else if (mgr->props[i].kind == TWD_PROP_I32) cnt_i32++;
        else if (mgr->props[i].kind == TWD_PROP_F32) cnt_f32++;
        else if (mgr->props[i].kind == TWD_PROP_STR) cnt_str++;
    }
    uint32_t num_groups = 2u + (cnt_bool > 0u ? 1u : 0u) + (cnt_f32 > 0u ? 1u : 0u);

    sv_buf_t b;
    memset(&b, 0, sizeof(b));
    int err = 0;
    err |= sv_buf_put(&b, "6VSM", 4u);
    size_t def_sz_off = b.len; err |= sv_buf_put_u32(&b, 0u);
    size_t dbg_sz_off = b.len; err |= sv_buf_put_u32(&b, 0u);
    err |= sv_buf_put_u32(&b, 0u); /* async_size */
    err |= sv_buf_put_u32(&b, 0u); /* type_count */

    size_t prop_start = b.len;
    err |= sv_buf_put_u32(&b, 2u); /* mPropVersion */
    err |= sv_buf_put_u32(&b, 0u); /* mPropertyFlags */
    size_t blk_sz_off = b.len; err |= sv_buf_put_u32(&b, 0u);
    size_t blk_start = b.len;
    err |= sv_buf_put_u32(&b, 0u); /* parent_count */
    err |= sv_buf_put_u32(&b, num_groups);

    /* String group: metadata + string properties */
    err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "String"));
    err |= sv_buf_put_u32(&b, cnt_str);
    err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "SaveDisplayName"));
    err |= sv_buf_put_str_val(&b, mgr->display_name);
    err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "ActiveScene"));
    err |= sv_buf_put_str_val(&b, mgr->active_scene);
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (!sv_is_reserved_crc(mgr->props[i].key_crc) && mgr->props[i].kind == TWD_PROP_STR) {
            err |= sv_buf_put_u64(&b, mgr->props[i].key_crc);
            err |= sv_buf_put_str_val(&b, mgr->props[i].str);
        }
    }

    /* int32 group: metadata + i32 properties */
    err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "int32"));
    err |= sv_buf_put_u32(&b, cnt_i32);
    err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "SaveEpisode"));
    err |= sv_buf_put_u32(&b, (uint32_t)mgr->episode);
    err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "SaveChapter"));
    err |= sv_buf_put_u32(&b, (uint32_t)mgr->chapter);
    err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "SaveSeq"));
    err |= sv_buf_put_u32(&b, (uint32_t)mgr->save_seq);
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (!sv_is_reserved_crc(mgr->props[i].key_crc) && mgr->props[i].kind == TWD_PROP_I32) {
            err |= sv_buf_put_u64(&b, mgr->props[i].key_crc);
            err |= sv_buf_put_u32(&b, (uint32_t)mgr->props[i].i32);
        }
    }

    if (cnt_bool > 0u) {
        err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "bool"));
        err |= sv_buf_put_u32(&b, cnt_bool);
        for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
            if (!sv_is_reserved_crc(mgr->props[i].key_crc) && mgr->props[i].kind == TWD_PROP_BOOL) {
                uint8_t bv = (uint8_t)(mgr->props[i].b ? 1u : 0u);
                err |= sv_buf_put_u64(&b, mgr->props[i].key_crc);
                err |= sv_buf_put(&b, &bv, 1u);
            }
        }
    }

    if (cnt_f32 > 0u) {
        err |= sv_buf_put_u64(&b, twd_crc64(0ULL, "float"));
        err |= sv_buf_put_u32(&b, cnt_f32);
        for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
            if (!sv_is_reserved_crc(mgr->props[i].key_crc) && mgr->props[i].kind == TWD_PROP_F32) {
                uint32_t bits = 0u;
                memcpy(&bits, &mgr->props[i].f32, 4u);
                err |= sv_buf_put_u64(&b, mgr->props[i].key_crc);
                err |= sv_buf_put_u32(&b, bits);
            }
        }
    }

    if (err == 0) {
        sv_wr_u32(b.data + blk_sz_off, (uint32_t)(b.len - blk_start));
        sv_wr_u32(b.data + def_sz_off, (uint32_t)(b.len - prop_start));
    }

    /* Debug section: "SAVK" key name table so loaded runtime props preserve string names */
    size_t dbg_start = b.len;
    uint32_t named_cnt = 0u;
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (mgr->props[i].key[0] != '\0') named_cnt++;
    }
    err |= sv_buf_put(&b, "SAVK", 4u);
    err |= sv_buf_put_u32(&b, named_cnt);
    for (uint32_t i = 0u; i < mgr->prop_count; ++i) {
        if (mgr->props[i].key[0] != '\0') {
            uint16_t klen = (uint16_t)strlen(mgr->props[i].key);
            err |= sv_buf_put_u64(&b, mgr->props[i].key_crc);
            err |= sv_buf_put_u16(&b, klen);
            err |= sv_buf_put(&b, mgr->props[i].key, klen);
        }
    }
    if (err == 0) {
        sv_wr_u32(b.data + dbg_sz_off, (uint32_t)(b.len - dbg_start));
    }

    int rc = -1;
    if (err == 0) {
        twd_file_t f;
        if (twd_file_open(path, TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &f) == 0) {
            int64_t wr = twd_file_write(&f, b.data, b.len);
            (void)twd_file_flush(&f);
            int cl = twd_file_close(&f);
            if (wr == (int64_t)b.len && cl == 0) {
                sv_copy(mgr->last_slot, sizeof(mgr->last_slot), slot);
                rc = 0;
            }
        }
    }
    free(b.data);
    mgr->is_saving = 0;
    return rc;
}

static int sv_read_and_validate(const char *path, uint8_t **out_buf, size_t *out_sz,
                                twd_metastream_header_t *out_mh, twd_propset_t *out_ps) {
    twd_file_t f;
    if (!path || twd_file_open(path, TWD_FILE_OPEN_READ, &f) != 0) return -1;
    uint64_t fsz = 0ULL;
    if (twd_file_info(&f, &fsz, NULL) != 0 || fsz < 20ULL || fsz > 16ULL * 1024ULL * 1024ULL) {
        (void)twd_file_close(&f);
        return -1;
    }
    size_t sz = (size_t)fsz;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) {
        (void)twd_file_close(&f);
        return -1;
    }
    size_t got = 0u;
    while (got < sz) {
        int64_t n = twd_file_read(&f, buf + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    (void)twd_file_close(&f);
    if (got != sz) {
        free(buf);
        return -1;
    }

    twd_metastream_header_t mh;
    memset(&mh, 0, sizeof(mh));
    if (twd_metastream_parse_header(buf, sz, &mh) != 0 ||
        (uint64_t)mh.payload_offset + (uint64_t)mh.default_size + (uint64_t)mh.debug_size > (uint64_t)sz) {
        free(buf);
        return -1;
    }

    twd_propset_t ps;
    memset(&ps, 0, sizeof(ps));
    if (twd_propset_parse_memory(buf, sz, &ps) != 0 || ps.truncated) {
        twd_propset_free(&ps);
        free(buf);
        return -1;
    }
    *out_buf = buf;
    *out_sz = sz;
    *out_mh = mh;
    *out_ps = ps;
    return 0;
}

int twd_load_game(twd_save_mgr_t *mgr, const char *slot_or_null) {
    if (!mgr) return -1;
    char path[600];
    char slot[64];
    if (sv_slot_path(mgr, slot_or_null, path, sizeof(path), slot, sizeof(slot)) != 0) return -1;

    uint8_t *buf = NULL;
    size_t sz = 0u;
    twd_metastream_header_t mh;
    twd_propset_t ps;
    if (sv_read_and_validate(path, &buf, &sz, &mh, &ps) != 0) return -1;

    twd_save_clear_props(mgr);
    const char *dname = twd_propset_get_str(&ps, "SaveDisplayName", NULL);
    if (dname) sv_copy(mgr->display_name, sizeof(mgr->display_name), dname);
    const char *scene = twd_propset_get_str(&ps, "ActiveScene", NULL);
    if (scene) sv_copy(mgr->active_scene, sizeof(mgr->active_scene), scene);
    mgr->episode = twd_propset_get_i32(&ps, "SaveEpisode", mgr->episode);
    mgr->chapter = twd_propset_get_i32(&ps, "SaveChapter", mgr->chapter);
    mgr->save_seq = twd_propset_get_i32(&ps, "SaveSeq", mgr->save_seq);

    for (uint32_t i = 0u; i < ps.entry_count; ++i) {
        const twd_prop_entry_t *e = &ps.entries[i];
        if (sv_is_reserved_crc(e->key_crc)) continue;
        twd_save_prop_t *dst = sv_upsert_prop(mgr, NULL, e->key_crc);
        if (!dst) continue;
        dst->kind = e->kind;
        if (e->kind == TWD_PROP_BOOL) dst->b = e->val.b;
        else if (e->kind == TWD_PROP_I32) dst->i32 = e->val.i32;
        else if (e->kind == TWD_PROP_F32) dst->f32 = e->val.f32;
        else if (e->kind == TWD_PROP_STR) sv_copy(dst->str, sizeof(dst->str), e->val.str);
    }

    size_t dbg_off = (size_t)mh.payload_offset + (size_t)mh.default_size;
    size_t dbg_end = dbg_off + (size_t)mh.debug_size;
    if (mh.debug_size >= 8u && dbg_end <= sz && memcmp(buf + dbg_off, "SAVK", 4u) == 0) {
        uint32_t kcnt = sv_rd_u32(buf + dbg_off + 4u);
        size_t pos = dbg_off + 8u;
        for (uint32_t ki = 0u; ki < kcnt && pos + 10u <= dbg_end; ++ki) {
            uint64_t kcrc = sv_rd_u64(buf + pos);
            uint16_t klen = sv_rd_u16(buf + pos + 8u);
            pos += 10u;
            if ((size_t)klen > dbg_end - pos) break;
            for (uint32_t pi = 0u; pi < mgr->prop_count; ++pi) {
                if (mgr->props[pi].key_crc == kcrc) {
                    size_t cp = ((size_t)klen < sizeof(mgr->props[pi].key) - 1u)
                        ? (size_t)klen : (sizeof(mgr->props[pi].key) - 1u);
                    memcpy(mgr->props[pi].key, buf + pos, cp);
                    mgr->props[pi].key[cp] = '\0';
                    break;
                }
            }
            pos += (size_t)klen;
        }
    }

    twd_propset_free(&ps);
    free(buf);
    sv_copy(mgr->last_slot, sizeof(mgr->last_slot), slot);
    return 0;
}

int twd_save_exists(const twd_save_mgr_t *mgr, const char *slot_or_null) {
    char path[600];
    if (sv_slot_path(mgr, slot_or_null, path, sizeof(path), NULL, 0u) != 0) return 0;
    return twd_file_exists(path) ? 1 : 0;
}

int twd_save_is_corrupt(const twd_save_mgr_t *mgr, const char *slot_or_null) {
    char path[600];
    if (sv_slot_path(mgr, slot_or_null, path, sizeof(path), NULL, 0u) != 0) return 1;
    if (!twd_file_exists(path)) return 0;
    uint8_t *buf = NULL;
    size_t sz = 0u;
    twd_metastream_header_t mh;
    twd_propset_t ps;
    if (sv_read_and_validate(path, &buf, &sz, &mh, &ps) != 0) return 1;
    twd_propset_free(&ps);
    free(buf);
    return 0;
}

int twd_save_delete(twd_save_mgr_t *mgr, const char *slot_or_null) {
    char path[600];
    if (sv_slot_path(mgr, slot_or_null, path, sizeof(path), NULL, 0u) != 0) return -1;
    return twd_file_delete(path);
}

int twd_save_new_game(twd_save_mgr_t *mgr, uint32_t slot_index) {
    if (!mgr) return -1;
    twd_save_clear_props(mgr);
    mgr->episode = 1;
    mgr->chapter = 1;
    mgr->active_scene[0] = '\0';
    char slot[64];
    (void)snprintf(slot, sizeof(slot), "slot%u.save", slot_index);
    (void)snprintf(mgr->display_name, sizeof(mgr->display_name), "Save Slot %u", slot_index);
    sv_copy(mgr->last_slot, sizeof(mgr->last_slot), slot);
    return 0;
}

int twd_save_reset_game(twd_save_mgr_t *mgr) {
    if (!mgr) return -1;
    twd_save_clear_props(mgr);
    mgr->episode = 1;
    mgr->chapter = 1;
    mgr->active_scene[0] = '\0';
    mgr->save_seq = 0;
    sv_copy(mgr->display_name, sizeof(mgr->display_name), "Default Save");
    return 0;
}

int twd_save_is_saving(const twd_save_mgr_t *mgr) {
    return mgr ? mgr->is_saving : 0;
}

void twd_save_get_metadata(
    const twd_save_mgr_t *mgr,
    char *out_slot, size_t slot_cap,
    char *out_display, size_t disp_cap,
    char *out_scene, size_t scene_cap,
    int32_t *out_seq) {
    if (!mgr) return;
    if (out_slot && slot_cap > 0u) sv_copy(out_slot, slot_cap, mgr->last_slot);
    if (out_display && disp_cap > 0u) sv_copy(out_display, disp_cap, mgr->display_name);
    if (out_scene && scene_cap > 0u) sv_copy(out_scene, scene_cap, mgr->active_scene);
    if (out_seq) *out_seq = mgr->save_seq;
}
