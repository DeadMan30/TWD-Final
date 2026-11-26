#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t read_i32_le(const uint8_t *p) {
    uint32_t v = read_u32_le(p);
    int32_t out = 0;
    memcpy(&out, &v, sizeof(out));
    return out;
}

static float read_f32_le(const uint8_t *p) {
    uint32_t v = read_u32_le(p);
    float out = 0.0f;
    memcpy(&out, &v, sizeof(out));
    return out;
}

static int skip_meta_block(const uint8_t *buf, size_t limit, size_t *cursor,
                           size_t *out_payload_start, size_t *out_payload_end) {
    if (*cursor + 4u > limit) return -1;
    uint32_t block_size = read_u32_le(buf + *cursor);
    *cursor += 4u;
    if (block_size < 4u) return -1;
    size_t payload = (size_t)(block_size - 4u);
    if (payload > limit - *cursor) return -1;
    if (out_payload_start) *out_payload_start = *cursor;
    if (out_payload_end) *out_payload_end = *cursor + payload;
    *cursor += payload;
    return 0;
}

static size_t compute_mip0_bytes(uint32_t width, uint32_t height, uint32_t fmt) {
    uint32_t bw = (width + 3u) / 4u;
    uint32_t bh = (height + 3u) / 4u;
    if (bw == 0u) bw = 1u;
    if (bh == 0u) bh = 1u;
    switch (fmt) {
        case TWD_T3_FMT_DXT1:
        case TWD_T3_FMT_DXT5A:
        case TWD_T3_FMT_CTX1:
            return (size_t)bw * (size_t)bh * 8u;
        case TWD_T3_FMT_DXT3:
        case TWD_T3_FMT_DXT5:
        case TWD_T3_FMT_DXN:
        case TWD_T3_FMT_BC6:
        case TWD_T3_FMT_BC7:
            return (size_t)bw * (size_t)bh * 16u;
        case TWD_T3_FMT_ARGB8:
        case TWD_T3_FMT_RGBA8:
            return (size_t)width * (size_t)height * 4u;
        case TWD_T3_FMT_RG8:
        case TWD_T3_FMT_R16:
        case TWD_T3_FMT_L16:
        case TWD_T3_FMT_AL8:
        case TWD_T3_FMT_RGB565:
        case TWD_T3_FMT_ARGB1555:
        case TWD_T3_FMT_ARGB4:
            return (size_t)width * (size_t)height * 2u;
        case TWD_T3_FMT_A8:
        case TWD_T3_FMT_L8:
        case TWD_T3_FMT_R8:
            return (size_t)width * (size_t)height;
        default:
            return 0u;
    }
}

void twd_d3dtx_free(twd_d3dtx_image_t *img) {
    if (!img) return;
    free(img->mip0_data);
    memset(img, 0, sizeof(*img));
}

int twd_d3dtx_parse_memory(const void *data, size_t size, twd_d3dtx_image_t *out) {
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

    /* mVersion (int32, unblocked) */
    if (pos + 4u > limit) return -1;
    out->version = read_i32_le(buf + pos);
    pos += 4u;
    if (out->version < 5 || out->version > 16) return -1;

    /* mSamplerState (T3SamplerStateBlock, blocked -> uint32 mData) */
    size_t blk_start = 0u, blk_end = 0u;
    if (skip_meta_block(buf, limit, &pos, &blk_start, &blk_end) != 0) return -1;
    if (blk_end - blk_start >= 4u) {
        out->sampler_state = read_u32_le(buf + blk_start);
    }

    /* mPlatform (PlatformType int32, optionally length-blocked on retail) */
    if (pos + 4u > limit) return -1;
    if (pos + 12u <= limit && read_u32_le(buf + pos) == 8u &&
        read_u32_le(buf + pos + 8u) >= 8u) {
        out->platform = read_i32_le(buf + pos + 4u);
        pos += 8u;
    } else {
        out->platform = read_i32_le(buf + pos);
        pos += 4u;
    }

    /* mName (String, blocked -> uint32 len + chars) */
    if (skip_meta_block(buf, limit, &pos, &blk_start, &blk_end) != 0) return -1;
    if (blk_end - blk_start >= 4u) {
        uint32_t slen = read_u32_le(buf + blk_start);
        size_t avail = (blk_end - blk_start) - 4u;
        size_t copy_len = ((size_t)slen <= avail) ? (size_t)slen : avail;
        if (copy_len >= sizeof(out->name)) copy_len = sizeof(out->name) - 1u;
        if (copy_len > 0u) memcpy(out->name, buf + blk_start + 4u, copy_len);
        out->name[copy_len] = '\0';
    }

    /* mImportName (String, blocked) */
    if (skip_meta_block(buf, limit, &pos, NULL, NULL) != 0) return -1;

    /* mImportScale (float), mbHasProps (bool -> uint8) */
    if (pos + 5u > limit) return -1;
    (void)read_f32_le(buf + pos);
    pos += 4u;
    pos += 1u; /* mToolProps.mbHasProps */

    /* mNumMipLevels, mWidth, mHeight, mDepth, mArraySize, mSurfaceFormat,
       mTextureLayout, mSurfaceGamma, mSurfaceMultisample, mResourceUsage, mType */
    if (pos + 44u > limit) return -1;
    out->mip_levels = read_u32_le(buf + pos + 0u);
    out->width = read_u32_le(buf + pos + 4u);
    out->height = read_u32_le(buf + pos + 8u);
    out->depth = read_u32_le(buf + pos + 12u);
    out->array_size = read_u32_le(buf + pos + 16u);
    out->surface_format = read_u32_le(buf + pos + 20u);
    out->texture_layout = read_u32_le(buf + pos + 24u);
    out->surface_gamma = read_u32_le(buf + pos + 28u);
    out->multisample = read_u32_le(buf + pos + 32u);
    out->resource_usage = read_u32_le(buf + pos + 36u);
    out->tex_type = read_u32_le(buf + pos + 40u);
    pos += 44u;

    if (out->width == 0u || out->height == 0u || out->width > 16384u || out->height > 16384u) {
        return -1;
    }

    /* mSwizzle (RenderSwizzleParams, blocked -> uint8[4]) */
    if (skip_meta_block(buf, limit, &pos, &blk_start, &blk_end) != 0) return -1;
    if (blk_end - blk_start >= 4u) {
        memcpy(out->swizzle, buf + blk_start, 4u);
    }

    /* mSpecularGlossExponent (f32), mHDRLightmapScale (f32), mToonGradientCutoff (f32),
       mAlphaMode (i32), mColorMode (i32), mUVOffset (2*f32), mUVScale (2*f32) = 36 bytes */
    if (pos + 36u > limit) return -1;
    out->alpha_mode = read_i32_le(buf + pos + 12u);
    out->color_mode = read_i32_le(buf + pos + 16u);
    pos += 36u;

    /* mArrayFrameNames (DCArray<Symbol>, blocked) */
    if (skip_meta_block(buf, limit, &pos, NULL, NULL) != 0) return -1;
    /* mToonRegions (DCArray<T3ToonGradientRegion>, blocked) */
    if (skip_meta_block(buf, limit, &pos, NULL, NULL) != 0) return -1;

    /* StreamHeader: mRegionCount, mAuxDataCount, mTotalDataSize */
    if (pos + 12u > limit) return -1;
    int32_t region_count = read_i32_le(buf + pos + 0u);
    int32_t aux_count = read_i32_le(buf + pos + 4u);
    uint32_t total_data_size = read_u32_le(buf + pos + 8u);
    pos += 12u;

    if (region_count <= 0 || region_count > 4096 || aux_count < 0 || aux_count > 4096) {
        return -1;
    }
    if ((size_t)region_count * 24u > limit - pos) return -1;

    size_t regions_off = pos;
    pos += (size_t)region_count * 24u;

    if (aux_count > 0) {
        if (skip_meta_block(buf, limit, &pos, NULL, NULL) != 0) return -1;
    }

    size_t async_off = pos;
    if (ms.async_size > 0u) {
        size_t sec_sum = ms.payload_offset + (size_t)ms.default_size + (size_t)ms.debug_size;
        if (sec_sum > size) return -1;
        async_off = sec_sum;
    }
    if (async_off > size) return -1;
    size_t async_avail = size - async_off;
    size_t data_pool_size = ((size_t)total_data_size <= async_avail) ? (size_t)total_data_size : async_avail;

    /* Walk regions sequentially to find face 0 lowest mip index (mip 0). */
    size_t running_off = 0u;
    size_t best_off = 0u;
    size_t best_size = 0u;
    uint32_t best_pitch = 0u;
    int32_t best_mip = 0x7fffffff;
    int found_mip0 = 0;

    for (int32_t i = 0; i < region_count; ++i) {
        const uint8_t *rh = buf + regions_off + (size_t)i * 24u;
        int32_t face_idx = read_i32_le(rh + 0u);
        int32_t mip_idx = read_i32_le(rh + 4u);
        int32_t mip_cnt = read_i32_le(rh + 8u);
        uint32_t r_size = read_u32_le(rh + 12u);
        uint32_t r_pitch = read_u32_le(rh + 16u);
        (void)mip_cnt;

        if ((size_t)r_size > data_pool_size - running_off) {
            return -1;
        }
        if (face_idx == 0 && (!found_mip0 || mip_idx < best_mip)) {
            found_mip0 = 1;
            best_mip = mip_idx;
            best_off = running_off;
            best_size = (size_t)r_size;
            best_pitch = r_pitch;
        }
        running_off += (size_t)r_size;
    }

    if (!found_mip0 || best_size == 0u) return -1;

    size_t min_expected = compute_mip0_bytes(out->width, out->height, out->surface_format);
    if (min_expected > 0u && best_size > min_expected) {
        /* If a region packs multiple mips from mip 0 downward, mip 0 sits at the start. */
        best_size = min_expected;
    }

    out->region_count = (uint32_t)region_count;
    out->total_data_size = total_data_size;
    out->mip0_pitch = best_pitch;
    out->mip0_size = best_size;
    out->mip0_data = (uint8_t *)malloc(best_size);
    if (!out->mip0_data) {
        twd_d3dtx_free(out);
        return -1;
    }
    memcpy(out->mip0_data, buf + async_off + best_off, best_size);
    return 0;
}

static void unpack_rgb565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t r5 = (uint8_t)((c >> 11) & 0x1Fu);
    uint8_t g6 = (uint8_t)((c >> 5) & 0x3Fu);
    uint8_t b5 = (uint8_t)(c & 0x1Fu);
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static void decode_bc1_colors(const uint8_t *blk, int allow_transparent, uint8_t lut[4][4]) {
    uint16_t c0 = read_u16_le(blk + 0u);
    uint16_t c1 = read_u16_le(blk + 2u);
    unpack_rgb565(c0, &lut[0][0], &lut[0][1], &lut[0][2]);
    lut[0][3] = 255u;
    unpack_rgb565(c1, &lut[1][0], &lut[1][1], &lut[1][2]);
    lut[1][3] = 255u;

    if (!allow_transparent || c0 > c1) {
        for (int ch = 0; ch < 3; ++ch) {
            lut[2][ch] = (uint8_t)((2u * (uint32_t)lut[0][ch] + (uint32_t)lut[1][ch]) / 3u);
            lut[3][ch] = (uint8_t)(((uint32_t)lut[0][ch] + 2u * (uint32_t)lut[1][ch]) / 3u);
        }
        lut[2][3] = 255u;
        lut[3][3] = 255u;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            lut[2][ch] = (uint8_t)(((uint32_t)lut[0][ch] + (uint32_t)lut[1][ch]) / 2u);
            lut[3][ch] = 0u;
        }
        lut[2][3] = 255u;
        lut[3][3] = 0u;
    }
}

static void decode_bc_alpha_block(const uint8_t *blk, uint8_t out_a[16]) {
    uint8_t a[8];
    a[0] = blk[0];
    a[1] = blk[1];
    if (a[0] > a[1]) {
        for (int i = 1; i <= 6; ++i) {
            a[1 + i] = (uint8_t)(((7 - i) * (uint32_t)a[0] + i * (uint32_t)a[1]) / 7u);
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            a[1 + i] = (uint8_t)(((5 - i) * (uint32_t)a[0] + i * (uint32_t)a[1]) / 5u);
        }
        a[6] = 0u;
        a[7] = 255u;
    }

    uint64_t bits = 0ULL;
    for (int i = 0; i < 6; ++i) {
        bits |= ((uint64_t)blk[2 + i]) << (8 * i);
    }
    for (int i = 0; i < 16; ++i) {
        uint32_t idx = (uint32_t)((bits >> (3 * i)) & 0x7ULL);
        out_a[i] = a[idx];
    }
}

int twd_d3dtx_decode_rgba8(const twd_d3dtx_image_t *img, uint8_t **out_rgba, size_t *out_size) {
    if (!img || !img->mip0_data || !out_rgba || img->width == 0u || img->height == 0u) {
        return -1;
    }
    if ((size_t)img->width > ((size_t)-1) / 4u / (size_t)img->height) return -1;

    size_t rgba_size = (size_t)img->width * (size_t)img->height * 4u;
    uint8_t *dst = (uint8_t *)malloc(rgba_size);
    if (!dst) return -1;

    const uint8_t *src = img->mip0_data;
    uint32_t w = img->width;
    uint32_t h = img->height;
    uint32_t fmt = img->surface_format;

    if (fmt == TWD_T3_FMT_RGBA8 || fmt == TWD_T3_FMT_ARGB8) {
        uint32_t row_bytes = w * 4u;
        uint32_t pitch = (img->mip0_pitch >= row_bytes) ? img->mip0_pitch : row_bytes;
        if ((size_t)pitch * (size_t)(h - 1u) + row_bytes > img->mip0_size) {
            free(dst);
            return -1;
        }
        for (uint32_t y = 0u; y < h; ++y) {
            const uint8_t *srow = src + (size_t)y * (size_t)pitch;
            uint8_t *drow = dst + (size_t)y * (size_t)row_bytes;
            if (fmt == TWD_T3_FMT_RGBA8) {
                memcpy(drow, srow, row_bytes);
            } else {
                for (uint32_t x = 0u; x < w; ++x) {
                    drow[x * 4u + 0u] = srow[x * 4u + 2u];
                    drow[x * 4u + 1u] = srow[x * 4u + 1u];
                    drow[x * 4u + 2u] = srow[x * 4u + 0u];
                    drow[x * 4u + 3u] = srow[x * 4u + 3u];
                }
            }
        }
    } else if (fmt == TWD_T3_FMT_A8 || fmt == TWD_T3_FMT_L8 || fmt == TWD_T3_FMT_R8) {
        uint32_t pitch = (img->mip0_pitch >= w) ? img->mip0_pitch : w;
        if ((size_t)pitch * (size_t)(h - 1u) + w > img->mip0_size) {
            free(dst);
            return -1;
        }
        for (uint32_t y = 0u; y < h; ++y) {
            const uint8_t *srow = src + (size_t)y * (size_t)pitch;
            uint8_t *drow = dst + (size_t)y * (size_t)w * 4u;
            for (uint32_t x = 0u; x < w; ++x) {
                uint8_t v = srow[x];
                if (fmt == TWD_T3_FMT_A8) {
                    drow[x * 4u + 0u] = 255u;
                    drow[x * 4u + 1u] = 255u;
                    drow[x * 4u + 2u] = 255u;
                    drow[x * 4u + 3u] = v;
                } else {
                    drow[x * 4u + 0u] = v;
                    drow[x * 4u + 1u] = (fmt == TWD_T3_FMT_L8) ? v : 0u;
                    drow[x * 4u + 2u] = (fmt == TWD_T3_FMT_L8) ? v : 0u;
                    drow[x * 4u + 3u] = 255u;
                }
            }
        }
    } else if (fmt == TWD_T3_FMT_DXT1 || fmt == TWD_T3_FMT_DXT3 || fmt == TWD_T3_FMT_DXT5 ||
               fmt == TWD_T3_FMT_DXT5A || fmt == TWD_T3_FMT_DXN) {
        uint32_t bx_cnt = (w + 3u) / 4u;
        uint32_t by_cnt = (h + 3u) / 4u;
        size_t blk_stride = (fmt == TWD_T3_FMT_DXT1 || fmt == TWD_T3_FMT_DXT5A) ? 8u : 16u;
        if ((size_t)bx_cnt * (size_t)by_cnt * blk_stride > img->mip0_size) {
            free(dst);
            return -1;
        }
        for (uint32_t by = 0u; by < by_cnt; ++by) {
            for (uint32_t bx = 0u; bx < bx_cnt; ++bx) {
                const uint8_t *blk = src + ((size_t)by * (size_t)bx_cnt + (size_t)bx) * blk_stride;
                uint8_t rgba_blk[16][4];
                if (fmt == TWD_T3_FMT_DXT1) {
                    uint8_t lut[4][4];
                    decode_bc1_colors(blk, 1, lut);
                    uint32_t code = read_u32_le(blk + 4u);
                    for (int p = 0; p < 16; ++p) {
                        uint32_t idx = (code >> (2 * p)) & 0x3u;
                        memcpy(rgba_blk[p], lut[idx], 4u);
                    }
                } else if (fmt == TWD_T3_FMT_DXT3) {
                    uint8_t lut[4][4];
                    decode_bc1_colors(blk + 8u, 0, lut);
                    uint32_t code = read_u32_le(blk + 12u);
                    for (int p = 0; p < 16; ++p) {
                        uint32_t idx = (code >> (2 * p)) & 0x3u;
                        uint8_t a4 = (uint8_t)((blk[p / 2] >> ((p & 1) * 4)) & 0xFu);
                        memcpy(rgba_blk[p], lut[idx], 3u);
                        rgba_blk[p][3] = (uint8_t)((a4 << 4) | a4);
                    }
                } else if (fmt == TWD_T3_FMT_DXT5) {
                    uint8_t alphas[16];
                    uint8_t lut[4][4];
                    decode_bc_alpha_block(blk, alphas);
                    decode_bc1_colors(blk + 8u, 0, lut);
                    uint32_t code = read_u32_le(blk + 12u);
                    for (int p = 0; p < 16; ++p) {
                        uint32_t idx = (code >> (2 * p)) & 0x3u;
                        memcpy(rgba_blk[p], lut[idx], 3u);
                        rgba_blk[p][3] = alphas[p];
                    }
                } else if (fmt == TWD_T3_FMT_DXT5A) {
                    uint8_t ch[16];
                    decode_bc_alpha_block(blk, ch);
                    for (int p = 0; p < 16; ++p) {
                        rgba_blk[p][0] = ch[p];
                        rgba_blk[p][1] = ch[p];
                        rgba_blk[p][2] = ch[p];
                        rgba_blk[p][3] = 255u;
                    }
                } else {
                    uint8_t cx[16], cy[16];
                    decode_bc_alpha_block(blk, cx);
                    decode_bc_alpha_block(blk + 8u, cy);
                    for (int p = 0; p < 16; ++p) {
                        rgba_blk[p][0] = cx[p];
                        rgba_blk[p][1] = cy[p];
                        rgba_blk[p][2] = 255u;
                        rgba_blk[p][3] = 255u;
                    }
                }

                for (uint32_t py = 0u; py < 4u; ++py) {
                    uint32_t y = by * 4u + py;
                    if (y >= h) continue;
                    for (uint32_t px = 0u; px < 4u; ++px) {
                        uint32_t x = bx * 4u + px;
                        if (x >= w) continue;
                        uint8_t *d = dst + ((size_t)y * (size_t)w + (size_t)x) * 4u;
                        memcpy(d, rgba_blk[py * 4u + px], 4u);
                    }
                }
            }
        }
    } else {
        free(dst);
        return -1;
    }

    *out_rgba = dst;
    if (out_size) *out_size = rgba_size;
    return 0;
}

twd_gfx_texture_t *twd_d3dtx_upload_gfx_texture(
    twd_gfx_device_t *dev, const void *d3dtx_bytes, size_t d3dtx_size, twd_d3dtx_image_t *out_info) {
    if (!dev || !d3dtx_bytes || d3dtx_size == 0u) return NULL;
    twd_d3dtx_image_t parsed;
    if (twd_d3dtx_parse_memory(d3dtx_bytes, d3dtx_size, &parsed) != 0) {
        return NULL;
    }
    uint8_t *rgba = NULL;
    size_t rgba_size = 0u;
    if (twd_d3dtx_decode_rgba8(&parsed, &rgba, &rgba_size) != 0) {
        twd_d3dtx_free(&parsed);
        return NULL;
    }
    /* Upload as DXGI_FORMAT_R8G8B8A8_UNORM (28) */
    twd_gfx_texture_t *tex = twd_gfx_texture_create(dev, parsed.width, parsed.height, 28u);
    if (!tex || twd_gfx_texture_update(tex, rgba, rgba_size) != 0) {
        twd_gfx_texture_destroy(tex);
        free(rgba);
        twd_d3dtx_free(&parsed);
        return NULL;
    }
    free(rgba);
    if (out_info) {
        *out_info = parsed;
    } else {
        twd_d3dtx_free(&parsed);
    }
    return tex;
}
