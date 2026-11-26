/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal standalone RFC 1951 raw DEFLATE (inflate) decompressor for uppercase
 * ZCTT / ECTT TTArchive2 chunks (WalkingDead4.exe compress_mode = 0).
 */
#include "twd_runtime.h"

#include <string.h>

#define INF_MAXBITS 15
#define INF_FIXLCODES 288
#define INF_MAXDCODES 32

typedef struct inf_state {
    const uint8_t *in;
    size_t in_len;
    size_t in_pos;
    uint32_t bitbuf;
    int bitcnt;

    uint8_t *out;
    size_t out_cap;
    size_t out_pos;
} inf_state_t;

typedef struct inf_huff {
    int16_t count[INF_MAXBITS + 1];
    uint16_t symbol[INF_FIXLCODES + INF_MAXDCODES];
} inf_huff_t;

static int inf_bits(inf_state_t *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) return -1;
        val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1u));
}

static int inf_construct(inf_huff_t *h, const int16_t *length, int n) {
    int16_t offs[INF_MAXBITS + 1];
    for (int len = 0; len <= INF_MAXBITS; ++len) h->count[len] = 0;
    for (int sym = 0; sym < n; ++sym) {
        if (length[sym] < 0 || length[sym] > INF_MAXBITS) return -1;
        h->count[length[sym]]++;
    }
    if (h->count[0] == n) return 0;

    int left = 1;
    for (int len = 1; len <= INF_MAXBITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }

    offs[1] = 0;
    for (int len = 1; len < INF_MAXBITS; ++len) {
        offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    }
    for (int sym = 0; sym < n; ++sym) {
        if (length[sym] != 0) {
            h->symbol[offs[length[sym]]++] = (uint16_t)sym;
        }
    }
    return 0;
}

static int inf_decode(inf_state_t *s, const inf_huff_t *h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= INF_MAXBITS; ++len) {
        int b = inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) {
            return (int)h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inf_codes(inf_state_t *s, const inf_huff_t *lencode, const inf_huff_t *distcode) {
    static const uint16_t lens[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t lext[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t dists[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t dext[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;) {
        int sym = inf_decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_cap) return -1;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int ex = inf_bits(s, lext[sym]);
            if (ex < 0) return -1;
            size_t len = (size_t)lens[sym] + (size_t)ex;
            int dsym = inf_decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            int dex = inf_bits(s, dext[dsym]);
            if (dex < 0) return -1;
            size_t dist = (size_t)dists[dsym] + (size_t)dex;
            if (dist == 0u || dist > s->out_pos || s->out_pos + len > s->out_cap) return -1;
            while (len-- > 0u) {
                uint8_t b = s->out[s->out_pos - dist];
                s->out[s->out_pos++] = b;
            }
        }
    }
}

static int inf_stored(inf_state_t *s) {
    s->bitbuf = 0u;
    s->bitcnt = 0;
    if (s->in_pos + 4u > s->in_len) return -1;
    uint16_t len = (uint16_t)s->in[s->in_pos] | ((uint16_t)s->in[s->in_pos + 1u] << 8u);
    uint16_t nlen = (uint16_t)s->in[s->in_pos + 2u] | ((uint16_t)s->in[s->in_pos + 3u] << 8u);
    s->in_pos += 4u;
    if ((uint16_t)(len ^ 0xFFFFu) != nlen) return -1;
    if (s->in_pos + (size_t)len > s->in_len || s->out_pos + (size_t)len > s->out_cap) return -1;
    if (len > 0u) {
        memcpy(s->out + s->out_pos, s->in + s->in_pos, (size_t)len);
        s->in_pos += (size_t)len;
        s->out_pos += (size_t)len;
    }
    return 0;
}

static int inf_fixed(inf_state_t *s) {
    int16_t lengths[INF_FIXLCODES + 32];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < INF_FIXLCODES; ++i) lengths[i] = 8;
    for (int i = 0; i < 32; ++i) lengths[INF_FIXLCODES + i] = 5;

    inf_huff_t lc, dc;
    if (inf_construct(&lc, lengths, INF_FIXLCODES) != 0) return -1;
    if (inf_construct(&dc, lengths + INF_FIXLCODES, 32) != 0) return -1;
    return inf_codes(s, &lc, &dc);
}

static int inf_dynamic(inf_state_t *s) {
    static const uint8_t k_order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    int nlen = inf_bits(s, 5);
    int ndist = inf_bits(s, 5);
    int ncode = inf_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > INF_FIXLCODES || ndist > 30) return -1;

    int16_t lengths[INF_FIXLCODES + 30];
    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; ++i) {
        int v = inf_bits(s, 3);
        if (v < 0) return -1;
        lengths[k_order[i]] = (int16_t)v;
    }
    inf_huff_t lc;
    if (inf_construct(&lc, lengths, 19) != 0) return -1;

    int index = 0;
    int total = nlen + ndist;
    while (index < total) {
        int sym = inf_decode(s, &lc);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[index++] = (int16_t)sym;
        } else {
            int len = 0;
            int rep = 0;
            if (sym == 16) {
                if (index == 0) return -1;
                len = lengths[index - 1];
                int b = inf_bits(s, 2);
                if (b < 0) return -1;
                rep = 3 + b;
            } else if (sym == 17) {
                int b = inf_bits(s, 3);
                if (b < 0) return -1;
                rep = 3 + b;
            } else {
                int b = inf_bits(s, 7);
                if (b < 0) return -1;
                rep = 11 + b;
            }
            if (index + rep > total) return -1;
            while (rep-- > 0) lengths[index++] = (int16_t)len;
        }
    }

    inf_huff_t lencode, distcode;
    if (lengths[256] == 0) return -1;
    if (inf_construct(&lencode, lengths, nlen) != 0) return -1;
    if (inf_construct(&distcode, lengths + nlen, ndist) != 0) return -1;
    return inf_codes(s, &lencode, &distcode);
}

int twd_inflate_raw(
    const void *src, size_t src_len, void *dst, size_t dst_cap, size_t *out_written) {
    if (!src || src_len == 0u || !dst || dst_cap == 0u) return -1;
    inf_state_t s;
    memset(&s, 0, sizeof(s));
    s.in = (const uint8_t *)src;
    s.in_len = src_len;
    s.out = (uint8_t *)dst;
    s.out_cap = dst_cap;

    int last = 0;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        int rc = -1;
        if (type == 0) rc = inf_stored(&s);
        else if (type == 1) rc = inf_fixed(&s);
        else if (type == 2) rc = inf_dynamic(&s);
        if (rc != 0) return -1;
    } while (!last);

    if (out_written) *out_written = s.out_pos;
    return 0;
}
