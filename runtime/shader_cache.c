/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

#define TWD_DXBC_MAGIC   0x43425844u /* 'DXBC' */
#define TWD_FOURCC_ISGN  0x4e475349u /* 'ISGN' */
#define TWD_FOURCC_OSGN  0x4e47534fu /* 'OSGN' */
#define TWD_FOURCC_SHDR  0x52444853u /* 'SHDR' */
#define TWD_FOURCC_SHEX  0x58454853u /* 'SHEX' */
#define TWD_SPIRV_MAGIC  0x07230203u
#define TWD_SHADER_CAP   256u

typedef struct shader_slot {
    int used;
    uint64_t hash;
    uint32_t stage;
    uint32_t *spirv;
    size_t spirv_words;
    char *glsl;
} shader_slot_t;

struct twd_shader_cache {
    shader_slot_t slots[TWD_SHADER_CAP];
    uint32_t count;
    uint64_t hits;
    uint64_t misses;
};

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t twd_dxbc_hash(const void *bytecode, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    const uint8_t *p = (const uint8_t *)bytecode;
    if (!p) return 0ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

int twd_dxbc_parse(const void *bytecode, size_t len, twd_dxbc_info_t *out_info) {
    if (!out_info) return -1;
    memset(out_info, 0, sizeof(*out_info));
    if (!bytecode || len == 0u) return -1;
    out_info->hash = twd_dxbc_hash(bytecode, len);
    out_info->total_bytes = (uint32_t)len;
    const uint8_t *b = (const uint8_t *)bytecode;
    if (len < 32u || rd_u32_le(b) != TWD_DXBC_MAGIC) {
        return 0;
    }
    uint32_t dxbc_sz = rd_u32_le(b + 24u);
    uint32_t nchunks = rd_u32_le(b + 28u);
    if (dxbc_sz > len || nchunks > 64u || 32u + (size_t)nchunks * 4u > len) {
        return 0;
    }
    out_info->valid_dxbc = 1;
    out_info->chunk_count = nchunks;
    for (uint32_t i = 0; i < nchunks; i++) {
        uint32_t coff = rd_u32_le(b + 32u + (size_t)i * 4u);
        if ((size_t)coff + 8u > len) continue;
        uint32_t fourcc = rd_u32_le(b + coff);
        uint32_t csize = rd_u32_le(b + coff + 4u);
        if (fourcc == TWD_FOURCC_ISGN) out_info->has_isgn = 1;
        else if (fourcc == TWD_FOURCC_OSGN) out_info->has_osgn = 1;
        else if ((fourcc == TWD_FOURCC_SHDR || fourcc == TWD_FOURCC_SHEX) &&
                 csize >= 4u && (size_t)coff + 12u <= len) {
            uint32_t tok = rd_u32_le(b + coff + 8u);
            uint32_t st = (tok >> 16) & 0xffffu;
            out_info->version_major = (tok >> 4) & 0xfu;
            out_info->version_minor = tok & 0xfu;
            if (st == 1u) out_info->stage = TWD_SHADER_STAGE_VS;
            else if (st == 0u) out_info->stage = TWD_SHADER_STAGE_PS;
            else if (st == 5u) out_info->stage = TWD_SHADER_STAGE_CS;
        }
    }
    return 1;
}

twd_shader_cache_t *twd_shader_cache_create(void) {
    return (twd_shader_cache_t *)calloc(1, sizeof(twd_shader_cache_t));
}

void twd_shader_cache_destroy(twd_shader_cache_t *cache) {
    if (!cache) return;
    for (uint32_t i = 0; i < TWD_SHADER_CAP; i++) {
        if (cache->slots[i].used) {
            free(cache->slots[i].spirv);
            free(cache->slots[i].glsl);
        }
    }
    free(cache);
}

static shader_slot_t *find_or_alloc_slot(
    twd_shader_cache_t *cache, uint64_t hash, uint32_t stage, int *out_is_hit) {
    int free_idx = -1;
    for (uint32_t i = 0; i < TWD_SHADER_CAP; i++) {
        if (cache->slots[i].used) {
            if (cache->slots[i].hash == hash) {
                if (out_is_hit) *out_is_hit = 1;
                return &cache->slots[i];
            }
        } else if (free_idx < 0) {
            free_idx = (int)i;
        }
    }
    if (free_idx < 0) return NULL;
    if (out_is_hit) *out_is_hit = 0;
    shader_slot_t *s = &cache->slots[free_idx];
    s->used = 1;
    s->hash = hash;
    s->stage = stage;
    cache->count++;
    return s;
}

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1u);
    if (!out) return NULL;
    memcpy(out, s, n + 1u);
    return out;
}

int twd_shader_cache_put_spirv(
    twd_shader_cache_t *cache, uint64_t hash, uint32_t stage,
    const uint32_t *words, size_t word_count) {
    if (!cache || !words || word_count < 5u || words[0] != TWD_SPIRV_MAGIC) return -1;
    shader_slot_t *s = find_or_alloc_slot(cache, hash, stage, NULL);
    if (!s) return -1;
    uint32_t *copy = (uint32_t *)malloc(word_count * sizeof(uint32_t));
    if (!copy) return -1;
    memcpy(copy, words, word_count * sizeof(uint32_t));
    free(s->spirv);
    s->spirv = copy;
    s->spirv_words = word_count;
    s->stage = stage;
    return 0;
}

int twd_shader_cache_put_glsl(
    twd_shader_cache_t *cache, uint64_t hash, uint32_t stage, const char *glsl) {
    if (!cache || !glsl || glsl[0] == '\0') return -1;
    shader_slot_t *s = find_or_alloc_slot(cache, hash, stage, NULL);
    if (!s) return -1;
    char *copy = dup_str(glsl);
    if (!copy) return -1;
    free(s->glsl);
    s->glsl = copy;
    s->stage = stage;
    return 0;
}

static const char *fallback_glsl_for_stage(uint32_t stage) {
    if (stage == TWD_SHADER_STAGE_VS) {
        return "#version 300 es\n"
               "layout(location=0) in vec4 a_pos;\n"
               "layout(location=1) in vec2 a_uv;\n"
               "out vec2 v_uv;\n"
               "void main(){ v_uv = a_uv; gl_Position = a_pos; }\n";
    }
    return "#version 300 es\n"
           "precision mediump float;\n"
           "in vec2 v_uv;\n"
           "out vec4 o_color;\n"
           "void main(){ o_color = vec4(1.0); }\n";
}

int twd_shader_cache_register_dxbc(
    twd_shader_cache_t *cache, const void *bytecode, size_t len,
    uint32_t fallback_stage, uint64_t *out_hash) {
    if (!cache) return -1;
    twd_dxbc_info_t info;
    if (bytecode && len > 0u) {
        twd_dxbc_parse(bytecode, len, &info);
    } else {
        memset(&info, 0, sizeof(info));
        info.hash = (uint64_t)(fallback_stage + 1u);
    }
    uint32_t stage = info.valid_dxbc ? info.stage : fallback_stage;
    if (out_hash) *out_hash = info.hash;

    int hit = 0;
    shader_slot_t *s = find_or_alloc_slot(cache, info.hash, stage, &hit);
    if (!s) return -1;
    if (hit && (s->glsl || s->spirv)) {
        cache->hits++;
        return 0;
    }
    cache->misses++;
    if (!s->glsl) {
        s->glsl = dup_str(fallback_glsl_for_stage(stage));
    }
    return s->glsl || s->spirv ? 0 : -1;
}

int twd_shader_cache_lookup(
    const twd_shader_cache_t *cache, uint64_t hash, twd_shader_cache_entry_t *out_entry) {
    if (!cache || !out_entry) return 0;
    for (uint32_t i = 0; i < TWD_SHADER_CAP; i++) {
        if (cache->slots[i].used && cache->slots[i].hash == hash) {
            out_entry->hash = cache->slots[i].hash;
            out_entry->stage = cache->slots[i].stage;
            out_entry->spirv_words = cache->slots[i].spirv;
            out_entry->spirv_word_count = cache->slots[i].spirv_words;
            out_entry->glsl_source = cache->slots[i].glsl;
            return 1;
        }
    }
    return 0;
}

uint32_t twd_shader_cache_count(const twd_shader_cache_t *cache) {
    return cache ? cache->count : 0u;
}

void twd_shader_cache_stats(const twd_shader_cache_t *cache, uint64_t *out_hits, uint64_t *out_misses) {
    if (out_hits) *out_hits = cache ? cache->hits : 0ULL;
    if (out_misses) *out_misses = cache ? cache->misses : 0ULL;
}
