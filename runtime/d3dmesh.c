/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

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

    /* Optional deprecated count + block (>= v19) */
    if (out->version >= 19) {
        if (pos + 4u > limit) return -1;
        uint32_t dep_cnt = dm_rd_u32(buf + pos);
        pos += 4u;
        if (dep_cnt > 0u) {
            if (dm_read_block(buf, limit, &pos, NULL, NULL) != 0) return -1;
        }
    }

    /* Optional CP mesh data block (>= v52) */
    if (out->version >= 52) {
        if (pos + 1u > limit) return -1;
        uint8_t has_cp = buf[pos++];
        if (has_cp) {
            if (dm_read_block(buf, limit, &pos, NULL, NULL) != 0) return -1;
        }
    }

    /* mMeshData MetaBlock */
    size_t md_start = 0u, md_end = 0u;
    if (dm_read_block(buf, limit, &pos, &md_start, &md_end) != 0) return -1;
    size_t mp = md_start;

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
                        out->batches[bi].base_index = dm_rd_u32(bp + 0u);
                        out->batches[bi].start_index = dm_rd_u32(bp + 4u);
                        out->batches[bi].num_primitives = dm_rd_u32(bp + 8u);
                        out->batches[bi].num_indices = dm_rd_u32(bp + 12u);
                        out->batches[bi].material_index = (stride >= 24u) ? dm_rd_i32(bp + 16u) : 0;
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
                lp += 12u; /* posWScale */
                for (int k = 0; k < 3; ++k) out->pos_offset[k] = dm_rd_f32(buf + lp + (size_t)k * 4u);
                lp += 12u;
                lp += 8u; /* lightmap area + prop key base */
                out->vertex_count = dm_rd_u32(buf + lp);
                lp += 8u; /* vertex_count + flags */
            }
        }
    }

    /* Skip mTextures, mMaterials, mMaterialOverrides, mBones, mLocalTransforms, mMaterialRequirements */
    for (int ci = 0; ci < 6; ++ci) {
        if (dm_read_block(buf, md_end, &mp, NULL, NULL) != 0) {
            twd_d3dmesh_free(out);
            return -1;
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
        uint32_t idx[3];
        for (int k = 0; k < 3; ++k) {
            size_t ioff = ((size_t)t * 3u + (size_t)k) * (size_t)mesh->index_stride;
            idx[k] = (mesh->index_stride == 2u)
                ? (uint32_t)dm_rd_u16(mesh->index_data + ioff)
                : dm_rd_u32(mesh->index_data + ioff);
            if (idx[k] >= vcnt) idx[k] = 0u;
        }
        int32_t sx[3], sy[3];
        for (int k = 0; k < 3; ++k) {
            const uint8_t *vp = mesh->vertex_data + (size_t)idx[k] * (size_t)mesh->vertex_stride;
            float vx = dm_rd_f32(vp + 0u);
            float vy = dm_rd_f32(vp + 4u);
            float vz = dm_rd_f32(vp + 8u);
            float z_div = (vz + 3.0f > 0.25f) ? (vz + 3.0f) : 0.25f;
            float proj = scale / z_div;
            sx[k] = (int32_t)(cx + vx * proj + 0.5f);
            sy[k] = (int32_t)(cy - vy * proj + 0.5f);
        }
        (void)twd_gfx_draw_line(dev, sx[0], sy[0], sx[1], sy[1], rgba);
        (void)twd_gfx_draw_line(dev, sx[1], sy[1], sx[2], sy[2], rgba);
        (void)twd_gfx_draw_line(dev, sx[2], sy[2], sx[0], sy[0], rgba);
    }
    return 0;
}
