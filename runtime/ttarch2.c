/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * TTArchive2 (.ttarch2) container, chunked Oodle/raw decompression, TTA3/TTA4
 * TOC parser, and mounted asset lookup for the portable TWD runtime.
 *
 * Binary layout verified against WalkingDead4.exe (DataStreamContainer
 * 0x1402b3c10 / 0x1402c1130 and WD4 .ttarch2 archives):
 *   Container header:
 *     - "NCTT" (0x5454434E): u64 payload_size; payload starts at +0x0C.
 *     - "ZCTT" (0x5454435A) / "ECTT" (0x54544345):
 *         u32 chunk_size, u32 chunk_count, u64 chunk_offsets[chunk_count + 1].
 *         Chunk i occupies [chunk_offsets[i], chunk_offsets[i + 1]).
 *     - "zCTT" (0x5454437A) / "eCTT" (0x54544365):
 *         u32 compress_mode, u32 chunk_size, u32 chunk_count,
 *         u64 chunk_offsets[chunk_count + 1].
 *   Decompressed TOC (at logical offset 0):
 *     - u32 magic ("4ATT" = 0x54544134 or "3ATT" = 0x54544133)
 *     - u32 names_size
 *     - u32 entry_count
 *     - entry_count * 28-byte records:
 *         u64 name_crc64, u64 offset (from data_base), u32 size, u32 flags,
 *         u16 name_page, u16 name_offset
 *     - names_size bytes of NUL-terminated filenames
 *     - entry data starting at data_base = 12 + entry_count * 28 + names_size
 */
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

#define TWD_TTARCH2_MAX_CHUNKS  0x100000u
#define TWD_TTARCH2_MAX_CHUNK_SZ 0x100000u
#define TWD_TTARCH2_MAX_ENTRIES 250000u
#define TWD_TTARCH2_MAX_NAMES   (64u * 1024u * 1024u)
#define TWD_TTARCH2_MOUNT_MAX   64u

struct twd_ttarch2_record {
    uint64_t name_crc64;
    uint64_t offset;
    uint32_t size;
    uint32_t flags;
    uint32_t name_pos;
};

struct twd_ttarch2_archive {
    twd_ttarch2_header_t hdr;
    int has_file;
    twd_file_t file;
    uint8_t *mem_copy;
    size_t mem_len;

    uint64_t *chunk_offsets; /* chunk_count + 1 entries for ZCTT/ECTT */
    uint8_t *chunk_raw;      /* cached decompressed chunk */
    uint32_t cached_chunk;
    uint32_t cached_raw_len;

    uint32_t toc_magic;
    uint32_t names_size;
    uint32_t entry_count;
    uint64_t data_base;
    uint64_t max_logical_end;
    struct twd_ttarch2_record *entries;
    char *names;
};

static twd_ttarch2_decrypt_fn g_decrypt_fn = NULL;
static void *g_decrypt_user = NULL;
static twd_ttarch2_archive_t *g_mounted[TWD_TTARCH2_MOUNT_MAX];
static uint8_t g_mounted_owned[TWD_TTARCH2_MOUNT_MAX];
static size_t g_mounted_count = 0u;

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p) {
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4u) << 32);
}

static char ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    if (c == '\\') return '/';
    return c;
}

static int path_eq_icase(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) return 0;
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

void twd_ttarch2_set_decrypt_callback(twd_ttarch2_decrypt_fn fn, void *user) {
    g_decrypt_fn = fn;
    g_decrypt_user = user;
}

int twd_ttarch2_parse_header(const void *data, size_t len, twd_ttarch2_header_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!data || len < 12u) return -1;

    const uint8_t *b = (const uint8_t *)data;
    uint32_t magic = rd_u32(b);
    if (magic == 0x5454434Eu) { /* "NCTT" */
        out->container_kind = TWD_TTARCH2_NCTT;
        out->encrypted = 0;
        out->compressed = 0;
        out->payload_size = rd_u64(b + 4u);
        out->header_size = 12u;
        return 0;
    }

    int is_lower = (magic == 0x5454437Au || magic == 0x54544365u); /* "zCTT" / "eCTT" */
    int is_zctt = (magic == 0x5454435Au || magic == 0x5454437Au);
    int is_ectt = (magic == 0x54544345u || magic == 0x54544365u);
    if (!is_zctt && !is_ectt) return -1;

    if (is_lower && len < 16u) return -1;
    size_t off = is_lower ? 8u : 4u;
    out->container_kind = is_ectt ? TWD_TTARCH2_ECTT : TWD_TTARCH2_ZCTT;
    out->encrypted = is_ectt ? 1 : 0;
    out->compressed = 1;
    out->compress_mode = is_lower ? rd_u32(b + 4u) : 0u;
    out->chunk_size = rd_u32(b + off);
    out->chunk_count = rd_u32(b + off + 4u);
    out->version = out->compress_mode;

    uint64_t table_base = is_lower ? 16u : 12u;
    if (out->chunk_count <= TWD_TTARCH2_MAX_CHUNKS) {
        out->header_size = table_base + ((uint64_t)out->chunk_count + 1u) * 8u;
    }
    if (out->chunk_count > 0u && out->chunk_count <= TWD_TTARCH2_MAX_CHUNKS &&
        len >= (size_t)out->header_size) {
        uint64_t first_off = rd_u64(b + (size_t)table_base);
        uint64_t last_off = rd_u64(b + (size_t)table_base + (size_t)out->chunk_count * 8u);
        out->payload_size = (last_off >= first_off) ? (last_off - first_off) : 0u;
    } else {
        out->payload_size = (uint64_t)out->chunk_size;
    }
    return 0;
}

static int raw_read_at(twd_ttarch2_archive_t *arch, uint64_t file_off, void *dst, size_t len) {
    if (len == 0u) return 0;
    if (arch->mem_copy) {
        if (file_off > (uint64_t)arch->mem_len ||
            (uint64_t)len > (uint64_t)arch->mem_len - file_off) {
            return -1;
        }
        memcpy(dst, arch->mem_copy + (size_t)file_off, len);
        return 0;
    }
    if (!arch->has_file) return -1;
    if (twd_file_seek(&arch->file, (int64_t)file_off, 0 /* SEEK_SET */) < 0) return -1;
    size_t got = 0u;
    uint8_t *p = (uint8_t *)dst;
    while (got < len) {
        int64_t r = twd_file_read(&arch->file, p + got, len - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int ensure_chunk(twd_ttarch2_archive_t *arch, uint32_t chunk_idx) {
    if (chunk_idx >= arch->hdr.chunk_count) return -1;
    if (arch->cached_chunk == chunk_idx && arch->chunk_raw != NULL) return 0;

    uint64_t c0 = arch->chunk_offsets[chunk_idx];
    uint64_t c1 = arch->chunk_offsets[chunk_idx + 1u];
    if (c1 < c0) return -1;
    uint64_t comp_len_u64 = c1 - c0;
    if (comp_len_u64 == 0u || comp_len_u64 > (uint64_t)arch->hdr.chunk_size * 2u + 4096u) {
        return -1;
    }
    size_t comp_len = (size_t)comp_len_u64;

    uint8_t *comp = (uint8_t *)malloc(comp_len);
    if (!comp) return -1;
    if (raw_read_at(arch, c0, comp, comp_len) != 0) {
        free(comp);
        return -1;
    }

    if (arch->hdr.encrypted) {
        if (!g_decrypt_fn || g_decrypt_fn(chunk_idx, comp, comp_len, g_decrypt_user) != 0) {
            free(comp);
            return -2;
        }
    }

    uint32_t target_raw = arch->hdr.chunk_size;
    int is_last = (chunk_idx + 1u == arch->hdr.chunk_count);
    if (is_last) {
        uint64_t chunk_start = (uint64_t)chunk_idx * (uint64_t)arch->hdr.chunk_size;
        if (arch->max_logical_end > chunk_start) {
            uint64_t rem = arch->max_logical_end - chunk_start;
            if (rem <= (uint64_t)arch->hdr.chunk_size) target_raw = (uint32_t)rem;
        } else if (!twd_oodle_is_available() && comp_len <= (size_t)arch->hdr.chunk_size) {
            target_raw = (uint32_t)comp_len;
        }
    }

    if (!arch->chunk_raw) {
        arch->chunk_raw = (uint8_t *)malloc((size_t)arch->hdr.chunk_size);
        if (!arch->chunk_raw) {
            free(comp);
            return -1;
        }
    }

    int64_t dec = twd_oodle_decompress(comp, comp_len, arch->chunk_raw, (size_t)target_raw);
    if (dec < 0 && is_last && target_raw != arch->hdr.chunk_size) {
        dec = twd_oodle_decompress(comp, comp_len, arch->chunk_raw, (size_t)arch->hdr.chunk_size);
    }
    free(comp);
    if (dec <= 0 || dec > (int64_t)arch->hdr.chunk_size) return -1;

    arch->cached_chunk = chunk_idx;
    arch->cached_raw_len = (uint32_t)dec;
    return 0;
}

static int stream_read(twd_ttarch2_archive_t *arch, uint64_t logical_off, void *dst, size_t len) {
    if (len == 0u) return 0;
    if (arch->hdr.container_kind == TWD_TTARCH2_NCTT) {
        if (logical_off > arch->hdr.payload_size ||
            (uint64_t)len > arch->hdr.payload_size - logical_off) {
            return -1;
        }
        return raw_read_at(arch, arch->hdr.header_size + logical_off, dst, len);
    }

    uint32_t csz = arch->hdr.chunk_size;
    if (csz == 0u) return -1;
    uint8_t *out = (uint8_t *)dst;
    size_t pos = 0u;
    while (pos < len) {
        uint64_t cur = logical_off + (uint64_t)pos;
        uint64_t idx64 = cur / (uint64_t)csz;
        if (idx64 >= (uint64_t)arch->hdr.chunk_count) return -1;
        int rc = ensure_chunk(arch, (uint32_t)idx64);
        if (rc != 0) return rc;

        uint32_t in_chunk = (uint32_t)(cur % (uint64_t)csz);
        if (in_chunk >= arch->cached_raw_len) return -1;
        uint32_t avail = arch->cached_raw_len - in_chunk;
        size_t take = len - pos;
        if (take > (size_t)avail) take = (size_t)avail;
        memcpy(out + pos, arch->chunk_raw + in_chunk, take);
        pos += take;
    }
    return 0;
}

static int parse_toc(twd_ttarch2_archive_t *arch) {
    uint8_t toc_hdr[12];
    int rc = stream_read(arch, 0u, toc_hdr, sizeof(toc_hdr));
    if (rc != 0) return rc;

    arch->toc_magic = rd_u32(toc_hdr);
    if (arch->toc_magic != TWD_TTARCH2_TOC_TTA4 &&
        arch->toc_magic != TWD_TTARCH2_TOC_TTA3) {
        return -1;
    }

    arch->names_size = rd_u32(toc_hdr + 4u);
    arch->entry_count = rd_u32(toc_hdr + 8u);
    if (arch->entry_count > TWD_TTARCH2_MAX_ENTRIES ||
        arch->names_size > TWD_TTARCH2_MAX_NAMES) {
        return -1;
    }

    uint64_t rec_bytes_u64 = (uint64_t)arch->entry_count * 28u;
    arch->data_base = 12u + rec_bytes_u64 + (uint64_t)arch->names_size;
    arch->max_logical_end = arch->data_base;

    if (arch->entry_count > 0u) {
        size_t rec_bytes = (size_t)rec_bytes_u64;
        uint8_t *raw_rec = (uint8_t *)malloc(rec_bytes);
        arch->entries = (struct twd_ttarch2_record *)calloc(
            (size_t)arch->entry_count, sizeof(struct twd_ttarch2_record));
        if (!raw_rec || !arch->entries) {
            free(raw_rec);
            return -1;
        }
        rc = stream_read(arch, 12u, raw_rec, rec_bytes);
        if (rc != 0) {
            free(raw_rec);
            return rc;
        }
        for (uint32_t i = 0u; i < arch->entry_count; ++i) {
            const uint8_t *p = raw_rec + (size_t)i * 28u;
            struct twd_ttarch2_record *e = &arch->entries[i];
            e->name_crc64 = rd_u64(p);
            e->offset = rd_u64(p + 8u);
            e->size = rd_u32(p + 16u);
            e->flags = rd_u32(p + 20u);
            uint16_t page = rd_u16(p + 24u);
            uint16_t noff = rd_u16(p + 26u);
            e->name_pos = ((uint32_t)page << 16) | (uint32_t)noff;
            uint64_t end = arch->data_base + e->offset + (uint64_t)e->size;
            if (end > arch->max_logical_end) arch->max_logical_end = end;
        }
        free(raw_rec);
    }

    if (arch->names_size > 0u) {
        arch->names = (char *)malloc((size_t)arch->names_size + 1u);
        if (!arch->names) return -1;
        rc = stream_read(arch, 12u + rec_bytes_u64, arch->names, (size_t)arch->names_size);
        if (rc != 0) return rc;
        arch->names[arch->names_size] = '\0';
    }
    return 0;
}

static int init_container(twd_ttarch2_archive_t *arch) {
    uint8_t probe[16];
    memset(probe, 0, sizeof(probe));
    size_t probe_len = 12u;
    uint8_t tmp4[4];
    if (raw_read_at(arch, 0u, tmp4, 4u) != 0) return -1;
    uint32_t m = rd_u32(tmp4);
    if (m == 0x5454437Au || m == 0x54544365u) probe_len = 16u;
    if (raw_read_at(arch, 0u, probe, probe_len) != 0) return -1;

    if (twd_ttarch2_parse_header(probe, probe_len, &arch->hdr) != 0) return -1;
    arch->cached_chunk = UINT32_MAX;

    if (arch->hdr.container_kind != TWD_TTARCH2_NCTT) {
        if (arch->hdr.chunk_size == 0u || arch->hdr.chunk_size > TWD_TTARCH2_MAX_CHUNK_SZ ||
            arch->hdr.chunk_count == 0u || arch->hdr.chunk_count > TWD_TTARCH2_MAX_CHUNKS) {
            return -1;
        }
        uint32_t n_off = arch->hdr.chunk_count + 1u;
        size_t tbl_bytes = (size_t)n_off * 8u;
        uint8_t *raw_tbl = (uint8_t *)malloc(tbl_bytes);
        arch->chunk_offsets = (uint64_t *)calloc((size_t)n_off, sizeof(uint64_t));
        if (!raw_tbl || !arch->chunk_offsets) {
            free(raw_tbl);
            return -1;
        }
        uint64_t table_off = (m == 0x5454437Au || m == 0x54544365u) ? 16u : 12u;
        if (raw_read_at(arch, table_off, raw_tbl, tbl_bytes) != 0) {
            free(raw_tbl);
            return -1;
        }
        for (uint32_t i = 0u; i < n_off; ++i) {
            arch->chunk_offsets[i] = rd_u64(raw_tbl + (size_t)i * 8u);
        }
        free(raw_tbl);
        arch->hdr.payload_size =
            arch->chunk_offsets[arch->hdr.chunk_count] - arch->chunk_offsets[0];
    }

    return parse_toc(arch);
}

twd_ttarch2_archive_t *twd_ttarch2_open(const char *path) {
    if (!path || path[0] == '\0') return NULL;
    twd_ttarch2_archive_t *arch =
        (twd_ttarch2_archive_t *)calloc(1u, sizeof(twd_ttarch2_archive_t));
    if (!arch) return NULL;
    if (twd_file_open(path, TWD_FILE_OPEN_READ, &arch->file) != 0) {
        free(arch);
        return NULL;
    }
    arch->has_file = 1;
    if (init_container(arch) != 0) {
        twd_ttarch2_close(arch);
        return NULL;
    }
    return arch;
}

twd_ttarch2_archive_t *twd_ttarch2_open_memory(const void *data, size_t len) {
    if (!data || len < 12u) return NULL;
    twd_ttarch2_archive_t *arch =
        (twd_ttarch2_archive_t *)calloc(1u, sizeof(twd_ttarch2_archive_t));
    if (!arch) return NULL;
    arch->mem_copy = (uint8_t *)malloc(len);
    if (!arch->mem_copy) {
        free(arch);
        return NULL;
    }
    memcpy(arch->mem_copy, data, len);
    arch->mem_len = len;
    if (init_container(arch) != 0) {
        twd_ttarch2_close(arch);
        return NULL;
    }
    return arch;
}

void twd_ttarch2_close(twd_ttarch2_archive_t *arch) {
    if (!arch) return;
    twd_asset_unmount_ttarch2(arch);
    if (arch->has_file) {
        twd_file_close(&arch->file);
        arch->has_file = 0;
    }
    free(arch->mem_copy);
    free(arch->chunk_offsets);
    free(arch->chunk_raw);
    free(arch->entries);
    free(arch->names);
    free(arch);
}

uint32_t twd_ttarch2_container_kind(const twd_ttarch2_archive_t *arch) {
    return arch ? arch->hdr.container_kind : TWD_TTARCH2_NONE;
}

uint32_t twd_ttarch2_toc_magic(const twd_ttarch2_archive_t *arch) {
    return arch ? arch->toc_magic : 0u;
}

uint32_t twd_ttarch2_entry_count(const twd_ttarch2_archive_t *arch) {
    return arch ? arch->entry_count : 0u;
}

static const char *entry_name_ptr(const twd_ttarch2_archive_t *arch, uint32_t index) {
    if (!arch || index >= arch->entry_count || !arch->names) return "";
    uint32_t pos = arch->entries[index].name_pos;
    if (pos >= arch->names_size) return "";
    return arch->names + pos;
}

int twd_ttarch2_get_entry(
    const twd_ttarch2_archive_t *arch, uint32_t index, twd_ttarch2_entry_t *out) {
    if (!arch || !out || index >= arch->entry_count) return -1;
    const struct twd_ttarch2_record *e = &arch->entries[index];
    out->name = entry_name_ptr(arch, index);
    out->name_crc64 = e->name_crc64;
    out->offset = e->offset;
    out->size = e->size;
    out->flags = e->flags;
    return 0;
}

int twd_ttarch2_find(
    const twd_ttarch2_archive_t *arch, const char *name, uint32_t *out_index,
    twd_ttarch2_entry_t *out) {
    if (!arch || !name) return -1;
    for (uint32_t i = 0u; i < arch->entry_count; ++i) {
        const char *en = entry_name_ptr(arch, i);
        if (path_eq_icase(en, name)) {
            if (out_index) *out_index = i;
            if (out) twd_ttarch2_get_entry(arch, i, out);
            return 0;
        }
    }
    /* Hash fallback using ECMA-182 CRC64 with normalized slashes */
    char norm[512];
    size_t len = strlen(name);
    if (len < sizeof(norm)) {
        for (size_t i = 0u; i < len; ++i) norm[i] = ascii_lower(name[i]);
        norm[len] = '\0';
        uint64_t h = twd_crc64(0ULL, norm);
        if (twd_ttarch2_find_crc64(arch, h, out_index, out) == 0) return 0;
    }
    return -1;
}

int twd_ttarch2_find_crc64(
    const twd_ttarch2_archive_t *arch, uint64_t crc64, uint32_t *out_index,
    twd_ttarch2_entry_t *out) {
    if (!arch) return -1;
    for (uint32_t i = 0u; i < arch->entry_count; ++i) {
        if (arch->entries[i].name_crc64 == crc64) {
            if (out_index) *out_index = i;
            if (out) twd_ttarch2_get_entry(arch, i, out);
            return 0;
        }
    }
    return -1;
}

void *twd_ttarch2_read_entry_alloc(
    twd_ttarch2_archive_t *arch, uint32_t index, size_t *out_size) {
    if (out_size) *out_size = 0u;
    if (!arch || index >= arch->entry_count) return NULL;
    const struct twd_ttarch2_record *e = &arch->entries[index];
    size_t sz = (size_t)e->size;
    uint8_t *buf = (uint8_t *)malloc(sz + 1u);
    if (!buf) return NULL;
    if (sz > 0u) {
        if (stream_read(arch, arch->data_base + e->offset, buf, sz) != 0) {
            free(buf);
            return NULL;
        }
    }
    buf[sz] = '\0';
    if (out_size) *out_size = sz;
    return buf;
}

void twd_ttarch2_free(void *ptr) {
    free(ptr);
}

int twd_asset_mount_ttarch2(twd_ttarch2_archive_t *arch) {
    if (!arch) return -1;
    for (size_t i = 0u; i < g_mounted_count; ++i) {
        if (g_mounted[i] == arch) return 0;
    }
    if (g_mounted_count >= TWD_TTARCH2_MOUNT_MAX) return -1;
    g_mounted[g_mounted_count] = arch;
    g_mounted_owned[g_mounted_count] = 0u;
    ++g_mounted_count;
    return 0;
}

void twd_asset_unmount_ttarch2(const twd_ttarch2_archive_t *arch) {
    if (!arch) return;
    for (size_t i = 0u; i < g_mounted_count; ++i) {
        if (g_mounted[i] == arch) {
            for (size_t j = i + 1u; j < g_mounted_count; ++j) {
                g_mounted[j - 1u] = g_mounted[j];
                g_mounted_owned[j - 1u] = g_mounted_owned[j];
            }
            --g_mounted_count;
            g_mounted[g_mounted_count] = NULL;
            g_mounted_owned[g_mounted_count] = 0u;
            return;
        }
    }
}

void twd_asset_unmount_all_ttarch2(void) {
    while (g_mounted_count > 0u) {
        size_t idx = g_mounted_count - 1u;
        twd_ttarch2_archive_t *a = g_mounted[idx];
        uint8_t owned = g_mounted_owned[idx];
        g_mounted[idx] = NULL;
        g_mounted_owned[idx] = 0u;
        --g_mounted_count;
        if (owned && a) twd_ttarch2_close(a);
    }
}

static int has_ttarch2_ext(const char *name) {
    static const char ext[] = ".ttarch2";
    size_t nlen = strlen(name);
    size_t elen = sizeof(ext) - 1u;
    if (nlen <= elen) return 0;
    const char *tail = name + (nlen - elen);
    for (size_t i = 0u; i < elen; ++i) {
        if (ascii_lower(tail[i]) != ext[i]) return 0;
    }
    return 1;
}

int twd_ttarch2_mount_dir(const char *dir_path) {
    if (!dir_path || dir_path[0] == '\0') return -1;
    twd_dir_t d;
    if (twd_dir_open(dir_path, &d) != 0) return -1;

    int mounted = 0;
    size_t dlen = strlen(dir_path);
    twd_dir_entry_t ent;
    while (twd_dir_next(&d, &ent) > 0) {
        if (ent.is_dir || !has_ttarch2_ext(ent.name)) continue;
        if (g_mounted_count >= TWD_TTARCH2_MOUNT_MAX) break;
        size_t nlen = strlen(ent.name);
        if (dlen + 1u + nlen >= 1024u) continue;
        char full[1024];
        memcpy(full, dir_path, dlen);
        full[dlen] = '/';
        memcpy(full + dlen + 1u, ent.name, nlen + 1u);
        twd_ttarch2_archive_t *arch = twd_ttarch2_open(full);
        if (arch) {
            if (twd_asset_mount_ttarch2(arch) == 0) {
                g_mounted_owned[g_mounted_count - 1u] = 1u;
                ++mounted;
            } else {
                twd_ttarch2_close(arch);
            }
        }
    }
    twd_dir_close(&d);
    return mounted;
}

void *twd_ttarch2_lookup_mounted_alloc(const char *rel_path, size_t *out_size) {
    if (out_size) *out_size = 0u;
    if (!rel_path || rel_path[0] == '\0') return NULL;
    for (size_t i = g_mounted_count; i > 0u; --i) {
        twd_ttarch2_archive_t *a = g_mounted[i - 1u];
        uint32_t idx = 0u;
        if (twd_ttarch2_find(a, rel_path, &idx, NULL) == 0) {
            void *p = twd_ttarch2_read_entry_alloc(a, idx, out_size);
            if (p) return p;
        }
    }
    return NULL;
}

/* ---------- ECMA-182 CRC64 & MetaStream Header Parser ---------- */

#define TWD_CRC64_POLY 0x42F0E1EBA9EA3693ULL

uint64_t twd_crc64_bytes(uint64_t seed, const void *data, size_t len, int to_lower) {
    if (!data || len == 0u) return seed;
    const uint8_t *p = (const uint8_t *)data;
    uint64_t crc = seed;
    for (size_t i = 0u; i < len; ++i) {
        uint8_t ch = p[i];
        if (to_lower && ch >= (uint8_t)'A' && ch <= (uint8_t)'Z') {
            ch = (uint8_t)(ch + ('a' - 'A'));
        }
        crc ^= ((uint64_t)ch << 56);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000000000000000ULL) != 0ULL) {
                crc = (crc << 1) ^ TWD_CRC64_POLY;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

uint64_t twd_crc64(uint64_t seed, const char *str) {
    if (!str) return seed;
    return twd_crc64_bytes(seed, str, strlen(str), 1);
}

int twd_metastream_parse_header(const void *data, size_t len, twd_metastream_header_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!data || len < 4u) return -1;

    const uint8_t *b = (const uint8_t *)data;
    uint32_t magic = rd_u32(b);
    size_t pos = 4u;

    if (magic == 0x4D42494Eu) { /* "NIBM" -> LE MBIN */
        out->kind = TWD_METASTREAM_MBIN;
        out->version = 1u;
        out->payload_offset = 4u;
        return 0;
    }
    if (magic == 0x4D424553u) { /* "SEBM" -> LE MBES */
        out->kind = TWD_METASTREAM_MBES;
        out->version = 1u;
        out->payload_offset = 4u;
        return 0;
    }
    if (magic == 0x4D545245u || magic == 0x4D434F4Du) { /* "ERTM" / "MOCM" */
        out->kind = (magic == 0x4D545245u) ? TWD_METASTREAM_MTRE : TWD_METASTREAM_MCOM;
        out->version = 3u;
        if (magic == 0x4D434F4Du) {
            if (len < 8u) return -1;
            pos += 4u;
        }
        if (len < pos + 4u) return -1;
        out->type_count = rd_u32(b + pos);
        pos += 4u;
        if (out->type_count > 1000u) return -1;
        out->payload_offset = (uint64_t)pos + (uint64_t)out->type_count * 12u;
        return 0;
    }
    if (magic == 0x4D535634u || magic == 0x4D535635u || magic == 0x4D535636u) {
        /* "4VSM", "5VSM", "6VSM" */
        uint32_t ver = (magic == 0x4D535634u) ? 4u :
                       (magic == 0x4D535635u) ? 5u : 6u;
        out->kind = ver;
        out->version = ver;
        if (ver >= 5u) {
            if (len < 20u) return -1;
            out->default_size = rd_u32(b + 4u) & 0x7FFFFFFFu;
            out->debug_size = rd_u32(b + 8u) & 0x7FFFFFFFu;
            uint32_t raw_async = rd_u32(b + 12u);
            out->async_size = raw_async & 0x7FFFFFFFu;
            out->async_aligned = ((raw_async & 0x80000000u) != 0u) ? 1u : 0u;
            out->type_count = rd_u32(b + 16u);
            pos = 20u;
        } else {
            if (len < 16u) return -1;
            out->debug_size = rd_u32(b + 4u) & 0x7FFFFFFFu;
            uint32_t raw_async = rd_u32(b + 8u);
            out->async_size = raw_async & 0x7FFFFFFFu;
            out->async_aligned = ((raw_async & 0x80000000u) != 0u) ? 1u : 0u;
            out->type_count = rd_u32(b + 12u);
            pos = 16u;
        }
        if (out->type_count > 1000u) return -1;
        out->payload_offset = (uint64_t)pos + (uint64_t)out->type_count * 12u;
        return 0;
    }
    return -1;
}

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}

static float rd_f32_le(const uint8_t *p) {
    uint32_t u = rd_u32(p);
    float f = 0.0f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static double rd_f64_le(const uint8_t *p) {
    uint64_t u = rd_u64(p);
    double d = 0.0;
    memcpy(&d, &u, sizeof(d));
    return d;
}

static int propset_read_str_crc(const uint8_t *b, size_t *pos, size_t limit, uint64_t *out_crc) {
    if (*pos + 4u > limit) return -1;
    uint32_t slen = rd_u32(b + *pos);
    *pos += 4u;
    if ((size_t)slen > limit - *pos) return -1;
    *out_crc = twd_crc64_bytes(0ULL, b + *pos, (size_t)slen, 1);
    *pos += (size_t)slen;
    return 0;
}

typedef enum propset_wire {
    PW_UNKNOWN = 0,
    PW_BOOL = 1,
    PW_I8,
    PW_U8,
    PW_I16,
    PW_U16,
    PW_I32,
    PW_U32,
    PW_I64,
    PW_U64,
    PW_F32,
    PW_F64,
    PW_STR,
    PW_SYMBOL,
    PW_VEC2,
    PW_VEC3,
    PW_VEC4,
    PW_IRECT
} propset_wire_t;

static propset_wire_t propset_classify_wire(uint64_t type_crc) {
    switch (type_crc) {
    case 0x9004c5587575d6c0ULL: return PW_BOOL;   /* bool */
    case 0xafb5db7305e23480ULL: return PW_I8;     /* int8 */
    case 0x3a195525e24afc93ULL: return PW_U8;     /* uint8 */
    case 0x29653dc4d2d86302ULL: return PW_I16;    /* int16 */
    case 0x1eef71b2306b5b6cULL: return PW_U16;    /* uint16 */
    case 0x7caceebcd26d075cULL: /* int32 */
    case 0x74dcd231c29f9a4aULL: /* S32 */
    case 0x84283cb979d71641ULL: /* Flags */
        return PW_I32;
    case 0x4b26a2ca30de3f32ULL: /* uint32 */
    case 0xbcbae3735c6dc8d8ULL: /* U32 */
        return PW_U32;
    case 0x268e0b3fc19a48aeULL: /* int64 */
    case 0x2efe37b2d168d5b8ULL: /* S64 */
        return PW_I64;
    case 0x11044749232970c0ULL: /* uint64 */
    case 0xe69806f04f9a872aULL: /* U64 */
        return PW_U64;
    case 0xbae4cbd77f139a91ULL: return PW_F32;    /* float */
    case 0x5dc78f79fc4b8df7ULL: return PW_F64;    /* double */
    case 0xcd9c6e605f5af4b4ULL: return PW_STR;    /* String */
    case 0x004f023463d89fb0ULL: /* Symbol */
    case 0x387e6b9ca558ac8dULL: /* Handle<PropertySet> */
        return PW_SYMBOL;
    case 0x7bbca244e61f1a07ULL: return PW_VEC2;   /* Vector2 */
    case 0x394c43af4ff52c94ULL: return PW_VEC3;   /* Vector3 */
    case 0xb76e07d6bb899bfeULL: /* Vector4 */
    case 0xc16762f7763d62abULL: /* Color */
    case 0x62c9e560c8dcec67ULL: /* Quaternion */
    case 0x53a0b3aa86983895ULL: /* TRect<float> */
        return PW_VEC4;
    case 0x336adb075a965645ULL: /* Rect */
    case 0xe3cf6e2a6cab5ae1ULL: /* TRect<int> */
        return PW_IRECT;
    default:
        return PW_UNKNOWN;
    }
}

static int propset_read_value(const uint8_t *b, size_t *pos, size_t limit,
                              propset_wire_t w, twd_prop_entry_t *ent) {
    switch (w) {
    case PW_BOOL:
        if (*pos + 1u > limit) return -1;
        ent->kind = TWD_PROP_BOOL;
        ent->val.b = (b[*pos] != 0u) ? 1 : 0;
        *pos += 1u;
        return 0;
    case PW_I8:
        if (*pos + 1u > limit) return -1;
        ent->kind = TWD_PROP_I32;
        ent->val.i32 = (int32_t)(int8_t)b[*pos];
        *pos += 1u;
        return 0;
    case PW_U8:
        if (*pos + 1u > limit) return -1;
        ent->kind = TWD_PROP_U32;
        ent->val.u32 = (uint32_t)b[*pos];
        *pos += 1u;
        return 0;
    case PW_I16:
        if (*pos + 2u > limit) return -1;
        ent->kind = TWD_PROP_I32;
        ent->val.i32 = (int32_t)(int16_t)rd_u16_le(b + *pos);
        *pos += 2u;
        return 0;
    case PW_U16:
        if (*pos + 2u > limit) return -1;
        ent->kind = TWD_PROP_U32;
        ent->val.u32 = (uint32_t)rd_u16_le(b + *pos);
        *pos += 2u;
        return 0;
    case PW_I32:
        if (*pos + 4u > limit) return -1;
        ent->kind = TWD_PROP_I32;
        ent->val.i32 = (int32_t)rd_u32(b + *pos);
        *pos += 4u;
        return 0;
    case PW_U32:
        if (*pos + 4u > limit) return -1;
        ent->kind = TWD_PROP_U32;
        ent->val.u32 = rd_u32(b + *pos);
        *pos += 4u;
        return 0;
    case PW_I64:
        if (*pos + 8u > limit) return -1;
        ent->kind = TWD_PROP_I64;
        ent->val.i64 = (int64_t)rd_u64(b + *pos);
        *pos += 8u;
        return 0;
    case PW_U64:
        if (*pos + 8u > limit) return -1;
        ent->kind = TWD_PROP_U64;
        ent->val.u64 = rd_u64(b + *pos);
        *pos += 8u;
        return 0;
    case PW_F32:
        if (*pos + 4u > limit) return -1;
        ent->kind = TWD_PROP_F32;
        ent->val.f32 = rd_f32_le(b + *pos);
        *pos += 4u;
        return 0;
    case PW_F64:
        if (*pos + 8u > limit) return -1;
        ent->kind = TWD_PROP_F64;
        ent->val.f64 = rd_f64_le(b + *pos);
        *pos += 8u;
        return 0;
    case PW_STR: {
        if (*pos + 4u > limit) return -1;
        uint32_t slen = rd_u32(b + *pos);
        *pos += 4u;
        if (slen > 1048576u || (size_t)slen > limit - *pos) return -1;
        char *s = (char *)malloc((size_t)slen + 1u);
        if (!s) return -1;
        if (slen > 0u) memcpy(s, b + *pos, (size_t)slen);
        s[slen] = '\0';
        *pos += (size_t)slen;
        ent->kind = TWD_PROP_STR;
        ent->val.str = s;
        return 0;
    }
    case PW_SYMBOL:
        if (*pos + 8u > limit) return -1;
        ent->kind = TWD_PROP_SYMBOL;
        ent->val.sym = rd_u64(b + *pos);
        *pos += 8u;
        return 0;
    case PW_VEC2:
        if (*pos + 8u > limit) return -1;
        ent->kind = TWD_PROP_VEC2;
        ent->val.vec[0] = rd_f32_le(b + *pos);
        ent->val.vec[1] = rd_f32_le(b + *pos + 4u);
        *pos += 8u;
        return 0;
    case PW_VEC3:
        if (*pos + 12u > limit) return -1;
        ent->kind = TWD_PROP_VEC3;
        ent->val.vec[0] = rd_f32_le(b + *pos);
        ent->val.vec[1] = rd_f32_le(b + *pos + 4u);
        ent->val.vec[2] = rd_f32_le(b + *pos + 8u);
        *pos += 12u;
        return 0;
    case PW_VEC4:
        if (*pos + 16u > limit) return -1;
        ent->kind = TWD_PROP_VEC4;
        ent->val.vec[0] = rd_f32_le(b + *pos);
        ent->val.vec[1] = rd_f32_le(b + *pos + 4u);
        ent->val.vec[2] = rd_f32_le(b + *pos + 8u);
        ent->val.vec[3] = rd_f32_le(b + *pos + 12u);
        *pos += 16u;
        return 0;
    case PW_IRECT:
        if (*pos + 16u > limit) return -1;
        ent->kind = TWD_PROP_IRECT;
        ent->val.irect[0] = (int32_t)rd_u32(b + *pos);
        ent->val.irect[1] = (int32_t)rd_u32(b + *pos + 4u);
        ent->val.irect[2] = (int32_t)rd_u32(b + *pos + 8u);
        ent->val.irect[3] = (int32_t)rd_u32(b + *pos + 12u);
        *pos += 16u;
        return 0;
    default:
        return 1;
    }
}

void twd_propset_free(twd_propset_t *ps) {
    if (!ps) return;
    if (ps->entries) {
        for (uint32_t i = 0u; i < ps->entry_count; ++i) {
            if (ps->entries[i].kind == TWD_PROP_STR) {
                free(ps->entries[i].val.str);
                ps->entries[i].val.str = NULL;
            }
        }
        free(ps->entries);
    }
    free(ps->parent_crcs);
    memset(ps, 0, sizeof(*ps));
}

int twd_propset_parse_memory(const void *data, size_t len, twd_propset_t *out) {
    if (!data || !out) return -1;
    memset(out, 0, sizeof(*out));
    const uint8_t *b = (const uint8_t *)data;
    size_t pos = 0u;
    size_t end_off = len;

    twd_metastream_header_t mh;
    if (twd_metastream_parse_header(data, len, &mh) == 0) {
        if (mh.payload_offset > (uint64_t)len) return -1;
        pos = (size_t)mh.payload_offset;
        if (mh.default_size > 0u && (size_t)mh.default_size <= len - pos) {
            end_off = pos + (size_t)mh.default_size;
        }
    }

    if (pos + 12u > end_off) return -1;
    int32_t prop_ver = (int32_t)rd_u32(b + pos);
    uint32_t prop_flags = rd_u32(b + pos + 4u);
    uint32_t body_size = rd_u32(b + pos + 8u);
    pos += 12u;
    if (prop_ver < 0 || prop_ver > 16 || (size_t)body_size > end_off - pos) {
        return -1;
    }
    size_t block_end = pos + (size_t)body_size;

    out->prop_version = prop_ver;
    out->prop_flags = prop_flags;

    if (pos + 4u > block_end) return -1;
    uint32_t parent_cnt = rd_u32(b + pos);
    pos += 4u;
    if (parent_cnt > 4096u || (size_t)parent_cnt * 8u > block_end - pos) {
        return -1;
    }
    if (parent_cnt > 0u) {
        out->parent_crcs = (uint64_t *)calloc((size_t)parent_cnt, sizeof(uint64_t));
        if (!out->parent_crcs) return -1;
        out->parent_count = parent_cnt;
        for (uint32_t i = 0u; i < parent_cnt; ++i) {
            out->parent_crcs[i] = rd_u64(b + pos);
            pos += 8u;
        }
    }

    if (pos + 4u > block_end) {
        twd_propset_free(out);
        return -1;
    }
    uint32_t num_groups = rd_u32(b + pos);
    pos += 4u;
    if (num_groups > 4096u) {
        twd_propset_free(out);
        return -1;
    }

    uint32_t cap = 0u;
    for (uint32_t g = 0u; g < num_groups; ++g) {
        uint64_t type_crc = 0ULL;
        if (prop_ver == 0) {
            if (propset_read_str_crc(b, &pos, block_end, &type_crc) != 0) {
                twd_propset_free(out);
                return -1;
            }
        } else {
            if (pos + 8u > block_end) {
                twd_propset_free(out);
                return -1;
            }
            type_crc = rd_u64(b + pos);
            pos += 8u;
        }
        if (pos + 4u > block_end) {
            twd_propset_free(out);
            return -1;
        }
        uint32_t count = rd_u32(b + pos);
        pos += 4u;
        if (count > 65536u || out->entry_count + count > 65536u) {
            twd_propset_free(out);
            return -1;
        }

        propset_wire_t wire = propset_classify_wire(type_crc);
        if (wire == PW_UNKNOWN) {
            out->truncated = 1;
            break;
        }

        if (out->entry_count + count > cap) {
            uint32_t new_cap = (cap == 0u) ? 16u : cap;
            while (new_cap < out->entry_count + count) new_cap *= 2u;
            twd_prop_entry_t *np = (twd_prop_entry_t *)realloc(
                out->entries, (size_t)new_cap * sizeof(twd_prop_entry_t));
            if (!np) {
                twd_propset_free(out);
                return -1;
            }
            out->entries = np;
            cap = new_cap;
        }

        for (uint32_t p = 0u; p < count; ++p) {
            twd_prop_entry_t ent;
            memset(&ent, 0, sizeof(ent));
            ent.type_crc = type_crc;
            if (prop_ver == 0) {
                if (propset_read_str_crc(b, &pos, block_end, &ent.key_crc) != 0) {
                    twd_propset_free(out);
                    return -1;
                }
            } else {
                if (pos + 8u > block_end) {
                    twd_propset_free(out);
                    return -1;
                }
                ent.key_crc = rd_u64(b + pos);
                pos += 8u;
            }
            if (propset_read_value(b, &pos, block_end, wire, &ent) != 0) {
                twd_propset_free(out);
                return -1;
            }
            out->entries[out->entry_count++] = ent;
        }
    }
    return 0;
}

const twd_prop_entry_t *twd_propset_find_crc(const twd_propset_t *ps, uint64_t key_crc) {
    if (!ps || !ps->entries) return NULL;
    for (uint32_t i = 0u; i < ps->entry_count; ++i) {
        if (ps->entries[i].key_crc == key_crc) {
            return &ps->entries[i];
        }
    }
    return NULL;
}

const twd_prop_entry_t *twd_propset_find(const twd_propset_t *ps, const char *key) {
    if (!ps || !key) return NULL;
    return twd_propset_find_crc(ps, twd_crc64(0ULL, key));
}

int twd_propset_get_bool(const twd_propset_t *ps, const char *key, int default_val) {
    const twd_prop_entry_t *e = twd_propset_find(ps, key);
    if (!e || e->kind != TWD_PROP_BOOL) return default_val;
    return e->val.b;
}

int32_t twd_propset_get_i32(const twd_propset_t *ps, const char *key, int32_t default_val) {
    const twd_prop_entry_t *e = twd_propset_find(ps, key);
    if (!e || e->kind != TWD_PROP_I32) return default_val;
    return e->val.i32;
}

uint32_t twd_propset_get_u32(const twd_propset_t *ps, const char *key, uint32_t default_val) {
    const twd_prop_entry_t *e = twd_propset_find(ps, key);
    if (!e) return default_val;
    if (e->kind == TWD_PROP_U32) return e->val.u32;
    if (e->kind == TWD_PROP_I32 && e->val.i32 >= 0) return (uint32_t)e->val.i32;
    return default_val;
}

float twd_propset_get_f32(const twd_propset_t *ps, const char *key, float default_val) {
    const twd_prop_entry_t *e = twd_propset_find(ps, key);
    if (!e || e->kind != TWD_PROP_F32) return default_val;
    return e->val.f32;
}

const char *twd_propset_get_str(const twd_propset_t *ps, const char *key, const char *default_val) {
    const twd_prop_entry_t *e = twd_propset_find(ps, key);
    if (!e || e->kind != TWD_PROP_STR || !e->val.str) return default_val;
    return e->val.str;
}

int twd_propset_get_vec2(const twd_propset_t *ps, const char *key, float *out_x, float *out_y) {
    const twd_prop_entry_t *e = twd_propset_find(ps, key);
    if (!e || e->kind != TWD_PROP_VEC2) return -1;
    if (out_x) *out_x = e->val.vec[0];
    if (out_y) *out_y = e->val.vec[1];
    return 0;
}
