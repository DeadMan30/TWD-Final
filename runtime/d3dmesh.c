/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint16_t dm_rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static uint32_t dm_rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static int32_t dm_rd_i32(const uint8_t *p) {
    uint32_t v = dm_rd_u32(p);
    int32_t out = 0;
    memcpy(&out, &v, sizeof(out));
    return out;
}

static uint64_t dm_rd_u64(const uint8_t *p) {
    return (uint64_t)dm_rd_u32(p) | ((uint64_t)dm_rd_u32(p + 4u) << 32u);
}

static float dm_rd_f32(const uint8_t *p) {
    uint32_t v = dm_rd_u32(p);
    float out = 0.0f;
    memcpy(&out, &v, sizeof(out));
    return out;
}

static int dm_read_block(const uint8_t *buf, size_t limit, size_t *cursor,
                         size_t *out_start, size_t *out_end) {
    if (*cursor + 4u > limit) return -1;
    uint32_t blk_sz = dm_rd_u32(buf + *cursor);
    *cursor += 4u;
    if (blk_sz < 4u) return -1;
    size_t pay = (size_t)(blk_sz - 4u);
    if (pay > limit - *cursor) return -1;
    if (out_start) *out_start = *cursor;
    if (out_end) *out_end = *cursor + pay;
    *cursor += pay;
    return 0;
}

static void dm_read_string(const uint8_t *bs_ptr, size_t bs_len, char *dst, size_t cap) {
    if (!dst || cap == 0u) return;
    dst[0] = '\0';
    if (!bs_ptr || bs_len < 4u) return;
    uint32_t slen = dm_rd_u32(bs_ptr);
    size_t avail = bs_len - 4u;
    size_t cp = ((size_t)slen <= avail) ? (size_t)slen : avail;
    if (cp >= cap) cp = cap - 1u;
    if (cp > 0u) memcpy(dst, bs_ptr + 4u, cp);
    dst[cp] = '\0';
}

static void dm_parse_textures_block(const uint8_t *blk, size_t blk_len, twd_d3dmesh_t *out) {
    if (!blk || blk_len < 4u || !out) return;
    uint32_t cnt = dm_rd_u32(blk);
    if (cnt == 0u || cnt > 256u) return;
    size_t pos = 4u;
    for (uint32_t i = 0u; i < cnt && out->texture_count < TWD_D3DMESH_MAX_TEXTURES && pos < blk_len; ++i) {
        if (pos + 8u <= blk_len) {
            uint32_t inner_blk = dm_rd_u32(blk + pos);
            uint32_t slen = dm_rd_u32(blk + pos + 4u);
            if (inner_blk >= 8u && (size_t)inner_blk <= blk_len - pos && slen == inner_blk - 8u) {
                char *dst = out->textures[out->texture_count];
                dm_read_string(blk + pos + 4u, (size_t)inner_blk - 4u, dst, 128u);
                out->texture_crcs[out->texture_count] = twd_crc64(0ULL, dst);
                out->texture_count++;
                pos += (size_t)inner_blk;
                continue;
            }
        }
        /* Fallback fixed T3MeshTexture record: optional type (4B) + Handle CRC64 (8B) + optional inline blocked name */
        size_t rem_items = (size_t)(cnt - i);
        size_t stride = (blk_len - pos) / rem_items;
        if (stride >= 12u) {
            uint64_t hcrc = dm_rd_u64(blk + pos + 4u);
            out->texture_crcs[out->texture_count] = hcrc;
            if (stride >= 20u) {
                uint32_t nblk = dm_rd_u32(blk + pos + 12u);
                uint32_t nlen = dm_rd_u32(blk + pos + 16u);
                if (nblk >= 8u && nblk <= stride - 12u && nlen == nblk - 8u) {
                    dm_read_string(blk + pos + 16u, (size_t)nblk - 4u,
                                   out->textures[out->texture_count], 128u);
                }
            }
            out->texture_count++;
            pos += stride;
        } else {
            break;
        }
    }
}

static void dm_parse_materials_block(const uint8_t *blk, size_t blk_len, twd_d3dmesh_t *out) {
    if (!blk || blk_len < 4u || !out) return;
    uint32_t cnt = dm_rd_u32(blk);
    if (cnt == 0u || cnt > 256u) return;
    size_t pos = 4u;
    for (uint32_t i = 0u; i < cnt && out->material_count < TWD_D3DMESH_MAX_MATERIALS && pos < blk_len; ++i) {
        if (pos + 8u <= blk_len) {
            uint32_t inner_blk = dm_rd_u32(blk + pos);
            uint32_t slen = dm_rd_u32(blk + pos + 4u);
            if (inner_blk >= 8u && (size_t)inner_blk <= blk_len - pos && slen == inner_blk - 8u) {
                dm_read_string(blk + pos + 4u, (size_t)inner_blk - 4u,
                               out->materials[out->material_count], 128u);
                out->material_count++;
                pos += (size_t)inner_blk;
                continue;
            }
        }
        size_t rem_items = (size_t)(cnt - i);
        size_t stride = (blk_len - pos) / rem_items;
        if (stride >= 16u) {
            uint32_t mblk = dm_rd_u32(blk + pos + 8u);
            uint32_t mlen = dm_rd_u32(blk + pos + 12u);
            if (mblk >= 8u && mblk <= stride - 8u && mlen == mblk - 8u) {
                dm_read_string(blk + pos + 12u, (size_t)mblk - 4u,
                               out->materials[out->material_count], 128u);
            }
            out->material_count++;
            pos += stride;
        } else {
            break;
        }
    }
}

static int dm_read_gfx_buffer(const uint8_t *buf, size_t limit, size_t *cursor,
                              uint32_t *out_count, uint32_t *out_stride,
                              uint8_t **out_bytes, size_t *out_byte_size) {
    if (*cursor + 8u > limit) return -1;
    int32_t num = dm_rd_i32(buf + *cursor + 0u);
    int32_t stride = dm_rd_i32(buf + *cursor + 4u);
    *cursor += 8u;
    if (num < 0 || stride < 0) return -1;
    if (num > 0 && stride > 0) {
        if ((size_t)num > (limit - *cursor) / (size_t)stride) return -1;
        size_t total = (size_t)num * (size_t)stride;
        uint8_t *copy = (uint8_t *)malloc(total);
        if (!copy) return -1;
        memcpy(copy, buf + *cursor, total);
        *cursor += total;
        *out_count = (uint32_t)num;
        *out_stride = (uint32_t)stride;
        *out_bytes = copy;
        *out_byte_size = total;
    } else {
        *out_count = 0u;
        *out_stride = (uint32_t)stride;
        *out_bytes = NULL;
        *out_byte_size = 0u;
    }
    return 0;
}

void twd_d3dmesh_free(twd_d3dmesh_t *mesh) {
    if (!mesh) return;
    free(mesh->batches);
    free(mesh->vertex_data);
    free(mesh->index_data);
    memset(mesh, 0, sizeof(*mesh));
}

int twd_d3dmesh_parse_memory(const void *data, size_t size, twd_d3dmesh_t *out) {
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
    if (dm_read_block(buf, limit, &pos, &bs, &be) != 0) return -1;
    dm_read_string(buf + bs, be - bs, out->name, sizeof(out->name));

    /* mVersion (i32), mbHasProps (bool), mLightmapGlobalScale (f32), mLightmapTexCoordVersion (i32), mLODParamCRC (u64) */
    if (pos + 21u > limit) return -1;
    out->version = dm_rd_i32(buf + pos);
    pos += 4u;
    pos += 1u; /* mbHasProps */
    out->lightmap_scale = dm_rd_f32(buf + pos);
    pos += 4u + 4u; /* lightmap_scale + lightmap_uv_ver */
    out->lod_param_crc = dm_rd_u64(buf + pos);
    pos += 8u;

    /* Optional deprecated count + block (>= v19) & CP block (>= v52) */
    size_t saved_after_header = pos;
    int sequential_ok = 1;
    if (out->version >= 19) {
        if (pos + 4u > limit) {
            sequential_ok = 0;
        } else {
            uint32_t dep_cnt = dm_rd_u32(buf + pos);
            pos += 4u;
            if (dep_cnt > 0u) {
                if (dm_read_block(buf, limit, &pos, NULL, NULL) != 0) {
                    sequential_ok = 0;
                }
            }
        }
    }
    if (sequential_ok && out->version >= 52) {
        if (pos + 1u > limit) {
            sequential_ok = 0;
        } else {
            uint8_t has_cp = buf[pos++];
            if (has_cp) {
                if (dm_read_block(buf, limit, &pos, NULL, NULL) != 0) {
                    sequential_ok = 0;
                }
            }
        }
    }

    /* mMeshData MetaBlock (fall back to locating exact-to-end mMeshData on retail meshes with internal resources) */
    size_t md_start = 0u, md_end = 0u;
    if (!sequential_ok || dm_read_block(buf, limit, &pos, &md_start, &md_end) != 0) {
        int found_md = 0;
        if (saved_after_header < limit && limit - saved_after_header >= 32u) {
            for (size_t k = saved_after_header; k + 16u <= limit; ++k) {
                uint32_t cand_sz = dm_rd_u32(buf + k);
                if (cand_sz >= 32u && (size_t)cand_sz == limit - k) {
                    uint32_t inner_sz = dm_rd_u32(buf + k + 4u);
                    uint32_t inner_cnt = dm_rd_u32(buf + k + 8u);
                    if (inner_sz >= 8u && inner_sz <= cand_sz - 4u && inner_cnt <= 32u) {
                        md_start = k + 4u;
                        md_end = k + (size_t)cand_sz;
                        found_md = 1;
                        break;
                    }
                }
            }
        }
        if (!found_md) return -1;
    }
    size_t mp = md_start;

    /* Scan embedded mInternalResources (saved_after_header .. md_start) for material diffuse texture Handle CRC64s */
    if (saved_after_header < md_start && md_start - saved_after_header >= 16u) {
        static const uint64_t k_diff_keys[3] = {
            0x8648fa82d1dbee1aULL, /* "Material - Diffuse Texture" */
            0xa6d9cf9425366f5fULL, /* "Diffuse Texture" */
            0xb38c7b2d005540eeULL  /* "Base Texture" */
        };
        size_t scan_end = md_start - 16u;
        for (size_t si = saved_after_header; si <= scan_end && out->texture_count < TWD_D3DMESH_MAX_TEXTURES; ++si) {
            uint64_t kcrc = dm_rd_u64(buf + si);
            int is_key = 0;
            for (int ki = 0; ki < 3; ++ki) {
                if (kcrc == k_diff_keys[ki]) { is_key = 1; break; }
            }
            if (!is_key) continue;
            uint64_t tcrc = dm_rd_u64(buf + si + 8u);
            if (tcrc != 0ULL && tcrc != 0xffffffffffffffffULL) {
                int dup = 0;
                for (uint32_t ti = 0u; ti < out->texture_count; ++ti) {
                    if (out->texture_crcs[ti] == tcrc) { dup = 1; break; }
                }
                if (!dup) {
                    out->texture_crcs[out->texture_count] = tcrc;
                    out->texture_count++;
                }
            }
        }
    }

    /* mLODs (blocked DCArray<T3MeshLOD>) */
    size_t lods_s = 0u, lods_e = 0u;
    if (dm_read_block(buf, md_end, &mp, &lods_s, &lods_e) != 0) return -1;
    if (lods_e - lods_s >= 4u) {
        uint32_t lod_cnt = dm_rd_u32(buf + lods_s);
        out->lod_count = lod_cnt;
        size_t lp = lods_s + 4u;
        if (lod_cnt > 0u) {
            /* Parse LOD 0: mBatches[0], mBatches[1], mVertexStreams, bbox(24B), bsphere(16B),
               mEndianType(4B), posScale(12B), posWScale(12B), posOffset(12B), area(4B), keyBase(4B), vertCount(4B), flags(4B) */
            size_t b0_s = 0u, b0_e = 0u;
            if (dm_read_block(buf, lods_e, &lp, &b0_s, &b0_e) == 0 && b0_e - b0_s >= 4u) {
                uint32_t bc = dm_rd_u32(buf + b0_s);
                size_t bpay = (b0_e - b0_s) - 4u;
                if (bc > 0u && bc <= 1024u && bpay >= (size_t)bc * 20u) {
                    size_t stride = bpay / (size_t)bc;
                    out->batches = (twd_d3dmesh_batch_t *)calloc((size_t)bc, sizeof(twd_d3dmesh_batch_t));
                    if (!out->batches) return -1;
                    for (uint32_t bi = 0u; bi < bc; ++bi) {
                        const uint8_t *bp = buf + b0_s + 4u + (size_t)bi * stride;
                        if (stride >= 76u) {
                            /* Retail v55 T3MeshBatch: bbox(24B) + bsphere(20B) + min/maxVert(8B) + base/start/prims/indices */
                            out->batches[bi].base_index = dm_rd_u32(bp + 52u);
                            out->batches[bi].start_index = dm_rd_u32(bp + 56u);
                            out->batches[bi].num_primitives = dm_rd_u32(bp + 60u);
                            out->batches[bi].num_indices = dm_rd_u32(bp + 64u);
                            out->batches[bi].material_index = dm_rd_i32(bp + 72u);
                        } else {
                            out->batches[bi].base_index = dm_rd_u32(bp + 0u);
                            out->batches[bi].start_index = dm_rd_u32(bp + 4u);
                            out->batches[bi].num_primitives = dm_rd_u32(bp + 8u);
                            out->batches[bi].num_indices = dm_rd_u32(bp + 12u);
                            out->batches[bi].material_index = (stride >= 24u) ? dm_rd_i32(bp + 16u) : 0;
                        }
                    }
                    out->batch_count = bc;
                }
            }
            /* Skip mBatches[1] */
            (void)dm_read_block(buf, lods_e, &lp, NULL, NULL);
            if (lp + 92u <= lods_e) {
                out->vertex_stream_mask = dm_rd_u32(buf + lp);
                lp += 4u;
                for (int k = 0; k < 6; ++k) out->bbox_minmax[k] = dm_rd_f32(buf + lp + (size_t)k * 4u);
                lp += 24u;
                for (int k = 0; k < 4; ++k) out->bsphere_xyzr[k] = dm_rd_f32(buf + lp + (size_t)k * 4u);
                lp += 16u;
                lp += 4u; /* endianType */
                for (int k = 0; k < 3; ++k) out->pos_scale[k] = dm_rd_f32(buf + lp + (size_t)k * 4u);
                lp += 12u;
                for (int k = 0; k < 3; ++k) out->pos_w_scale[k] = dm_rd_f32(buf + lp + (size_t)k * 4u);
                lp += 12u; /* posWScale */
                for (int k = 0; k < 3; ++k) out->pos_offset[k] = dm_rd_f32(buf + lp + (size_t)k * 4u);
                lp += 12u;
                lp += 8u; /* lightmap area + prop key base */
                out->vertex_count = dm_rd_u32(buf + lp);
                lp += 8u; /* vertex_count + flags */
            }
        }
    }

    /* Parse mTextures & mMaterials, skip mMaterialOverrides, mBones, mLocalTransforms, mMaterialRequirements */
    for (int ci = 0; ci < 6; ++ci) {
        size_t c_s = 0u, c_e = 0u;
        if (dm_read_block(buf, md_end, &mp, &c_s, &c_e) != 0) {
            twd_d3dmesh_free(out);
            return -1;
        }
        if (ci == 0) {
            dm_parse_textures_block(buf + c_s, c_e - c_s, out);
        } else if (ci == 1) {
            dm_parse_materials_block(buf + c_s, c_e - c_s, out);
        }
    }
    /* Optional mVertexStreams DCArray or uint32 */
    if (mp + 4u <= md_end) {
        uint32_t probe = dm_rd_u32(buf + mp);
        if (probe >= 4u && probe <= md_end - mp) {
            mp += (size_t)probe;
        } else {
            mp += 4u;
        }
    }

    /* Retail MSV6 T3MeshData stores bbox/bsphere/posScale/posOffset + async T3GFXBuffer tail */
    if (ms.async_size > 0u &&
        ms.payload_offset + ms.default_size + ms.debug_size + ms.async_size <= size) {
        if (mp + 84u <= md_end) {
            float sx = dm_rd_f32(buf + mp + 48u);
            float sy = dm_rd_f32(buf + mp + 52u);
            float ox = dm_rd_f32(buf + mp + 72u);
            float oy = dm_rd_f32(buf + mp + 76u);
            if (fabsf(sx) > 1e-6f || fabsf(sy) > 1e-6f || fabsf(ox) > 1e-6f || fabsf(oy) > 1e-6f) {
                for (int k = 0; k < 6; ++k) out->bbox_minmax[k] = dm_rd_f32(buf + mp + (size_t)k * 4u);
                for (int k = 0; k < 3; ++k) out->pos_scale[k] = dm_rd_f32(buf + mp + 48u + (size_t)k * 4u);
                for (int k = 0; k < 3; ++k) out->pos_w_scale[k] = dm_rd_f32(buf + mp + 60u + (size_t)k * 4u);
                for (int k = 0; k < 3; ++k) out->pos_offset[k] = dm_rd_f32(buf + mp + 72u + (size_t)k * 4u);
            }
        }
        const uint8_t *async_ptr = buf + ms.payload_offset + (size_t)ms.default_size + (size_t)ms.debug_size;
        for (uint32_t nb = 16u; nb >= 2u; --nb) {
            size_t tbl_bytes = (size_t)nb * 20u;
            if (tbl_bytes > limit - md_start) continue;
            const uint8_t *tbl = buf + limit - tbl_bytes;
            size_t sum_async = 0u;
            int table_ok = 1;
            int ib_idx = -1, vb_pos_idx = -1, vb_uv_idx = -1;
            uint32_t vb_pos_fmt = 0u;
            size_t offsets[16];
            for (uint32_t bi = 0u; bi < nb; ++bi) {
                const uint8_t *e = tbl + (size_t)bi * 20u;
                uint32_t endian = dm_rd_u32(e + 0u);
                uint32_t fmt = dm_rd_u32(e + 4u);
                uint32_t usage = dm_rd_u32(e + 8u);
                uint32_t cnt = dm_rd_u32(e + 12u);
                uint32_t stride = dm_rd_u32(e + 16u);
                if (endian > 4u || cnt == 0u || cnt > 2000000u || stride == 0u || stride > 256u) {
                    table_ok = 0;
                    break;
                }
                offsets[bi] = sum_async;
                if ((size_t)cnt > ((size_t)ms.async_size - sum_async) / (size_t)stride) {
                    table_ok = 0;
                    break;
                }
                sum_async += (size_t)cnt * (size_t)stride;
                if ((stride == 2u || stride == 4u) && (fmt == 16u || fmt == 17u || usage == 2u || usage == 10u)) {
                    if (ib_idx < 0 || usage == 2u) ib_idx = (int)bi;
                } else if (ib_idx >= 0 && vb_pos_idx < 0 &&
                           ((stride >= 8u && fmt == 26u) || (stride == 4u && fmt == 41u))) {
                    vb_pos_idx = (int)bi;
                    vb_pos_fmt = fmt;
                } else if (ib_idx >= 0 && vb_uv_idx < 0 && stride == 4u && fmt == 24u) {
                    vb_uv_idx = (int)bi;
                }
            }
            if (table_ok && sum_async == (size_t)ms.async_size && ib_idx >= 0 && vb_pos_idx >= 0) {
                const uint8_t *ie = tbl + (size_t)ib_idx * 20u;
                uint32_t icnt = dm_rd_u32(ie + 12u);
                uint32_t istr = dm_rd_u32(ie + 16u);
                size_t ibytes = (size_t)icnt * (size_t)istr;
                out->index_data = (uint8_t *)malloc(ibytes);
                if (!out->index_data) { twd_d3dmesh_free(out); return -1; }
                memcpy(out->index_data, async_ptr + offsets[ib_idx], ibytes);
                out->index_count = icnt;
                out->index_stride = istr;
                out->index_bytes = ibytes;

                const uint8_t *ve = tbl + (size_t)vb_pos_idx * 20u;
                uint32_t vcnt = dm_rd_u32(ve + 12u);
                uint32_t vstr = dm_rd_u32(ve + 16u);
                const uint8_t *vsrc = async_ptr + offsets[vb_pos_idx];
                const uint8_t *uvsrc = NULL;
                if (vb_uv_idx >= 0 && dm_rd_u32(tbl + (size_t)vb_uv_idx * 20u + 12u) == vcnt) {
                    uvsrc = async_ptr + offsets[vb_uv_idx];
                }
                uint32_t out_stride = uvsrc ? 20u : 12u;
                size_t vbytes = (size_t)vcnt * (size_t)out_stride;
                out->vertex_data = (uint8_t *)calloc(1u, vbytes);
                if (!out->vertex_data) { twd_d3dmesh_free(out); return -1; }
                float ps[3];
                float pws[3];
                float po[3];
                ps[0] = out->pos_scale[0]; ps[1] = out->pos_scale[1]; ps[2] = out->pos_scale[2];
                pws[0] = out->pos_w_scale[0]; pws[1] = out->pos_w_scale[1]; pws[2] = out->pos_w_scale[2];
                po[0] = out->pos_offset[0]; po[1] = out->pos_offset[1]; po[2] = out->pos_offset[2];
                if (fabsf(ps[0]) < 1e-6f && fabsf(ps[1]) < 1e-6f && fabsf(ps[2]) < 1e-6f) {
                    ps[0] = 1.0f; ps[1] = 1.0f; ps[2] = 1.0f;
                }
                for (uint32_t vi = 0u; vi < vcnt; ++vi) {
                    const uint8_t *vp = vsrc + (size_t)vi * (size_t)vstr;
                    uint8_t *dst = out->vertex_data + (size_t)vi * (size_t)out_stride;
                    if (vb_pos_fmt == 41u) {
                        uint32_t p32 = dm_rd_u32(vp);
                        float q[3];
                        q[0] = (float)(p32 & 0x3ffu) / 1023.0f;
                        q[1] = (float)((p32 >> 10u) & 0x3ffu) / 1023.0f;
                        q[2] = (float)((p32 >> 20u) & 0x3ffu) / 1023.0f;
                        float w = (float)((p32 >> 30u) & 0x3u);
                        for (int k = 0; k < 3; ++k) {
                            float fv = q[k] * ps[k] + w * pws[k] + po[k];
                            uint32_t bits = 0u;
                            memcpy(&bits, &fv, sizeof(bits));
                            dst[k * 4 + 0] = (uint8_t)(bits & 0xffu);
                            dst[k * 4 + 1] = (uint8_t)((bits >> 8u) & 0xffu);
                            dst[k * 4 + 2] = (uint8_t)((bits >> 16u) & 0xffu);
                            dst[k * 4 + 3] = (uint8_t)((bits >> 24u) & 0xffu);
                        }
                    } else {
                        for (int k = 0; k < 3; ++k) {
                            float fv = ((float)dm_rd_u16(vp + (size_t)k * 2u) / 65535.0f) * ps[k] + po[k];
                            uint32_t bits = 0u;
                            memcpy(&bits, &fv, sizeof(bits));
                            dst[k * 4 + 0] = (uint8_t)(bits & 0xffu);
                            dst[k * 4 + 1] = (uint8_t)((bits >> 8u) & 0xffu);
                            dst[k * 4 + 2] = (uint8_t)((bits >> 16u) & 0xffu);
                            dst[k * 4 + 3] = (uint8_t)((bits >> 24u) & 0xffu);
                        }
                    }
                    if (uvsrc) {
                        const uint8_t *up = uvsrc + (size_t)vi * 4u;
                        for (int k = 0; k < 2; ++k) {
                            float uvf = (float)dm_rd_u16(up + (size_t)k * 2u) / 65535.0f;
                            uint32_t bits = 0u;
                            memcpy(&bits, &uvf, sizeof(bits));
                            dst[12 + k * 4 + 0] = (uint8_t)(bits & 0xffu);
                            dst[12 + k * 4 + 1] = (uint8_t)((bits >> 8u) & 0xffu);
                            dst[12 + k * 4 + 2] = (uint8_t)((bits >> 16u) & 0xffu);
                            dst[12 + k * 4 + 3] = (uint8_t)((bits >> 24u) & 0xffu);
                        }
                    }
                }
                out->ib_or_vb_verts = vcnt;
                out->vertex_count = vcnt;
                out->vertex_stride = out_stride;
                out->vertex_bytes = vbytes;
                return 0;
            }
        }
    }

    /* TexCoord UV bounds array: count + count * (i32 + 4*f32 = 20B) */
    if (mp + 4u <= md_end) {
        uint32_t uv_cnt = dm_rd_u32(buf + mp);
        mp += 4u;
        if (uv_cnt <= 8u && (size_t)uv_cnt * 20u <= md_end - mp) {
            mp += (size_t)uv_cnt * 20u;
        }
    }

    /* Vertex States count + first VertexState index/vertex T3GFXBuffers */
    if (mp + 4u <= md_end) {
        int32_t vs_cnt = dm_rd_i32(buf + mp);
        mp += 4u;
        if (vs_cnt > 0 && mp < md_end) {
            /* VertexState header (positionFmt, weightFmt, normalFmt, streams, normalCnt, weightOff, vertSize, weightSize = 32B) */
            if (mp + 32u <= md_end) {
                mp += 32u;
                /* Index T3GFXBuffer + Vertex T3GFXBuffer */
                if (dm_read_gfx_buffer(buf, md_end, &mp,
                                       &out->index_count, &out->index_stride,
                                       &out->index_data, &out->index_bytes) != 0 ||
                    dm_read_gfx_buffer(buf, md_end, &mp,
                                       &out->ib_or_vb_verts, &out->vertex_stride,
                                       &out->vertex_data, &out->vertex_bytes) != 0) {
                    twd_d3dmesh_free(out);
                    return -1;
                }
                if (out->vertex_count == 0u) out->vertex_count = out->ib_or_vb_verts;
            }
        }
    }

    return 0;
}

int twd_d3dmesh_upload_gfx(
    twd_gfx_device_t *dev, const twd_d3dmesh_t *mesh, twd_d3dmesh_gpu_t *out_gpu) {
    if (!dev || !mesh || !out_gpu) return -1;
    memset(out_gpu, 0, sizeof(*out_gpu));
    if (mesh->vertex_data && mesh->vertex_bytes > 0u) {
        out_gpu->vb = twd_gfx_buffer_create(dev, TWD_GFX_BUF_VERTEX, mesh->vertex_bytes, mesh->vertex_data);
        if (!out_gpu->vb) return -1;
    }
    if (mesh->index_data && mesh->index_bytes > 0u) {
        out_gpu->ib = twd_gfx_buffer_create(dev, TWD_GFX_BUF_INDEX, mesh->index_bytes, mesh->index_data);
        if (!out_gpu->ib) {
            twd_gfx_buffer_destroy(out_gpu->vb);
            out_gpu->vb = NULL;
            return -1;
        }
    }
    out_gpu->vertex_count = mesh->vertex_count;
    out_gpu->vertex_stride = mesh->vertex_stride;
    out_gpu->index_count = mesh->index_count;
    out_gpu->index_stride = mesh->index_stride;
    return 0;
}

void twd_d3dmesh_gpu_free(twd_d3dmesh_gpu_t *gpu) {
    if (!gpu) return;
    if (gpu->vb) twd_gfx_buffer_destroy(gpu->vb);
    if (gpu->ib) twd_gfx_buffer_destroy(gpu->ib);
    memset(gpu, 0, sizeof(*gpu));
}

static void dm_project_vertex(
    const uint8_t *vp, uint32_t stride,
    float cx, float cy, float scale,
    float *out_vx, float *out_vy, float *out_vz,
    float *out_sx, float *out_sy, float *out_u, float *out_v) {
    float vx = dm_rd_f32(vp + 0u);
    float vy = dm_rd_f32(vp + 4u);
    float vz = dm_rd_f32(vp + 8u);
    float z_div = (vz + 3.0f > 0.25f) ? (vz + 3.0f) : 0.25f;
    float proj = scale / z_div;
    *out_vx = vx;
    *out_vy = vy;
    *out_vz = vz;
    *out_sx = cx + vx * proj;
    *out_sy = cy - vy * proj;
    if (stride >= 32u) {
        *out_u = dm_rd_f32(vp + 24u);
        *out_v = dm_rd_f32(vp + 28u);
    } else if (stride >= 20u) {
        *out_u = dm_rd_f32(vp + 12u);
        *out_v = dm_rd_f32(vp + 16u);
    } else {
        *out_u = 0.0f;
        *out_v = 0.0f;
    }
}

static uint32_t dm_read_index(const twd_d3dmesh_t *mesh, uint32_t elem, uint32_t vcnt) {
    size_t ioff = (size_t)elem * (size_t)mesh->index_stride;
    uint32_t idx = (mesh->index_stride == 2u)
        ? (uint32_t)dm_rd_u16(mesh->index_data + ioff)
        : dm_rd_u32(mesh->index_data + ioff);
    return (idx < vcnt) ? idx : 0u;
}

int twd_d3dmesh_draw_wire(
    twd_gfx_device_t *dev,
    const twd_d3dmesh_t *mesh,
    float cx, float cy, float scale,
    const uint8_t rgba[4]) {
    if (!dev || !mesh || !mesh->vertex_data || mesh->vertex_stride < 12u ||
        !mesh->index_data || (mesh->index_stride != 2u && mesh->index_stride != 4u)) {
        return -1;
    }
    uint32_t vcnt = (uint32_t)(mesh->vertex_bytes / mesh->vertex_stride);
    uint32_t icnt = mesh->index_count;
    if (vcnt == 0u || icnt < 3u) return -1;

    uint32_t tri_cnt = icnt / 3u;
    for (uint32_t t = 0u; t < tri_cnt; ++t) {
        int32_t sx[3], sy[3];
        for (int k = 0; k < 3; ++k) {
            uint32_t idx = dm_read_index(mesh, t * 3u + (uint32_t)k, vcnt);
            const uint8_t *vp = mesh->vertex_data + (size_t)idx * (size_t)mesh->vertex_stride;
            float vx, vy, vz, fx, fy, fu, fv;
            dm_project_vertex(vp, mesh->vertex_stride, cx, cy, scale, &vx, &vy, &vz, &fx, &fy, &fu, &fv);
            sx[k] = (int32_t)(fx + 0.5f);
            sy[k] = (int32_t)(fy + 0.5f);
        }
        (void)twd_gfx_draw_line(dev, sx[0], sy[0], sx[1], sy[1], rgba);
        (void)twd_gfx_draw_line(dev, sx[1], sy[1], sx[2], sy[2], rgba);
        (void)twd_gfx_draw_line(dev, sx[2], sy[2], sx[0], sy[0], rgba);
    }
    return 0;
}

typedef struct dm_tri_sort {
    uint32_t tri_idx;
    float avg_z;
} dm_tri_sort_t;

static void dm_shade_triangle(const float vx[3], const float vy[3], const float vz[3],
                              const uint8_t base_rgba[4], twd_gfx_vertex2d_t tv[3]) {
    float ax = vx[1] - vx[0], ay = vy[1] - vy[0], az = vz[1] - vz[0];
    float bx = vx[2] - vx[0], by = vy[2] - vy[0], bz = vz[2] - vz[0];
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    float nlen = sqrtf(nx * nx + ny * ny + nz * nz);
    float shade = 1.0f;
    if (nlen > 1e-6f) {
        float dot_l = (nx * 0.35f + ny * 0.55f - nz * 0.75f) / nlen;
        if (dot_l < 0.0f) dot_l = -dot_l;
        shade = 0.55f + 0.45f * dot_l;
    }
    for (int k = 0; k < 3; ++k) {
        tv[k].rgba[0] = (uint8_t)((float)base_rgba[0] * shade + 0.5f);
        tv[k].rgba[1] = (uint8_t)((float)base_rgba[1] * shade + 0.5f);
        tv[k].rgba[2] = (uint8_t)((float)base_rgba[2] * shade + 0.5f);
        tv[k].rgba[3] = base_rgba[3];
    }
}

int twd_d3dmesh_draw_shaded(
    twd_gfx_device_t *dev,
    const twd_d3dmesh_t *mesh,
    const twd_gfx_texture_t *tex_or_null,
    float cx, float cy, float scale,
    const uint8_t rgba[4]) {
    if (!dev || !mesh || !mesh->vertex_data || mesh->vertex_stride < 12u ||
        !mesh->index_data || (mesh->index_stride != 2u && mesh->index_stride != 4u)) {
        return -1;
    }
    uint32_t vcnt = (uint32_t)(mesh->vertex_bytes / mesh->vertex_stride);
    uint32_t icnt = mesh->index_count;
    if (vcnt == 0u || icnt < 3u) return -1;

    static const uint8_t k_default_rgba[4] = {255u, 255u, 255u, 255u};
    const uint8_t *base_rgba = rgba ? rgba : k_default_rgba;
    uint32_t tri_cnt = icnt / 3u;

    dm_tri_sort_t order_stack[256];
    uint32_t sort_cnt = (tri_cnt <= 256u) ? tri_cnt : 0u;
    if (sort_cnt > 0u) {
        for (uint32_t t = 0u; t < sort_cnt; ++t) {
            float zsum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                uint32_t idx = dm_read_index(mesh, t * 3u + (uint32_t)k, vcnt);
                zsum += dm_rd_f32(mesh->vertex_data + (size_t)idx * (size_t)mesh->vertex_stride + 8u);
            }
            order_stack[t].tri_idx = t;
            order_stack[t].avg_z = zsum * (1.0f / 3.0f);
        }
        /* Back-to-front painter's sort (largest z first) */
        for (uint32_t i = 1u; i < sort_cnt; ++i) {
            dm_tri_sort_t key = order_stack[i];
            uint32_t j = i;
            while (j > 0u && order_stack[j - 1u].avg_z < key.avg_z) {
                order_stack[j] = order_stack[j - 1u];
                j--;
            }
            order_stack[j] = key;
        }
    }

    for (uint32_t it = 0u; it < tri_cnt; ++it) {
        uint32_t t = (sort_cnt > 0u) ? order_stack[it].tri_idx : it;
        float vx[3], vy[3], vz[3];
        twd_gfx_vertex2d_t tv[3];
        for (int k = 0; k < 3; ++k) {
            uint32_t idx = dm_read_index(mesh, t * 3u + (uint32_t)k, vcnt);
            const uint8_t *vp = mesh->vertex_data + (size_t)idx * (size_t)mesh->vertex_stride;
            dm_project_vertex(vp, mesh->vertex_stride, cx, cy, scale,
                              &vx[k], &vy[k], &vz[k], &tv[k].x, &tv[k].y, &tv[k].u, &tv[k].v);
            tv[k].z = vz[k];
        }
        dm_shade_triangle(vx, vy, vz, base_rgba, tv);
        (void)twd_gfx_draw_triangle(dev, tex_or_null, tv);
    }
    return 0;
}

int twd_d3dmesh_draw_camera(
    twd_gfx_device_t *dev,
    const twd_d3dmesh_t *mesh,
    const twd_gfx_texture_t *tex_or_null,
    const float cam_pos[3],
    float cam_yaw,
    float cam_pitch,
    float fov_deg,
    const float obj_pos[3],
    const float obj_scale[3],
    const uint8_t rgba[4]) {
    if (!dev || !mesh || !mesh->vertex_data || mesh->vertex_stride < 12u ||
        !mesh->index_data || (mesh->index_stride != 2u && mesh->index_stride != 4u)) {
        return -1;
    }
    uint32_t bb_w = 0u, bb_h = 0u;
    if (!twd_gfx_backbuffer_data(dev, &bb_w, &bb_h, NULL) || bb_w == 0u || bb_h == 0u) return -1;
    uint32_t vcnt = (uint32_t)(mesh->vertex_bytes / mesh->vertex_stride);
    uint32_t icnt = mesh->index_count;
    if (vcnt == 0u || icnt < 3u) return -1;

    static const float k_def_cam[3] = {0.0f, 0.0f, 5.0f};
    static const float k_def_pos[3] = {0.0f, 0.0f, 0.0f};
    static const float k_def_sc[3] = {1.0f, 1.0f, 1.0f};
    static const uint8_t k_def_rgba[4] = {255u, 255u, 255u, 255u};
    const float *cp = cam_pos ? cam_pos : k_def_cam;
    const float *op = obj_pos ? obj_pos : k_def_pos;
    const float *os = obj_scale ? obj_scale : k_def_sc;
    const uint8_t *base_rgba = rgba ? rgba : k_def_rgba;

    float cy = cosf(cam_yaw), sy = sinf(cam_yaw);
    float cpitch = cosf(cam_pitch), spitch = sinf(cam_pitch);
    float rx = cy, ry = 0.0f, rz = sy;
    float fx = sy * cpitch, fy = spitch, fz = -cy * cpitch;
    float ux = -sy * spitch, uy = cpitch, uz = cy * spitch;

    float fov = (fov_deg >= 10.0f && fov_deg <= 150.0f) ? fov_deg : 55.0f;
    float half_rad = fov * 0.5f * (3.14159265f / 180.0f);
    float focal = ((float)bb_h * 0.5f) / tanf(half_rad);
    float hbw = (float)bb_w * 0.5f;
    float hbh = (float)bb_h * 0.5f;

    uint32_t tri_cnt = icnt / 3u;
    for (uint32_t t = 0u; t < tri_cnt; ++t) {
        float vx[3], vy[3], vz[3];
        twd_gfx_vertex2d_t tv[3];
        int visible = 1;
        for (int k = 0; k < 3; ++k) {
            uint32_t idx = dm_read_index(mesh, t * 3u + (uint32_t)k, vcnt);
            const uint8_t *vp = mesh->vertex_data + (size_t)idx * (size_t)mesh->vertex_stride;
            float wx = dm_rd_f32(vp + 0u) * os[0] + op[0];
            float wy = dm_rd_f32(vp + 4u) * os[1] + op[1];
            float wz = dm_rd_f32(vp + 8u) * os[2] + op[2];
            float dx = wx - cp[0], dy = wy - cp[1], dz = wz - cp[2];
            float cam_x = dx * rx + dy * ry + dz * rz;
            float cam_y = dx * ux + dy * uy + dz * uz;
            float cam_z = dx * fx + dy * fy + dz * fz;
            if (cam_z < 0.08f) { visible = 0; break; }
            vx[k] = wx; vy[k] = wy; vz[k] = wz;
            tv[k].x = hbw + (cam_x / cam_z) * focal;
            tv[k].y = hbh - (cam_y / cam_z) * focal;
            tv[k].z = cam_z;
            if (mesh->vertex_stride >= 32u) {
                tv[k].u = dm_rd_f32(vp + 24u);
                tv[k].v = dm_rd_f32(vp + 28u);
            } else if (mesh->vertex_stride >= 20u) {
                tv[k].u = dm_rd_f32(vp + 12u);
                tv[k].v = dm_rd_f32(vp + 16u);
            } else {
                tv[k].u = 0.0f;
                tv[k].v = 0.0f;
            }
        }
        if (!visible) continue;
        if ((tv[0].x < -64.0f && tv[1].x < -64.0f && tv[2].x < -64.0f) ||
            (tv[0].x > (float)bb_w + 64.0f && tv[1].x > (float)bb_w + 64.0f && tv[2].x > (float)bb_w + 64.0f) ||
            (tv[0].y < -64.0f && tv[1].y < -64.0f && tv[2].y < -64.0f) ||
            (tv[0].y > (float)bb_h + 64.0f && tv[1].y > (float)bb_h + 64.0f && tv[2].y > (float)bb_h + 64.0f)) {
            continue;
        }
        dm_shade_triangle(vx, vy, vz, base_rgba, tv);
        (void)twd_gfx_draw_triangle_z(dev, tex_or_null, tv);
    }
    return 0;
}
