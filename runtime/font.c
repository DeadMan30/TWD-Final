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
    if (pos + 4u <= limit && font_read_block(buf, limit, &pos, &bs, &be) == 0) {
        if (be - bs >= 4u) {
            int32_t ttf_sz = font_rd_i32(buf + bs);
            out->has_ttf_data = (ttf_sz > 0) ? 1 : 0;
        }
        if (pos + 4u <= limit) {
            out->base_point_size = font_rd_f32(buf + pos);
            pos += 4u;
        }
        if (pos + 4u <= limit) {
            (void)font_read_block(buf, limit, &pos, NULL, NULL);
        }
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

    float pen_x = x;
    float pen_y = y;
    const char *p = utf8_text;
    while (*p) {
        uint32_t cp = next_utf8_cp(&p);
        if (cp == '\n') {
            pen_x = x;
            pen_y += (font->height > 0.0f ? font->height : 16.0f) * scale;
            continue;
        }
        const twd_font_glyph_t *g = twd_font_find_glyph(font, cp);
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
