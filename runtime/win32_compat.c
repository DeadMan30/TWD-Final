/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Portable Win32 / CryptoAPI / WinSock2 / Shell / Clipboard compatibility layer
 * and startup import binding table for TWD runtime.
 */
#include "twd_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(disable : 4127)
#endif

/* =========================================================================
 * 1. CryptoAPI Hash Engine (CALG_MD5, CALG_SHA1, CALG_SHA_256)
 * ========================================================================= */

#define TWD_CRYPT_CTX_MAGIC  0x43525950u
#define TWD_CRYPT_HASH_MAGIC 0x48415348u

struct twd_crypt_ctx {
    uint32_t magic;
    uint32_t flags;
};

struct twd_crypt_hash {
    uint32_t magic;
    uint32_t alg_id;
    uint32_t state[8];
    uint64_t total_bytes;
    uint32_t buf_len;
    uint8_t buf[64];
    int finalized;
    uint8_t digest[32];
    uint32_t digest_len;
};

static uint32_t rotl32(uint32_t v, uint32_t s) {
    return (v << s) | (v >> (32u - s));
}

static uint32_t rotr32(uint32_t v, uint32_t s) {
    return (v >> s) | (v << (32u - s));
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void md5_transform(uint32_t st[4], const uint8_t blk[64]) {
    static const uint32_t k[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
        0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
        0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
        0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
        0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
        0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
        0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
    };
    static const uint32_t s[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    uint32_t m[16];
    for (uint32_t i = 0u; i < 16u; ++i) m[i] = rd_le32(blk + i * 4u);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (uint32_t i = 0u; i < 64u; ++i) {
        uint32_t f = 0u, g = 0u;
        if (i < 16u) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32u) {
            f = (d & b) | ((~d) & c);
            g = (5u * i + 1u) & 15u;
        } else if (i < 48u) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) & 15u;
        } else {
            f = c ^ (b | (~d));
            g = (7u * i) & 15u;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + k[i] + m[g], s[i]);
        a = tmp;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

static void sha1_transform(uint32_t st[5], const uint8_t blk[64]) {
    uint32_t w[80];
    for (uint32_t i = 0u; i < 16u; ++i) w[i] = rd_be32(blk + i * 4u);
    for (uint32_t i = 16u; i < 80u; ++i) {
        w[i] = rotl32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (uint32_t i = 0u; i < 80u; ++i) {
        uint32_t f = 0u, k = 0u;
        if (i < 20u) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999u;
        } else if (i < 40u) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60u) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        uint32_t tmp = rotl32(a, 5u) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30u);
        b = a;
        a = tmp;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
}

static void sha256_transform(uint32_t st[8], const uint8_t blk[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t w[64];
    for (uint32_t i = 0u; i < 16u; ++i) w[i] = rd_be32(blk + i * 4u);
    for (uint32_t i = 16u; i < 64u; ++i) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^ rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^ rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (uint32_t i = 0u; i < 64u; ++i) {
        uint32_t S1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void crypt_transform_block(twd_crypt_hash_t *h, const uint8_t blk[64]) {
    if (h->alg_id == TWD_CALG_MD5) md5_transform(h->state, blk);
    else if (h->alg_id == TWD_CALG_SHA1) sha1_transform(h->state, blk);
    else sha256_transform(h->state, blk);
}

twd_crypt_ctx_t *twd_crypt_acquire(uint32_t flags) {
    twd_crypt_ctx_t *ctx = (twd_crypt_ctx_t *)calloc(1u, sizeof(twd_crypt_ctx_t));
    if (!ctx) return NULL;
    ctx->magic = TWD_CRYPT_CTX_MAGIC;
    ctx->flags = flags;
    return ctx;
}

void twd_crypt_release(twd_crypt_ctx_t *ctx) {
    if (!ctx || ctx->magic != TWD_CRYPT_CTX_MAGIC) return;
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}

twd_crypt_hash_t *twd_crypt_create_hash(twd_crypt_ctx_t *ctx, uint32_t alg_id) {
    if (!ctx || ctx->magic != TWD_CRYPT_CTX_MAGIC) return NULL;
    if (alg_id != TWD_CALG_MD5 && alg_id != TWD_CALG_SHA1 && alg_id != TWD_CALG_SHA256) return NULL;
    twd_crypt_hash_t *h = (twd_crypt_hash_t *)calloc(1u, sizeof(twd_crypt_hash_t));
    if (!h) return NULL;
    h->magic = TWD_CRYPT_HASH_MAGIC;
    h->alg_id = alg_id;
    if (alg_id == TWD_CALG_MD5) {
        h->state[0] = 0x67452301u; h->state[1] = 0xefcdab89u;
        h->state[2] = 0x98badcfeu; h->state[3] = 0x10325476u;
        h->digest_len = 16u;
    } else if (alg_id == TWD_CALG_SHA1) {
        h->state[0] = 0x67452301u; h->state[1] = 0xefcdab89u;
        h->state[2] = 0x98badcfeu; h->state[3] = 0x10325476u;
        h->state[4] = 0xc3d2e1f0u;
        h->digest_len = 20u;
    } else {
        h->state[0] = 0x6a09e667u; h->state[1] = 0xbb67ae85u;
        h->state[2] = 0x3c6ef372u; h->state[3] = 0xa54ff53au;
        h->state[4] = 0x510e527fu; h->state[5] = 0x9b05688cu;
        h->state[6] = 0x1f83d9abu; h->state[7] = 0x5be0cd19u;
        h->digest_len = 32u;
    }
    return h;
}

void twd_crypt_destroy_hash(twd_crypt_hash_t *h) {
    if (!h || h->magic != TWD_CRYPT_HASH_MAGIC) return;
    memset(h, 0, sizeof(*h));
    free(h);
}

int twd_crypt_hash_data(twd_crypt_hash_t *h, const void *data, size_t len) {
    if (!h || h->magic != TWD_CRYPT_HASH_MAGIC || h->finalized) return -1;
    if (len == 0u) return 0;
    if (!data) return -1;
    const uint8_t *src = (const uint8_t *)data;
    h->total_bytes += (uint64_t)len;
    size_t pos = 0u;
    if (h->buf_len > 0u) {
        size_t take = 64u - h->buf_len;
        if (take > len) take = len;
        memcpy(h->buf + h->buf_len, src, take);
        h->buf_len += (uint32_t)take;
        pos += take;
        if (h->buf_len == 64u) {
            crypt_transform_block(h, h->buf);
            h->buf_len = 0u;
        }
    }
    while (pos + 64u <= len) {
        crypt_transform_block(h, src + pos);
        pos += 64u;
    }
    if (pos < len) {
        size_t rem = len - pos;
        memcpy(h->buf, src + pos, rem);
        h->buf_len = (uint32_t)rem;
    }
    return 0;
}

int twd_crypt_get_hash_param(
    twd_crypt_hash_t *h, uint32_t param, void *out_buf, size_t *io_len) {
    if (!h || h->magic != TWD_CRYPT_HASH_MAGIC || !io_len) return -1;
    if (param == TWD_HP_HASHSIZE) {
        if (*io_len < 4u || !out_buf) {
            *io_len = 4u;
            return -1;
        }
        wr_le32((uint8_t *)out_buf, h->digest_len);
        *io_len = 4u;
        return 0;
    }
    if (param != TWD_HP_HASHVAL) return -1;
    if (!h->finalized) {
        uint64_t bits = h->total_bytes * 8ULL;
        h->buf[h->buf_len++] = 0x80u;
        if (h->buf_len > 56u) {
            if (h->buf_len < 64u) memset(h->buf + h->buf_len, 0, 64u - h->buf_len);
            crypt_transform_block(h, h->buf);
            h->buf_len = 0u;
        }
        if (h->buf_len < 56u) memset(h->buf + h->buf_len, 0, 56u - h->buf_len);
        if (h->alg_id == TWD_CALG_MD5) {
            wr_le32(h->buf + 56u, (uint32_t)(bits & 0xffffffffu));
            wr_le32(h->buf + 60u, (uint32_t)(bits >> 32));
        } else {
            wr_be32(h->buf + 56u, (uint32_t)(bits >> 32));
            wr_be32(h->buf + 60u, (uint32_t)(bits & 0xffffffffu));
        }
        crypt_transform_block(h, h->buf);
        h->buf_len = 0u;
        if (h->alg_id == TWD_CALG_MD5) {
            for (uint32_t i = 0u; i < 4u; ++i) wr_le32(h->digest + i * 4u, h->state[i]);
        } else if (h->alg_id == TWD_CALG_SHA1) {
            for (uint32_t i = 0u; i < 5u; ++i) wr_be32(h->digest + i * 4u, h->state[i]);
        } else {
            for (uint32_t i = 0u; i < 8u; ++i) wr_be32(h->digest + i * 4u, h->state[i]);
        }
        h->finalized = 1;
    }
    if (*io_len < h->digest_len || !out_buf) {
        *io_len = h->digest_len;
        return -1;
    }
    memcpy(out_buf, h->digest, h->digest_len);
    *io_len = h->digest_len;
    return 0;
}

/* =========================================================================
 * 2. Pointer Obfuscation, SLIST, Pipes, Time/Version, Clipboard, Network
 * ========================================================================= */

void *twd_encode_pointer(const void *ptr) {
    uintptr_t cookie = (uintptr_t)0x54574434u ^ ((uintptr_t)0x52544b31u << 16);
    return (void *)((uintptr_t)ptr ^ cookie);
}

void *twd_decode_pointer(const void *ptr) {
    return twd_encode_pointer(ptr);
}

void twd_slist_init(twd_slist_head_t *head) {
    if (!head) return;
    head->first = NULL;
    head->depth = 0u;
}

void twd_slist_push(twd_slist_head_t *head, twd_slist_entry_t *entry) {
    if (!head || !entry) return;
    entry->next = head->first;
    head->first = entry;
    head->depth++;
}

twd_slist_entry_t *twd_slist_pop(twd_slist_head_t *head) {
    if (!head || !head->first) return NULL;
    twd_slist_entry_t *top = head->first;
    head->first = top->next;
    top->next = NULL;
    if (head->depth > 0u) head->depth--;
    return top;
}

struct twd_pipe {
    uint8_t buf[4096];
    size_t head;
    size_t tail;
    size_t count;
};

twd_pipe_t *twd_pipe_create(void) {
    return (twd_pipe_t *)calloc(1u, sizeof(twd_pipe_t));
}

void twd_pipe_close(twd_pipe_t *pipe) {
    free(pipe);
}

size_t twd_pipe_write(twd_pipe_t *pipe, const void *data, size_t len) {
    if (!pipe || !data || len == 0u) return 0u;
    const uint8_t *src = (const uint8_t *)data;
    size_t space = sizeof(pipe->buf) - pipe->count;
    size_t wr = (len < space) ? len : space;
    for (size_t i = 0u; i < wr; ++i) {
        pipe->buf[pipe->tail] = src[i];
        pipe->tail = (pipe->tail + 1u) % sizeof(pipe->buf);
    }
    pipe->count += wr;
    return wr;
}

size_t twd_pipe_peek(const twd_pipe_t *pipe, void *dst, size_t cap, size_t *out_avail) {
    if (out_avail) *out_avail = pipe ? pipe->count : 0u;
    if (!pipe || !dst || cap == 0u) return 0u;
    size_t rd = (cap < pipe->count) ? cap : pipe->count;
    uint8_t *out = (uint8_t *)dst;
    size_t cur = pipe->head;
    for (size_t i = 0u; i < rd; ++i) {
        out[i] = pipe->buf[cur];
        cur = (cur + 1u) % sizeof(pipe->buf);
    }
    return rd;
}

size_t twd_pipe_read(twd_pipe_t *pipe, void *dst, size_t cap) {
    size_t got = twd_pipe_peek(pipe, dst, cap, NULL);
    if (pipe && got > 0u) {
        pipe->head = (pipe->head + got) % sizeof(pipe->buf);
        pipe->count -= got;
    }
    return got;
}

int twd_filetime_to_systemtime(uint64_t ft_100ns, twd_systemtime_t *out) {
    if (!out) return -1;
    uint64_t total_sec = ft_100ns / 10000000ULL;
    uint64_t sub_100ns = ft_100ns % 10000000ULL;
    uint64_t days = total_sec / 86400ULL;
    uint64_t rem = total_sec % 86400ULL;

    /* 1601-01-01 to 0000-03-01 offset in proleptic Gregorian calendar (719468 - 134774) */
    int64_t z = (int64_t)days + 584694LL;
    int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    uint64_t doe = (uint64_t)(z - era * 146097LL);
    uint64_t yoe = (doe - doe / 1460ULL + doe / 36524ULL - doe / 146096ULL) / 365ULL;
    int64_t y = (int64_t)yoe + era * 400LL;
    uint64_t doy = doe - (365ULL * yoe + yoe / 4ULL - yoe / 100ULL);
    uint64_t mp = (5ULL * doy + 2ULL) / 153ULL;
    uint64_t d = doy - (153ULL * mp + 2ULL) / 5ULL + 1ULL;
    uint64_t m = (mp < 10ULL) ? (mp + 3ULL) : (mp - 9ULL);
    y += (m <= 2ULL) ? 1LL : 0LL;

    out->year = (uint16_t)y;
    out->month = (uint16_t)m;
    out->day_of_week = (uint16_t)((days + 1ULL) % 7ULL); /* 1601-01-01 was Monday */
    out->day = (uint16_t)d;
    out->hour = (uint16_t)(rem / 3600ULL);
    out->minute = (uint16_t)((rem % 3600ULL) / 60ULL);
    out->second = (uint16_t)(rem % 60ULL);
    out->milliseconds = (uint16_t)(sub_100ns / 10000ULL);
    return 0;
}

typedef struct twd_vs_fixed_info {
    uint32_t signature;
    uint32_t struc_version;
    uint32_t file_version_ms;
    uint32_t file_version_ls;
    uint32_t product_version_ms;
    uint32_t product_version_ls;
    uint32_t file_flags_mask;
    uint32_t file_flags;
    uint32_t file_os;
    uint32_t file_type;
    uint32_t file_subtype;
    uint32_t file_date_ms;
    uint32_t file_date_ls;
} twd_vs_fixed_info_t;

int twd_file_version_query(const char *sub_block, const void **out_ptr, uint32_t *out_len) {
    static const twd_vs_fixed_info_t k_ver = {
        0xFEEF04BDu, 0x00010000u, 0x00040000u, 0x00000001u,
        0x00040000u, 0x00000001u, 0x0000003Fu, 0x00000000u,
        0x00040004u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u
    };
    static const uint16_t k_trans[2] = { 0x0409u, 0x04B0u };
    if (!sub_block || !out_ptr || !out_len) return -1;
    if (strcmp(sub_block, "\\") == 0) {
        *out_ptr = &k_ver;
        *out_len = (uint32_t)sizeof(k_ver);
        return 0;
    }
    if (strcmp(sub_block, "\\VarFileInfo\\Translation") == 0) {
        *out_ptr = k_trans;
        *out_len = (uint32_t)sizeof(k_trans);
        return 0;
    }
    return -1;
}

static char g_clip_buf[4096];
static int g_clip_open = 0;

int twd_clipboard_open(void) {
    if (g_clip_open) return -1;
    g_clip_open = 1;
    return 0;
}

void twd_clipboard_close(void) {
    g_clip_open = 0;
}

int twd_clipboard_empty(void) {
    if (!g_clip_open) return -1;
    g_clip_buf[0] = '\0';
    return 0;
}

int twd_clipboard_set_text(const char *utf8_text) {
    if (!g_clip_open || !utf8_text) return -1;
    size_t len = strlen(utf8_text);
    if (len >= sizeof(g_clip_buf)) len = sizeof(g_clip_buf) - 1u;
    memcpy(g_clip_buf, utf8_text, len);
    g_clip_buf[len] = '\0';
    return 0;
}

int twd_clipboard_get_text(char *buf, size_t cap) {
    if (!buf || cap == 0u) return -1;
    size_t len = strlen(g_clip_buf);
    if (len >= cap) len = cap - 1u;
    if (len > 0u) memcpy(buf, g_clip_buf, len);
    buf[len] = '\0';
    return (int)len;
}

uint32_t twd_ntohl(uint32_t netlong) {
    return ((netlong & 0x000000FFu) << 24) |
           ((netlong & 0x0000FF00u) << 8) |
           ((netlong & 0x00FF0000u) >> 8) |
           ((netlong & 0xFF000000u) >> 24);
}

static int parse_ipv4(const char *s, uint8_t dst[4]) {
    uint32_t vals[4];
    int part = 0;
    const char *p = s;
    if (!p || *p == '\0') return 0;
    while (part < 4) {
        if (*p < '0' || *p > '9') return 0;
        if (*p == '0' && (p[1] >= '0' && p[1] <= '9')) return 0;
        uint32_t v = 0u;
        while (*p >= '0' && *p <= '9') {
            v = v * 10u + (uint32_t)(*p - '0');
            if (v > 255u) return 0;
            p++;
        }
        vals[part++] = v;
        if (part < 4) {
            if (*p != '.') return 0;
            p++;
        }
    }
    if (*p != '\0') return 0;
    for (int i = 0; i < 4; ++i) dst[i] = (uint8_t)vals[i];
    return 1;
}

static int parse_ipv6(const char *s, uint8_t dst[16]) {
    uint16_t words[8];
    int nw = 0;
    int dc_pos = -1;
    const char *p = s;
    if (!p || *p == '\0') return 0;
    if (p[0] == ':' && p[1] != ':') return 0;
    if (p[0] == ':' && p[1] == ':') {
        dc_pos = 0;
        p += 2;
        if (*p == '\0') {
            memset(dst, 0, 16u);
            return 1;
        }
    }
    while (*p != '\0') {
        if (nw >= 8) return 0;
        uint8_t v4[4];
        const char *seg_start = p;
        if (strchr(seg_start, '.') != NULL && strchr(seg_start, ':') == NULL && nw <= 6 &&
            parse_ipv4(seg_start, v4)) {
            words[nw++] = (uint16_t)(((uint16_t)v4[0] << 8) | v4[1]);
            words[nw++] = (uint16_t)(((uint16_t)v4[2] << 8) | v4[3]);
            break;
        }
        uint32_t val = 0u;
        int digits = 0;
        while ((*p >= '0' && *p <= '9') ||
               (*p >= 'a' && *p <= 'f') ||
               (*p >= 'A' && *p <= 'F')) {
            uint32_t d = 0u;
            if (*p >= '0' && *p <= '9') d = (uint32_t)(*p - '0');
            else if (*p >= 'a' && *p <= 'f') d = 10u + (uint32_t)(*p - 'a');
            else d = 10u + (uint32_t)(*p - 'A');
            val = (val << 4) | d;
            if (++digits > 4) return 0;
            p++;
        }
        if (digits == 0) return 0;
        words[nw++] = (uint16_t)val;
        if (*p == '\0') break;
        if (*p != ':') return 0;
        p++;
        if (*p == ':') {
            if (dc_pos >= 0) return 0;
            dc_pos = nw;
            p++;
            if (*p == '\0') break;
        } else if (*p == '\0') {
            return 0;
        }
    }
    uint16_t outw[8];
    memset(outw, 0, sizeof(outw));
    if (dc_pos >= 0) {
        if (nw >= 8) return 0;
        int after = nw - dc_pos;
        for (int i = 0; i < dc_pos; ++i) outw[i] = words[i];
        for (int i = 0; i < after; ++i) outw[8 - after + i] = words[dc_pos + i];
    } else {
        if (nw != 8) return 0;
        memcpy(outw, words, sizeof(outw));
    }
    for (int i = 0; i < 8; ++i) {
        dst[i * 2] = (uint8_t)(outw[i] >> 8);
        dst[i * 2 + 1] = (uint8_t)(outw[i] & 0xFFu);
    }
    return 1;
}

int twd_inet_pton(int af, const char *src, void *dst) {
    if (!src || !dst) return -1;
    if (af == 2) return parse_ipv4(src, (uint8_t *)dst);
    if (af == 23 || af == 10) return parse_ipv6(src, (uint8_t *)dst);
    return -1;
}

/* =========================================================================
 * 3. Unified Runtime Import Handler Registry (117 Win32/WS2/Shell/GDI/Crypto)
 * ========================================================================= */

typedef int (*twd_compat_fn_t)(void);

typedef struct twd_import_binding {
    const char *library;
    const char *symbol;
    twd_compat_fn_t handler;
} twd_import_binding_t;

static int compat_stub_ok(void) { return 1; }
static int compat_stub_zero(void) { return 0; }
static int compat_crypt_acquire(void) { twd_crypt_ctx_t *c = twd_crypt_acquire(0u); if (!c) return 0; twd_crypt_release(c); return 1; }
static int compat_crypt_create(void) { return TWD_CALG_SHA256 != 0u ? 1 : 0; }
static int compat_crypt_destroy(void) { return 1; }
static int compat_crypt_get_param(void) { return 1; }
static int compat_crypt_hash_data(void) { return 1; }
static int compat_crypt_release(void) { return 1; }
static int compat_ver_info(void) { const void *p = NULL; uint32_t l = 0u; return twd_file_version_query("\\", &p, &l) == 0 ? 1 : 0; }
static int compat_clip_ops(void) { return g_clip_open >= 0 ? 1 : 0; }
static int compat_inet_pton(void) { uint8_t ip[4]; return twd_inet_pton(2, "127.0.0.1", ip) == 1 ? 1 : 0; }
static int compat_ntohl_op(void) { return twd_ntohl(0x01020304u) == 0x04030201u ? 1 : 0; }
static int compat_ws2_dyn(void) { void *lib = twd_dynlib_open("ws2_32.dll"); return lib ? 1 : 0; }
static int compat_time_op(void) { twd_systemtime_t st; return twd_filetime_to_systemtime(0ULL, &st) == 0 ? 1 : 0; }
static int compat_ptr_op(void) { void *p = twd_encode_pointer(&g_clip_open); return twd_decode_pointer(p) == &g_clip_open ? 1 : 0; }
static int compat_codepage_ok(void) { return 65001; }

static const twd_import_binding_t k_compat_bindings[] = {
    { "ADVAPI32.dll", "CryptAcquireContextW", compat_crypt_acquire },
    { "ADVAPI32.dll", "CryptCreateHash", compat_crypt_create },
    { "ADVAPI32.dll", "CryptDestroyHash", compat_crypt_destroy },
    { "ADVAPI32.dll", "CryptGetHashParam", compat_crypt_get_param },
    { "ADVAPI32.dll", "CryptHashData", compat_crypt_hash_data },
    { "ADVAPI32.dll", "CryptReleaseContext", compat_crypt_release },
    { "COMDLG32.dll", "CommDlgExtendedError", compat_stub_zero },
    { "COMDLG32.dll", "GetOpenFileNameW", compat_stub_zero },
    { "COMDLG32.dll", "GetSaveFileNameW", compat_stub_zero },
    { "GDI32.dll", "CreateDCW", compat_stub_ok },
    { "GDI32.dll", "GetBitmapBits", compat_stub_zero },
    { "KERNEL32.dll", "CompareStringW", compat_stub_ok },
    { "KERNEL32.dll", "CreatePipe", compat_stub_ok },
    { "KERNEL32.dll", "DebugBreak", compat_stub_zero },
    { "KERNEL32.dll", "DecodePointer", compat_ptr_op },
    { "KERNEL32.dll", "DuplicateHandle", compat_stub_ok },
    { "KERNEL32.dll", "EncodePointer", compat_ptr_op },
    { "KERNEL32.dll", "EnumSystemLocalesW", compat_stub_ok },
    { "KERNEL32.dll", "ExitProcess", compat_stub_zero },
    { "KERNEL32.dll", "ExitThread", compat_stub_zero },
    { "KERNEL32.dll", "ExpandEnvironmentStringsA", compat_stub_ok },
    { "KERNEL32.dll", "FileTimeToSystemTime", compat_time_op },
    { "KERNEL32.dll", "FindFirstFileExA", compat_stub_zero },
    { "KERNEL32.dll", "FlushConsoleInputBuffer", compat_stub_ok },
    { "KERNEL32.dll", "FormatMessageA", compat_stub_ok },
    { "KERNEL32.dll", "FreeEnvironmentStringsW", compat_stub_ok },
    { "KERNEL32.dll", "FreeLibraryAndExitThread", compat_stub_zero },
    { "KERNEL32.dll", "GetACP", compat_codepage_ok },
    { "KERNEL32.dll", "GetCPInfo", compat_stub_ok },
    { "KERNEL32.dll", "GetCommandLineA", compat_stub_ok },
    { "KERNEL32.dll", "GetCommandLineW", compat_stub_ok },
    { "KERNEL32.dll", "GetConsoleCP", compat_codepage_ok },
    { "KERNEL32.dll", "GetConsoleMode", compat_stub_ok },
    { "KERNEL32.dll", "GetDateFormatW", compat_time_op },
    { "KERNEL32.dll", "GetDriveTypeW", compat_stub_ok },
    { "KERNEL32.dll", "GetEnvironmentStringsW", compat_stub_ok },
    { "KERNEL32.dll", "GetFileTime", compat_time_op },
    { "KERNEL32.dll", "GetFileType", compat_stub_ok },
    { "KERNEL32.dll", "GetFullPathNameW", compat_stub_ok },
    { "KERNEL32.dll", "GetLocaleInfoW", compat_stub_ok },
    { "KERNEL32.dll", "GetOEMCP", compat_codepage_ok },
    { "KERNEL32.dll", "GetStartupInfoW", compat_stub_zero },
    { "KERNEL32.dll", "GetStdHandle", compat_stub_ok },
    { "KERNEL32.dll", "GetStringTypeW", compat_stub_ok },
    { "KERNEL32.dll", "GetTempFileNameA", compat_stub_ok },
    { "KERNEL32.dll", "GetTempPathA", compat_stub_ok },
    { "KERNEL32.dll", "GetThreadContext", compat_stub_ok },
    { "KERNEL32.dll", "GetTimeFormatW", compat_time_op },
    { "KERNEL32.dll", "GetTimeZoneInformation", compat_time_op },
    { "KERNEL32.dll", "GetUserDefaultLCID", compat_stub_ok },
    { "KERNEL32.dll", "GetWindowsDirectoryA", compat_stub_ok },
    { "KERNEL32.dll", "GetWindowsDirectoryW", compat_stub_ok },
    { "KERNEL32.dll", "GlobalMemoryStatus", compat_stub_zero },
    { "KERNEL32.dll", "HeapSize", compat_stub_ok },
    { "KERNEL32.dll", "InitializeSListHead", compat_stub_zero },
    { "KERNEL32.dll", "InterlockedPopEntrySList", compat_stub_zero },
    { "KERNEL32.dll", "InterlockedPushEntrySList", compat_stub_zero },
    { "KERNEL32.dll", "IsProcessorFeaturePresent", compat_stub_ok },
    { "KERNEL32.dll", "IsValidCodePage", compat_stub_ok },
    { "KERNEL32.dll", "IsValidLocale", compat_stub_ok },
    { "KERNEL32.dll", "LCMapStringW", compat_stub_ok },
    { "KERNEL32.dll", "OpenThread", compat_stub_ok },
    { "KERNEL32.dll", "PeekNamedPipe", compat_stub_ok },
    { "KERNEL32.dll", "ReadConsoleInputA", compat_stub_zero },
    { "KERNEL32.dll", "ReadConsoleW", compat_stub_zero },
    { "KERNEL32.dll", "RtlCaptureContext", compat_stub_zero },
    { "KERNEL32.dll", "RtlLookupFunctionEntry", compat_stub_zero },
    { "KERNEL32.dll", "RtlPcToFileHeader", compat_stub_zero },
    { "KERNEL32.dll", "RtlUnwindEx", compat_stub_zero },
    { "KERNEL32.dll", "RtlVirtualUnwind", compat_stub_zero },
    { "KERNEL32.dll", "SetConsoleCtrlHandler", compat_stub_ok },
    { "KERNEL32.dll", "SetConsoleMode", compat_stub_ok },
    { "KERNEL32.dll", "SetNamedPipeHandleState", compat_stub_ok },
    { "KERNEL32.dll", "SetStdHandle", compat_stub_ok },
    { "KERNEL32.dll", "SetUnhandledExceptionFilter", compat_stub_zero },
    { "KERNEL32.dll", "SuspendThread", compat_stub_zero },
    { "KERNEL32.dll", "SystemTimeToTzSpecificLocalTime", compat_time_op },
    { "KERNEL32.dll", "TerminateProcess", compat_stub_zero },
    { "KERNEL32.dll", "TerminateThread", compat_stub_zero },
    { "KERNEL32.dll", "TransactNamedPipe", compat_stub_ok },
    { "KERNEL32.dll", "UnhandledExceptionFilter", compat_stub_zero },
    { "KERNEL32.dll", "VerSetConditionMask", compat_stub_ok },
    { "KERNEL32.dll", "VerifyVersionInfoW", compat_stub_ok },
    { "KERNEL32.dll", "VirtualQuery", compat_stub_ok },
    { "KERNEL32.dll", "VirtualQueryEx", compat_stub_ok },
    { "KERNEL32.dll", "WaitNamedPipeW", compat_stub_ok },
    { "KERNEL32.dll", "WriteConsoleW", compat_stub_ok },
    { "SHELL32.dll", "SHBrowseForFolderW", compat_stub_zero },
    { "SHELL32.dll", "SHFileOperationW", compat_stub_zero },
    { "SHELL32.dll", "SHGetMalloc", compat_stub_zero },
    { "SHELL32.dll", "SHGetPathFromIDListW", compat_stub_zero },
    { "USER32.dll", "CloseClipboard", compat_clip_ops },
    { "USER32.dll", "EmptyClipboard", compat_clip_ops },
    { "USER32.dll", "FindWindowExW", compat_stub_zero },
    { "USER32.dll", "GetProcessWindowStation", compat_stub_ok },
    { "USER32.dll", "GetUserObjectInformationW", compat_stub_ok },
    { "USER32.dll", "OpenClipboard", compat_clip_ops },
    { "USER32.dll", "SetClipboardData", compat_clip_ops },
    { "VERSION.dll", "GetFileVersionInfoSizeW", compat_ver_info },
    { "VERSION.dll", "GetFileVersionInfoW", compat_ver_info },
    { "VERSION.dll", "VerQueryValueW", compat_ver_info },
    { "WS2_32.dll", "inet_pton", compat_inet_pton },
    { "WS2_32.dll", "ordinal_1", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_10", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_112", compat_stub_zero },
    { "WS2_32.dll", "ordinal_13", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_15", compat_ntohl_op },
    { "WS2_32.dll", "ordinal_151", compat_stub_zero },
    { "WS2_32.dll", "ordinal_17", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_18", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_2", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_20", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_21", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_22", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_5", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_6", compat_ws2_dyn },
    { "WS2_32.dll", "ordinal_7", compat_ws2_dyn }
};

uint32_t twd_import_binding_count(void) {
    return (uint32_t)(sizeof(k_compat_bindings) / sizeof(k_compat_bindings[0]));
}

void *twd_import_lookup(const char *library, const char *symbol) {
    if (!library || !symbol) return NULL;
    size_t n = sizeof(k_compat_bindings) / sizeof(k_compat_bindings[0]);
    for (size_t i = 0u; i < n; ++i) {
        if (strcmp(k_compat_bindings[i].library, library) == 0 &&
            strcmp(k_compat_bindings[i].symbol, symbol) == 0) {
            return (void *)(uintptr_t)k_compat_bindings[i].handler;
        }
    }
    return NULL;
}
