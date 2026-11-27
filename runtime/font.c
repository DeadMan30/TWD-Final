/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

static uint32_t font_rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t font_rd_u64(const uint8_t *p) {
    return (uint64_t)font_rd_u32(p) | ((uint64_t)font_rd_u32(p + 4u) << 32u);
}

static int32_t font_rd_i32(const uint8_t *p) {
    uint32_t v = font_rd_u32(p);
    int32_t out = 0;
    memcpy(&out, &v, sizeof(out));
    return out;
}

static float font_rd_f32(const uint8_t *p) {
    uint32_t v = font_rd_u32(p);
    float out = 0.0f;
    memcpy(&out, &v, sizeof(out));
    return out;
}

static uint16_t ttf_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8u) | (uint16_t)p[1]);
}

static int16_t ttf_i16(const uint8_t *p) {
    uint16_t u = ttf_be16(p);
    int16_t s = 0;
    memcpy(&s, &u, sizeof(s));
    return s;
}

static uint32_t ttf_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u) | (uint32_t)p[3];
}

static int32_t ttf_ifloor(float x) {
    int32_t i = (int32_t)x;
    return ((float)i > x) ? (i - 1) : i;
}

static int32_t ttf_iceil(float x) {
    int32_t i = (int32_t)x;
    return ((float)i < x) ? (i + 1) : i;
}

#define TWD_TTF_CACHE_CAP 256u

typedef struct ttf_glyph_cache_entry {
    uint32_t codepoint;
    uint32_t px_round;
    int32_t x_off;
    int32_t y_off;
    int32_t gw;
    int32_t gh;
    float x_adv;
    uint8_t *alpha;
    int valid;
} ttf_glyph_cache_entry_t;

typedef struct twd_ttf_ctx {
    uint8_t *data;
    size_t size;
    uint32_t head_off, head_len;
    uint32_t maxp_off, maxp_len;
    uint32_t hhea_off, hhea_len;
    uint32_t hmtx_off, hmtx_len;
    uint32_t cmap_off, cmap_len;
    uint32_t loca_off, loca_len;
    uint32_t glyf_off, glyf_len;
    uint32_t cmap_fmt4_off;
    uint32_t cmap_fmt12_off;
    uint16_t units_per_em;
    int16_t index_to_loc_fmt;
    uint16_t num_glyphs;
    uint16_t num_hmetrics;
    int16_t ascent;
    int16_t descent;
    int16_t line_gap;
    float default_px;
    float ascent_ratio;
    ttf_glyph_cache_entry_t cache[TWD_TTF_CACHE_CAP];
    uint32_t cache_cursor;
} twd_ttf_ctx_t;

typedef struct ttf_edge {
    float x0, y0, x1, y1;
} ttf_edge_t;

typedef struct ttf_edge_vec {
    ttf_edge_t *items;
    uint32_t count;
    uint32_t cap;
} ttf_edge_vec_t;

typedef struct ttf_pt {
    float x, y;
    uint8_t on_curve;
} ttf_pt_t;

static int ttf_find_table(const uint8_t *buf, size_t size, uint32_t sfnt_off,
                          const char tag[4], uint32_t *out_off, uint32_t *out_len) {
    if (sfnt_off > size || size - sfnt_off < 12u) return -1;
    uint16_t ntab = ttf_be16(buf + sfnt_off + 4u);
    if ((size_t)ntab * 16u > size - sfnt_off - 12u) return -1;
    for (uint16_t i = 0u; i < ntab; ++i) {
        const uint8_t *rec = buf + sfnt_off + 12u + (size_t)i * 16u;
        if (memcmp(rec, tag, 4u) == 0) {
            uint32_t off = ttf_be32(rec + 8u);
            uint32_t len = ttf_be32(rec + 12u);
            if (off <= size && len <= size - off) {
                *out_off = off;
                *out_len = len;
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

static void ttf_ctx_destroy(twd_ttf_ctx_t *ctx) {
    if (!ctx) return;
    for (uint32_t i = 0u; i < TWD_TTF_CACHE_CAP; ++i) {
        free(ctx->cache[i].alpha);
        ctx->cache[i].alpha = NULL;
        ctx->cache[i].valid = 0;
    }
    free(ctx->data);
    free(ctx);
}

static twd_ttf_ctx_t *ttf_ctx_create(const uint8_t *src, size_t len, float base_pt,
                                     float *io_height, float *io_base) {
    if (!src || len < 16u || len > 64u * 1024u * 1024u) return NULL;
    twd_ttf_ctx_t *ctx = (twd_ttf_ctx_t *)calloc(1u, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->data = (uint8_t *)malloc(len);
    if (!ctx->data) {
        free(ctx);
        return NULL;
    }
    memcpy(ctx->data, src, len);
    ctx->size = len;

    uint32_t sfnt_off = 0u;
    if (memcmp(ctx->data, "ttcf", 4u) == 0) {
        if (len < 16u) { ttf_ctx_destroy(ctx); return NULL; }
        uint32_t nfonts = ttf_be32(ctx->data + 8u);
        if (nfonts == 0u) { ttf_ctx_destroy(ctx); return NULL; }
        sfnt_off = ttf_be32(ctx->data + 12u);
    }
    if (sfnt_off > len || len - sfnt_off < 12u) { ttf_ctx_destroy(ctx); return NULL; }
    uint32_t sfnt_ver = ttf_be32(ctx->data + sfnt_off);
    if (sfnt_ver != 0x00010000u && sfnt_ver != 0x74727565u) {
        ttf_ctx_destroy(ctx);
        return NULL;
    }

    if (ttf_find_table(ctx->data, len, sfnt_off, "head", &ctx->head_off, &ctx->head_len) != 0 ||
        ttf_find_table(ctx->data, len, sfnt_off, "maxp", &ctx->maxp_off, &ctx->maxp_len) != 0 ||
        ttf_find_table(ctx->data, len, sfnt_off, "hhea", &ctx->hhea_off, &ctx->hhea_len) != 0 ||
        ttf_find_table(ctx->data, len, sfnt_off, "hmtx", &ctx->hmtx_off, &ctx->hmtx_len) != 0 ||
        ttf_find_table(ctx->data, len, sfnt_off, "cmap", &ctx->cmap_off, &ctx->cmap_len) != 0 ||
        ttf_find_table(ctx->data, len, sfnt_off, "loca", &ctx->loca_off, &ctx->loca_len) != 0 ||
        ttf_find_table(ctx->data, len, sfnt_off, "glyf", &ctx->glyf_off, &ctx->glyf_len) != 0) {
        ttf_ctx_destroy(ctx);
        return NULL;
    }
    if (ctx->head_len < 54u || ctx->maxp_len < 6u || ctx->hhea_len < 36u || ctx->cmap_len < 4u) {
        ttf_ctx_destroy(ctx);
        return NULL;
    }

    ctx->units_per_em = ttf_be16(ctx->data + ctx->head_off + 18u);
    ctx->index_to_loc_fmt = ttf_i16(ctx->data + ctx->head_off + 50u);
    ctx->num_glyphs = ttf_be16(ctx->data + ctx->maxp_off + 4u);
    ctx->ascent = ttf_i16(ctx->data + ctx->hhea_off + 4u);
    ctx->descent = ttf_i16(ctx->data + ctx->hhea_off + 6u);
    ctx->line_gap = ttf_i16(ctx->data + ctx->hhea_off + 8u);
    ctx->num_hmetrics = ttf_be16(ctx->data + ctx->hhea_off + 34u);
    if (ctx->units_per_em == 0u || ctx->num_glyphs == 0u) {
        ttf_ctx_destroy(ctx);
        return NULL;
    }

    uint16_t n_sub = ttf_be16(ctx->data + ctx->cmap_off + 2u);
    for (uint16_t i = 0u; i < n_sub && 4u + (uint32_t)(i + 1u) * 8u <= ctx->cmap_len; ++i) {
        const uint8_t *rec = ctx->data + ctx->cmap_off + 4u + (size_t)i * 8u;
        uint16_t pid = ttf_be16(rec);
        uint16_t eid = ttf_be16(rec + 2u);
        uint32_t rel = ttf_be32(rec + 4u);
        if (rel >= ctx->cmap_len || ctx->cmap_len - rel < 4u) continue;
        uint32_t abs_off = ctx->cmap_off + rel;
        uint16_t fmt = ttf_be16(ctx->data + abs_off);
        if (fmt == 12u && (pid == 3u || pid == 0u) && ctx->cmap_fmt12_off == 0u) {
            ctx->cmap_fmt12_off = abs_off;
        } else if (fmt == 4u && ((pid == 3u && eid == 1u) || pid == 0u)) {
            if (ctx->cmap_fmt4_off == 0u || pid == 3u) ctx->cmap_fmt4_off = abs_off;
        }
    }
    if (ctx->cmap_fmt4_off == 0u && ctx->cmap_fmt12_off == 0u) {
        ttf_ctx_destroy(ctx);
        return NULL;
    }

    float px = (base_pt > 0.0f) ? base_pt : ((io_height && *io_height > 0.0f) ? *io_height : 24.0f);
    ctx->default_px = px;
    ctx->ascent_ratio = (float)ctx->ascent / (float)ctx->units_per_em;
    if (io_height && *io_height <= 0.0f) {
        float h = (float)((int32_t)ctx->ascent - (int32_t)ctx->descent + (int32_t)ctx->line_gap) *
                  px / (float)ctx->units_per_em;
        *io_height = (h > 0.0f) ? h : px;
    }
    if (io_base && *io_base <= 0.0f) {
        float b = (float)ctx->ascent * px / (float)ctx->units_per_em;
        *io_base = (b > 0.0f) ? b : px * 0.8f;
    }
    return ctx;
}

static uint16_t ttf_cmap_lookup(const twd_ttf_ctx_t *ctx, uint32_t cp) {
    if (ctx->cmap_fmt12_off != 0u && ctx->cmap_fmt12_off + 16u <= ctx->size) {
        const uint8_t *t = ctx->data + ctx->cmap_fmt12_off;
        uint32_t ngrp = ttf_be32(t + 12u);
        uint32_t avail = (uint32_t)(ctx->size - (ctx->cmap_fmt12_off + 16u)) / 12u;
        if (ngrp > avail) ngrp = avail;
        uint32_t lo = 0u, hi = ngrp;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2u;
            const uint8_t *g = t + 16u + (size_t)mid * 12u;
            uint32_t sc = ttf_be32(g);
            uint32_t ec = ttf_be32(g + 4u);
            if (cp < sc) hi = mid;
            else if (cp > ec) lo = mid + 1u;
            else {
                uint32_t sg = ttf_be32(g + 8u);
                uint32_t gid = sg + (cp - sc);
                return (gid < ctx->num_glyphs) ? (uint16_t)gid : 0u;
            }
        }
    }
    if (ctx->cmap_fmt4_off != 0u && cp <= 0xFFFFu && ctx->cmap_fmt4_off + 14u <= ctx->size) {
        const uint8_t *t = ctx->data + ctx->cmap_fmt4_off;
        uint16_t seg_x2 = ttf_be16(t + 6u);
        uint16_t nseg = (uint16_t)(seg_x2 / 2u);
        size_t need = 16u + (size_t)nseg * 8u;
        if (nseg > 0u && ctx->cmap_fmt4_off + need <= ctx->size) {
            const uint8_t *end_codes = t + 14u;
            const uint8_t *start_codes = end_codes + (size_t)nseg * 2u + 2u;
            const uint8_t *id_deltas = start_codes + (size_t)nseg * 2u;
            const uint8_t *id_ranges = id_deltas + (size_t)nseg * 2u;
            for (uint16_t i = 0u; i < nseg; ++i) {
                uint16_t ec = ttf_be16(end_codes + (size_t)i * 2u);
                uint16_t sc = ttf_be16(start_codes + (size_t)i * 2u);
                if (cp < sc || cp > ec) continue;
                int16_t delta = ttf_i16(id_deltas + (size_t)i * 2u);
                uint16_t roff = ttf_be16(id_ranges + (size_t)i * 2u);
                if (roff == 0u) {
                    uint16_t gid = (uint16_t)((uint16_t)cp + (uint16_t)delta);
                    return (gid < ctx->num_glyphs) ? gid : 0u;
                }
                size_t ptr_off = (size_t)(id_ranges + (size_t)i * 2u - ctx->data) +
                                 (size_t)roff + (size_t)(cp - sc) * 2u;
                if (ptr_off + 2u <= ctx->size) {
                    uint16_t gid = ttf_be16(ctx->data + ptr_off);
                    if (gid != 0u) gid = (uint16_t)(gid + (uint16_t)delta);
                    return (gid < ctx->num_glyphs) ? gid : 0u;
                }
                return 0u;
            }
        }
    }
    return 0u;
}

static uint16_t ttf_hmetric_adv(const twd_ttf_ctx_t *ctx, uint16_t gid) {
    if (ctx->num_hmetrics == 0u || ctx->hmtx_len < 4u) return ctx->units_per_em / 2u;
    uint16_t idx = (gid < ctx->num_hmetrics) ? gid : (uint16_t)(ctx->num_hmetrics - 1u);
    size_t off = (size_t)ctx->hmtx_off + (size_t)idx * 4u;
    if (off + 2u > ctx->size) return ctx->units_per_em / 2u;
    return ttf_be16(ctx->data + off);
}

static int ttf_edge_push(ttf_edge_vec_t *v, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return 0;
    if (v->count >= v->cap) {
        uint32_t ncap = v->cap ? v->cap * 2u : 128u;
        ttf_edge_t *ni = (ttf_edge_t *)realloc(v->items, (size_t)ncap * sizeof(ttf_edge_t));
        if (!ni) return -1;
        v->items = ni;
        v->cap = ncap;
    }
    v->items[v->count].x0 = x0;
    v->items[v->count].y0 = y0;
    v->items[v->count].x1 = x1;
    v->items[v->count].y1 = y1;
    v->count++;
    return 0;
}

static void ttf_emit_quad(ttf_edge_vec_t *v, float x0, float y0, float x1, float y1,
                          float x2, float y2, float px_scale,
                          float a, float b, float c, float d, float tx, float ty) {
    enum { NSEG = 6 };
    float sx = (x0 * a + y0 * c + tx) * px_scale;
    float sy = -(x0 * b + y0 * d + ty) * px_scale;
    for (int s = 1; s <= NSEG; ++s) {
        float t = (float)s / (float)NSEG;
        float it = 1.0f - t;
        float qx = it * it * x0 + 2.0f * it * t * x1 + t * t * x2;
        float qy = it * it * y0 + 2.0f * it * t * y1 + t * t * y2;
        float nx = (qx * a + qy * c + tx) * px_scale;
        float ny = -(qx * b + qy * d + ty) * px_scale;
        (void)ttf_edge_push(v, sx, sy, nx, ny);
        sx = nx;
        sy = ny;
    }
}

static void ttf_decode_glyph(const twd_ttf_ctx_t *ctx, uint16_t gid, float px_scale,
                             float ma, float mb, float mc, float md, float mtx, float mty,
                             int depth, ttf_edge_vec_t *out) {
    if (depth > 4 || gid >= ctx->num_glyphs) return;
    uint32_t g0 = 0u, g1 = 0u;
    if (ctx->index_to_loc_fmt == 0) {
        size_t off = (size_t)ctx->loca_off + (size_t)gid * 2u;
        if (off + 4u > ctx->loca_off + ctx->loca_len) return;
        g0 = (uint32_t)ttf_be16(ctx->data + off) * 2u;
        g1 = (uint32_t)ttf_be16(ctx->data + off + 2u) * 2u;
    } else {
        size_t off = (size_t)ctx->loca_off + (size_t)gid * 4u;
        if (off + 8u > ctx->loca_off + ctx->loca_len) return;
        g0 = ttf_be32(ctx->data + off);
        g1 = ttf_be32(ctx->data + off + 4u);
    }
    if (g1 <= g0 || g1 > ctx->glyf_len || g1 - g0 < 10u) return;

    const uint8_t *g = ctx->data + ctx->glyf_off + g0;
    const uint8_t *gend = ctx->data + ctx->glyf_off + g1;
    int16_t ncont = ttf_i16(g);
    const uint8_t *p = g + 10u;

    if (ncont < 0) {
        /* Compound glyph */
        uint16_t flags = 0u;
        do {
            if (p + 4u > gend) break;
            flags = ttf_be16(p);
            uint16_t sub_gid = ttf_be16(p + 2u);
            p += 4u;
            int32_t arg1 = 0, arg2 = 0;
            if (flags & 0x0001u) {
                if (p + 4u > gend) break;
                arg1 = ttf_i16(p);
                arg2 = ttf_i16(p + 2u);
                p += 4u;
            } else {
                if (p + 2u > gend) break;
                arg1 = (int8_t)p[0];
                arg2 = (int8_t)p[1];
                p += 2u;
            }
            float ca = 1.0f, cb = 0.0f, cc = 0.0f, cd = 1.0f;
            if (flags & 0x0008u) {
                if (p + 2u > gend) break;
                ca = cd = (float)ttf_i16(p) / 16384.0f;
                p += 2u;
            } else if (flags & 0x0040u) {
                if (p + 4u > gend) break;
                ca = (float)ttf_i16(p) / 16384.0f;
                cd = (float)ttf_i16(p + 2u) / 16384.0f;
                p += 4u;
            } else if (flags & 0x0080u) {
                if (p + 8u > gend) break;
                ca = (float)ttf_i16(p) / 16384.0f;
                cb = (float)ttf_i16(p + 2u) / 16384.0f;
                cc = (float)ttf_i16(p + 4u) / 16384.0f;
                cd = (float)ttf_i16(p + 6u) / 16384.0f;
                p += 8u;
            }
            float dx = (flags & 0x0002u) ? (float)arg1 : 0.0f;
            float dy = (flags & 0x0002u) ? (float)arg2 : 0.0f;
            float na = ca * ma + cb * mc;
            float nb = ca * mb + cb * md;
            float nc = cc * ma + cd * mc;
            float nd = cc * mb + cd * md;
            float ntx = dx * ma + dy * mc + mtx;
            float nty = dx * mb + dy * md + mty;
            ttf_decode_glyph(ctx, sub_gid, px_scale, na, nb, nc, nd, ntx, nty, depth + 1, out);
        } while (flags & 0x0020u);
        return;
    }

    if (ncont == 0 || (size_t)ncont * 2u + 2u > (size_t)(gend - p)) return;
    uint16_t last_pt = ttf_be16(p + (size_t)(ncont - 1) * 2u);
    uint32_t npts = (uint32_t)last_pt + 1u;
    if (npts > 8192u) return;
    const uint8_t *end_pts = p;
    p += (size_t)ncont * 2u;
    uint16_t inst_len = ttf_be16(p);
    p += 2u;
    if ((size_t)inst_len > (size_t)(gend - p)) return;
    p += inst_len;

    uint8_t *flags = (uint8_t *)malloc(npts);
    ttf_pt_t *pts = (ttf_pt_t *)calloc(npts, sizeof(ttf_pt_t));
    if (!flags || !pts) {
        free(flags);
        free(pts);
        return;
    }

    uint32_t fi = 0u;
    while (fi < npts && p < gend) {
        uint8_t fl = *p++;
        flags[fi++] = fl;
        if ((fl & 0x08u) && p < gend) {
            uint8_t rep = *p++;
            while (rep-- > 0u && fi < npts) flags[fi++] = fl;
        }
    }
    if (fi < npts) { free(flags); free(pts); return; }

    int32_t cx = 0;
    for (uint32_t i = 0u; i < npts; ++i) {
        uint8_t fl = flags[i];
        int32_t dx = 0;
        if (fl & 0x02u) {
            if (p >= gend) { free(flags); free(pts); return; }
            dx = (fl & 0x10u) ? (int32_t)(*p++) : -(int32_t)(*p++);
        } else if (!(fl & 0x10u)) {
            if (p + 2u > gend) { free(flags); free(pts); return; }
            dx = ttf_i16(p);
            p += 2u;
        }
        cx += dx;
        pts[i].x = (float)cx;
        pts[i].on_curve = (fl & 0x01u) ? 1u : 0u;
    }

    int32_t cy = 0;
    for (uint32_t i = 0u; i < npts; ++i) {
        uint8_t fl = flags[i];
        int32_t dy = 0;
        if (fl & 0x04u) {
            if (p >= gend) { free(flags); free(pts); return; }
            dy = (fl & 0x20u) ? (int32_t)(*p++) : -(int32_t)(*p++);
        } else if (!(fl & 0x20u)) {
            if (p + 2u > gend) { free(flags); free(pts); return; }
            dy = ttf_i16(p);
            p += 2u;
        }
        cy += dy;
        pts[i].y = (float)cy;
    }

    uint32_t start = 0u;
    for (int16_t c = 0; c < ncont; ++c) {
        uint32_t end = (uint32_t)ttf_be16(end_pts + (size_t)c * 2u);
        if (end < start || end >= npts) break;
        uint32_t len = (end - start) + 1u;
        if (len >= 2u) {
            uint32_t idx0 = start;
            ttf_pt_t cur;
            if (pts[start].on_curve) {
                cur = pts[start];
                idx0 = start + 1u;
            } else if (pts[end].on_curve) {
                cur = pts[end];
            } else {
                cur.x = 0.5f * (pts[start].x + pts[end].x);
                cur.y = 0.5f * (pts[start].y + pts[end].y);
                cur.on_curve = 1u;
            }
            ttf_pt_t first_pt = cur;
            uint32_t steps = (pts[start].on_curve) ? (len - 1u) : len;
            ttf_pt_t off = {0.0f, 0.0f, 0u};
            int has_off = 0;
            for (uint32_t s = 0u; s < steps; ++s) {
                uint32_t idx = start + ((idx0 - start + s) % len);
                ttf_pt_t pt = pts[idx];
                if (pt.on_curve) {
                    if (has_off) {
                        ttf_emit_quad(out, cur.x, cur.y, off.x, off.y, pt.x, pt.y,
                                      px_scale, ma, mb, mc, md, mtx, mty);
                        has_off = 0;
                    } else {
                        float x0 = (cur.x * ma + cur.y * mc + mtx) * px_scale;
                        float y0 = -(cur.x * mb + cur.y * md + mty) * px_scale;
                        float x1 = (pt.x * ma + pt.y * mc + mtx) * px_scale;
                        float y1 = -(pt.x * mb + pt.y * md + mty) * px_scale;
                        (void)ttf_edge_push(out, x0, y0, x1, y1);
                    }
                    cur = pt;
                } else {
                    if (has_off) {
                        ttf_pt_t mid;
                        mid.x = 0.5f * (off.x + pt.x);
                        mid.y = 0.5f * (off.y + pt.y);
                        ttf_emit_quad(out, cur.x, cur.y, off.x, off.y, mid.x, mid.y,
                                      px_scale, ma, mb, mc, md, mtx, mty);
                        cur = mid;
                    }
                    off = pt;
                    has_off = 1;
                }
            }
            if (has_off) {
                ttf_emit_quad(out, cur.x, cur.y, off.x, off.y, first_pt.x, first_pt.y,
                              px_scale, ma, mb, mc, md, mtx, mty);
            } else if (cur.x != first_pt.x || cur.y != first_pt.y) {
                float x0 = (cur.x * ma + cur.y * mc + mtx) * px_scale;
                float y0 = -(cur.x * mb + cur.y * md + mty) * px_scale;
                float x1 = (first_pt.x * ma + first_pt.y * mc + mtx) * px_scale;
                float y1 = -(first_pt.x * mb + first_pt.y * md + mty) * px_scale;
                (void)ttf_edge_push(out, x0, y0, x1, y1);
            }
        }
        start = end + 1u;
    }
    free(flags);
    free(pts);
}

typedef struct ttf_crossing {
    float x;
    int dir;
} ttf_crossing_t;

static const ttf_glyph_cache_entry_t *ttf_get_glyph(twd_ttf_ctx_t *ctx, uint32_t cp, float px_h) {
    if (!ctx) return NULL;
    if (px_h < 6.0f) px_h = 6.0f;
    if (px_h > 128.0f) px_h = 128.0f;
    uint32_t px_round = (uint32_t)(px_h + 0.5f);

    for (uint32_t i = 0u; i < TWD_TTF_CACHE_CAP; ++i) {
        if (ctx->cache[i].valid && ctx->cache[i].codepoint == cp &&
            ctx->cache[i].px_round == px_round) {
            return &ctx->cache[i];
        }
    }

    uint32_t slot = ctx->cache_cursor % TWD_TTF_CACHE_CAP;
    ctx->cache_cursor++;
    ttf_glyph_cache_entry_t *ent = &ctx->cache[slot];
    free(ent->alpha);
    memset(ent, 0, sizeof(*ent));
    ent->codepoint = cp;
    ent->px_round = px_round;
    ent->valid = 1;

    uint16_t gid = ttf_cmap_lookup(ctx, cp);
    float px_scale = (float)px_round / (float)ctx->units_per_em;
    ent->x_adv = (float)ttf_hmetric_adv(ctx, gid) * px_scale;

    ttf_edge_vec_t edges = {NULL, 0u, 0u};
    ttf_decode_glyph(ctx, gid, px_scale, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0, &edges);
    if (edges.count == 0u) {
        free(edges.items);
        return ent;
    }

    float minx = edges.items[0].x0, maxx = edges.items[0].x0;
    float miny = edges.items[0].y0, maxy = edges.items[0].y0;
    for (uint32_t i = 0u; i < edges.count; ++i) {
        const ttf_edge_t *e = &edges.items[i];
        if (e->x0 < minx) minx = e->x0;
        if (e->x0 > maxx) maxx = e->x0;
        if (e->x1 < minx) minx = e->x1;
        if (e->x1 > maxx) maxx = e->x1;
        if (e->y0 < miny) miny = e->y0;
        if (e->y0 > maxy) maxy = e->y0;
        if (e->y1 < miny) miny = e->y1;
        if (e->y1 > maxy) maxy = e->y1;
    }

    int32_t ix0 = ttf_ifloor(minx);
    int32_t iy0 = ttf_ifloor(miny);
    int32_t ix1 = ttf_iceil(maxx);
    int32_t iy1 = ttf_iceil(maxy);
    int32_t gw = ix1 - ix0;
    int32_t gh = iy1 - iy0;
    if (gw <= 0 || gh <= 0 || gw > 256 || gh > 256) {
        free(edges.items);
        return ent;
    }

    uint8_t *alpha = (uint8_t *)calloc((size_t)gw * (size_t)gh, 1u);
    if (!alpha) {
        free(edges.items);
        return ent;
    }

    uint16_t *row_cov = (uint16_t *)calloc((size_t)gw, sizeof(uint16_t));
    ttf_crossing_t xings[512];
    if (!row_cov) {
        free(alpha);
        free(edges.items);
        return ent;
    }

    for (int32_t row = 0; row < gh; ++row) {
        memset(row_cov, 0, (size_t)gw * sizeof(uint16_t));
        for (int sy = 0; sy < 4; ++sy) {
            float py = (float)(iy0 + row) + ((float)sy + 0.5f) * 0.25f;
            uint32_t nx = 0u;
            for (uint32_t ei = 0u; ei < edges.count && nx < 512u; ++ei) {
                const ttf_edge_t *e = &edges.items[ei];
                int down = (e->y0 <= py && e->y1 > py);
                int up = (e->y1 <= py && e->y0 > py);
                if (!down && !up) continue;
                float t = (py - e->y0) / (e->y1 - e->y0);
                xings[nx].x = e->x0 + t * (e->x1 - e->x0);
                xings[nx].dir = down ? 1 : -1;
                nx++;
            }
            for (uint32_t i = 1u; i < nx; ++i) {
                ttf_crossing_t key = xings[i];
                uint32_t j = i;
                while (j > 0u && xings[j - 1u].x > key.x) {
                    xings[j] = xings[j - 1u];
                    j--;
                }
                xings[j] = key;
            }
            uint32_t xi = 0u;
            int wind = 0;
            for (int32_t col = 0; col < gw; ++col) {
                for (int sx = 0; sx < 4; ++sx) {
                    float px = (float)(ix0 + col) + ((float)sx + 0.5f) * 0.25f;
                    while (xi < nx && xings[xi].x <= px) {
                        wind += xings[xi].dir;
                        xi++;
                    }
                    if (wind != 0) row_cov[col]++;
                }
            }
        }
        for (int32_t col = 0; col < gw; ++col) {
            uint16_t c = row_cov[col];
            alpha[(size_t)row * (size_t)gw + (size_t)col] = (uint8_t)((c * 255u + 8u) / 16u);
        }
    }

    free(row_cov);
    free(edges.items);
    ent->x_off = ix0;
    ent->y_off = iy0;
    ent->gw = gw;
    ent->gh = gh;
    ent->alpha = alpha;
    return ent;
}

static int font_read_block(const uint8_t *buf, size_t limit, size_t *cursor,
                           size_t *out_start, size_t *out_end) {
    if (*cursor + 4u > limit) return -1;
    uint32_t blk_sz = font_rd_u32(buf + *cursor);
    *cursor += 4u;
    if (blk_sz < 4u) return -1;
    size_t pay = (size_t)(blk_sz - 4u);
    if (pay > limit - *cursor) return -1;
    if (out_start) *out_start = *cursor;
    if (out_end) *out_end = *cursor + pay;
    *cursor += pay;
    return 0;
}

void twd_font_free(twd_font_t *font) {
    if (!font) return;
    ttf_ctx_destroy((twd_ttf_ctx_t *)font->ttf_ctx);
    free(font->glyphs);
    memset(font, 0, sizeof(*font));
}

int twd_font_parse_memory(const void *data, size_t size, twd_font_t *out) {
    if (!data || !out || size < 16u) return -1;
    memset(out, 0, sizeof(*out));

    twd_metastream_header_t ms;
    if (twd_metastream_parse_header(data, size, &ms) != 0) {
        return -1;
    }

    const uint8_t *buf = (const uint8_t *)data;
    size_t pos = ms.payload_offset;
    size_t limit = size;
    if (ms.default_size > 0u && ms.payload_offset <= size &&
        (size_t)ms.default_size <= size - ms.payload_offset) {
        limit = ms.payload_offset + (size_t)ms.default_size;
    }

    /* mName (blocked String) */
    size_t bs = 0u, be = 0u;
    if (font_read_block(buf, limit, &pos, &bs, &be) != 0) return -1;
    if (be - bs >= 4u) {
        uint32_t slen = font_rd_u32(buf + bs);
        size_t avail = (be - bs) - 4u;
        size_t cp = ((size_t)slen <= avail) ? (size_t)slen : avail;
        if (cp >= sizeof(out->name)) cp = sizeof(out->name) - 1u;
        if (cp > 0u) memcpy(out->name, buf + bs + 4u, cp);
        out->name[cp] = '\0';
    }

    /* mbUnicode (bool), mHeight (f32), mBase (f32) = 9 bytes */
    if (pos + 9u > limit) return -1;
    out->is_unicode = buf[pos] ? 1 : 0;
    out->height = font_rd_f32(buf + pos + 1u);
    out->base = font_rd_f32(buf + pos + 5u);
    pos += 9u;

    /* mGlyphInfo (blocked Map<Key, GlyphInfo>) */
    if (font_read_block(buf, limit, &pos, &bs, &be) != 0) return -1;
    if (be - bs >= 4u) {
        uint32_t cnt = font_rd_u32(buf + bs);
        size_t map_bytes = (be - bs) - 4u;
        if (cnt > 0u && cnt <= 65536u && map_bytes >= (size_t)cnt * 44u) {
            size_t entry_stride = map_bytes / (size_t)cnt;
            if (entry_stride >= 48u && entry_stride * (size_t)cnt <= map_bytes) {
                size_t key_sz = entry_stride - 44u;
                out->glyphs = (twd_font_glyph_t *)calloc((size_t)cnt, sizeof(twd_font_glyph_t));
                if (!out->glyphs) return -1;
                const uint8_t *mp = buf + bs + 4u;
                for (uint32_t i = 0u; i < cnt; ++i) {
                    const uint8_t *ep = mp + (size_t)i * entry_stride;
                    uint32_t cp = (key_sz >= 8u) ? (uint32_t)font_rd_u64(ep) : font_rd_u32(ep);
                    const uint8_t *gp = ep + key_sz;
                    twd_font_glyph_t *g = &out->glyphs[i];
                    g->codepoint = cp;
                    g->texture_page = font_rd_i32(gp + 0u);
                    g->channel = font_rd_i32(gp + 4u);
                    g->u0 = font_rd_f32(gp + 8u);
                    g->u1 = font_rd_f32(gp + 12u);
                    g->v0 = font_rd_f32(gp + 16u);
                    g->v1 = font_rd_f32(gp + 20u);
                    g->width = font_rd_f32(gp + 24u);
                    g->height = font_rd_f32(gp + 28u);
                    g->x_offset = font_rd_f32(gp + 32u);
                    g->y_offset = font_rd_f32(gp + 36u);
                    g->x_advance = font_rd_f32(gp + 40u);
                }
                out->glyph_count = cnt;
            }
        }
    }

    /* mTexturePages (blocked DCArray<Handle<T3Texture>>) */
    if (font_read_block(buf, limit, &pos, &bs, &be) != 0) {
        twd_font_free(out);
        return -1;
    }
    if (be - bs >= 4u) {
        uint32_t pcnt = font_rd_u32(buf + bs);
        size_t pay = (be - bs) - 4u;
        if (pcnt > 0u && pay >= (size_t)pcnt * 8u) {
            size_t stride = pay / (size_t)pcnt;
            uint32_t keep = (pcnt <= TWD_FONT_MAX_PAGES) ? pcnt : TWD_FONT_MAX_PAGES;
            for (uint32_t i = 0u; i < keep; ++i) {
                out->page_hashes[i] = font_rd_u64(buf + bs + 4u + (size_t)i * stride);
            }
            out->page_count = keep;
        }
    }

    /* mIsDistanceField, mIsRuntime, mIsFiltered */
    if (pos + 3u <= limit) {
        out->is_distance_field = buf[pos + 0u] ? 1 : 0;
        out->is_filtered = buf[pos + 2u] ? 1 : 0;
        pos += 3u;
    }

    /* Optional mTtfData (blocked BinaryBuffer), mBasePointSize (f32), mPreferredPointSizes */
    const uint8_t *ttf_ptr = NULL;
    size_t ttf_len = 0u;
    if (pos + 4u <= limit && font_read_block(buf, limit, &pos, &bs, &be) == 0) {
        if (be - bs >= 4u) {
            int32_t ttf_sz = font_rd_i32(buf + bs);
            if (ttf_sz > 0) {
                out->has_ttf_data = 1;
                if ((size_t)ttf_sz <= (be - bs) - 4u) {
                    ttf_ptr = buf + bs + 4u;
                    ttf_len = (size_t)ttf_sz;
                }
            }
        }
        if (pos + 4u <= limit) {
            out->base_point_size = font_rd_f32(buf + pos);
            pos += 4u;
        }
        if (pos + 4u <= limit) {
            (void)font_read_block(buf, limit, &pos, NULL, NULL);
        }
    }
    if (ttf_ptr && ttf_len > 0u) {
        out->ttf_ctx = ttf_ctx_create(ttf_ptr, ttf_len, out->base_point_size, &out->height, &out->base);
    }
    return 0;
}

const twd_font_glyph_t *twd_font_find_glyph(const twd_font_t *font, uint32_t codepoint) {
    if (!font || !font->glyphs) return NULL;
    for (uint32_t i = 0u; i < font->glyph_count; ++i) {
        if (font->glyphs[i].codepoint == codepoint) return &font->glyphs[i];
    }
    return NULL;
}

int twd_font_attach_page(twd_font_t *font, uint32_t page_index, const twd_gfx_texture_t *tex) {
    if (!font || page_index >= TWD_FONT_MAX_PAGES) return -1;
    font->pages[page_index] = tex;
    if (page_index + 1u > font->page_count) {
        font->page_count = page_index + 1u;
    }
    return 0;
}

static uint32_t next_utf8_cp(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint8_t c0 = s[0];
    if (c0 < 0x80u) {
        *p += 1;
        return (uint32_t)c0;
    }
    if ((c0 & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        *p += 2;
        return ((uint32_t)(c0 & 0x1Fu) << 6u) | (uint32_t)(s[1] & 0x3Fu);
    }
    if ((c0 & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *p += 3;
        return ((uint32_t)(c0 & 0x0Fu) << 12u) |
               ((uint32_t)(s[1] & 0x3Fu) << 6u) |
               (uint32_t)(s[2] & 0x3Fu);
    }
    if ((c0 & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u &&
        (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
        *p += 4;
        return ((uint32_t)(c0 & 0x07u) << 18u) |
               ((uint32_t)(s[1] & 0x3Fu) << 12u) |
               ((uint32_t)(s[2] & 0x3Fu) << 6u) |
               (uint32_t)(s[3] & 0x3Fu);
    }
    *p += 1;
    return (uint32_t)c0;
}

float twd_font_draw_text(
    twd_gfx_device_t *dev,
    const twd_font_t *font,
    float x,
    float y,
    float scale,
    const uint8_t rgba[4],
    const char *utf8_text) {
    if (!dev || !font || !utf8_text || scale <= 0.0f) return 0.0f;

    uint8_t col[4] = {255u, 255u, 255u, 255u};
    if (rgba) memcpy(col, rgba, 4u);

    uint32_t bb_w = 0u, bb_h = 0u;
    size_t bb_sz = 0u;
    const uint8_t *bb_ro = twd_gfx_backbuffer_data(dev, &bb_w, &bb_h, &bb_sz);
    (void)bb_sz;
    if (!bb_ro || bb_w == 0u || bb_h == 0u) return 0.0f;

    twd_ttf_ctx_t *ttf = (twd_ttf_ctx_t *)font->ttf_ctx;
    float line_h = (font->height > 0.0f) ? font->height : (ttf ? ttf->default_px : 16.0f);

    float pen_x = x;
    float pen_y = y;
    const char *p = utf8_text;
    while (*p) {
        uint32_t cp = next_utf8_cp(&p);
        if (cp == '\n') {
            pen_x = x;
            pen_y += line_h * scale;
            continue;
        }
        const twd_font_glyph_t *g = twd_font_find_glyph(font, cp);
        if (!g && ttf) {
            float target_px = ttf->default_px * scale;
            const ttf_glyph_cache_entry_t *cg = ttf_get_glyph(ttf, cp, target_px);
            if (cg) {
                if (cg->alpha && cg->gw > 0 && cg->gh > 0) {
                    float baseline_y = pen_y + (float)cg->px_round * ttf->ascent_ratio;
                    int32_t dx = (int32_t)(pen_x + (float)cg->x_off + 0.5f);
                    int32_t dy = (int32_t)(baseline_y + (float)cg->y_off + 0.5f);
                    (void)twd_gfx_draw_alpha8(dev, cg->alpha, cg->gw, cg->gh, dx, dy, col);
                }
                pen_x += (cg->x_adv > 0.0f) ? cg->x_adv : (8.0f * scale);
                continue;
            }
        }
        if (!g) {
            pen_x += 8.0f * scale;
            continue;
        }
        if (g->texture_page >= 0 && (uint32_t)g->texture_page < TWD_FONT_MAX_PAGES) {
            const twd_gfx_texture_t *page = font->pages[(uint32_t)g->texture_page];
            if (page && g->width > 0.0f && g->height > 0.0f) {
                size_t p_sz = 0u;
                const uint8_t *p_px = (const uint8_t *)twd_gfx_texture_data(page, &p_sz);
                uint32_t tex_w = 0u, tex_h = 0u;
                twd_gfx_texture_get_size(page, &tex_w, &tex_h);
                if (p_px && p_sz >= 4u && tex_w > 0u && tex_h > 0u) {
                    int32_t sx = (g->u1 <= 1.0f) ? (int32_t)(g->u0 * (float)tex_w + 0.5f) : (int32_t)g->u0;
                    int32_t sy = (g->v1 <= 1.0f) ? (int32_t)(g->v0 * (float)tex_h + 0.5f) : (int32_t)g->v0;
                    int32_t sw = (g->u1 <= 1.0f) ? (int32_t)((g->u1 - g->u0) * (float)tex_w + 0.5f)
                                                 : (int32_t)(g->u1 - g->u0);
                    int32_t sh = (g->v1 <= 1.0f) ? (int32_t)((g->v1 - g->v0) * (float)tex_h + 0.5f)
                                                 : (int32_t)(g->v1 - g->v0);
                    if (sw <= 0) sw = (int32_t)g->width;
                    if (sh <= 0) sh = (int32_t)g->height;

                    int32_t dx = (int32_t)(pen_x + g->x_offset * scale + 0.5f);
                    int32_t dy = (int32_t)(pen_y + g->y_offset * scale + 0.5f);
                    int32_t dw = (int32_t)(g->width * scale + 0.5f);
                    int32_t dh = (int32_t)(g->height * scale + 0.5f);

                    if (g->channel < 0) {
                        (void)twd_gfx_draw_sprite(dev, page, sx, sy, sw, sh, dx, dy, dw, dh, col);
                    } else if (g->channel < 4 && dw > 0 && dh > 0) {
                        for (int32_t py = 0; py < dh; ++py) {
                            int32_t ty = sy + (int32_t)(((int64_t)py * (int64_t)sh) / (int64_t)dh);
                            if (ty < 0 || ty >= (int32_t)tex_h) continue;
                            for (int32_t px = 0; px < dw; ++px) {
                                int32_t tx = sx + (int32_t)(((int64_t)px * (int64_t)sw) / (int64_t)dw);
                                if (tx < 0 || tx >= (int32_t)tex_w) continue;
                                uint8_t cov = p_px[((size_t)ty * (size_t)tex_w + (size_t)tx) * 4u +
                                                   (size_t)g->channel];
                                if (cov == 0u) continue;
                                uint8_t px_col[4];
                                px_col[0] = col[0];
                                px_col[1] = col[1];
                                px_col[2] = col[2];
                                px_col[3] = (uint8_t)(((uint32_t)col[3] * (uint32_t)cov) / 255u);
                                (void)twd_gfx_draw_rect(dev, dx + px, dy + py, 1, 1, px_col);
                            }
                        }
                    }
                }
            }
        }
        pen_x += (g->x_advance > 0.0f ? g->x_advance : g->width) * scale;
    }
    return pen_x - x;
}
