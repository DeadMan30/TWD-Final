/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_runtime.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static uint32_t sc_rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t sc_rd_u64(const uint8_t *p) {
    return (uint64_t)sc_rd_u32(p) | ((uint64_t)sc_rd_u32(p + 4u) << 32u);
}

static void sc_copy_str(char *dst, size_t cap, const char *src) {
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

static int sc_str_eq_nocase(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0');
}

static int sc_read_block(const uint8_t *buf, size_t limit, size_t *cursor,
                         size_t *out_start, size_t *out_end) {
    if (*cursor + 4u > limit) return -1;
    uint32_t blk_sz = sc_rd_u32(buf + *cursor);
    *cursor += 4u;
    if (blk_sz < 4u) return -1;
    size_t pay = (size_t)(blk_sz - 4u);
    if (pay > limit - *cursor) return -1;
    if (out_start) *out_start = *cursor;
    if (out_end) *out_end = *cursor + pay;
    *cursor += pay;
    return 0;
}

static void sc_read_meta_string(const uint8_t *bs_ptr, size_t bs_len, char *dst, size_t cap) {
    if (!dst || cap == 0u) return;
    dst[0] = '\0';
    if (!bs_ptr || bs_len < 4u) return;
    uint32_t slen = sc_rd_u32(bs_ptr);
    size_t avail = bs_len - 4u;
    size_t cp = ((size_t)slen <= avail) ? (size_t)slen : avail;
    if (cp >= cap) cp = cap - 1u;
    if (cp > 0u) memcpy(dst, bs_ptr + 4u, cp);
    dst[cp] = '\0';
}

static void sc_populate_agent_fields(twd_scene_agent_t *ag) {
    ag->visible = twd_propset_get_bool(&ag->props, "Visible", 1);
    (void)twd_propset_get_vec3(&ag->props, "Agent Position", ag->pos);
    const char *mesh = twd_propset_get_str(&ag->props, "Render Mesh", NULL);
    if (mesh) sc_copy_str(ag->render_mesh, sizeof(ag->render_mesh), mesh);
    const char *ui_txt = twd_propset_get_str(&ag->props, "UI Text", NULL);
    if (ui_txt) sc_copy_str(ag->ui_text, sizeof(ag->ui_text), ui_txt);
}

void twd_scene_free(twd_scene_t *sc) {
    if (!sc) return;
    if (sc->agents) {
        for (uint32_t i = 0u; i < sc->agent_count; ++i) {
            twd_propset_free(&sc->agents[i].props);
        }
        free(sc->agents);
    }
    memset(sc, 0, sizeof(*sc));
}

int twd_scene_parse_memory(const void *data, size_t size, twd_scene_t *out) {
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

    /* mbHidden (bool) */
    if (pos + 1u > limit) return -1;
    out->hidden = (buf[pos++] & 1u) ? 1 : 0;
    out->active = 1;

    /* mName (blocked String) */
    size_t bs = 0u, be = 0u;
    if (sc_read_block(buf, limit, &pos, &bs, &be) != 0) return -1;
    sc_read_meta_string(buf + bs, be - bs, out->name, sizeof(out->name));

    /* mReferencedScenes (blocked DCArray<Handle<Scene>>) */
    if (sc_read_block(buf, limit, &pos, &bs, &be) != 0) return -1;
    if (be - bs >= 4u) {
        uint32_t rcnt = sc_rd_u32(buf + bs);
        size_t pay = (be - bs) - 4u;
        if (rcnt > 0u && pay >= (size_t)rcnt * 8u) {
            size_t stride = pay / (size_t)rcnt;
            uint32_t keep = (rcnt <= TWD_SCENE_MAX_REFS) ? rcnt : TWD_SCENE_MAX_REFS;
            for (uint32_t i = 0u; i < keep; ++i) {
                out->ref_scene_hashes[i] = sc_rd_u64(buf + bs + 4u + (size_t)i * stride);
            }
            out->ref_scene_count = keep;
        }
    }

    /* mAgentList (blocked AgentList -> uint32 agent_count + AgentInfo entries) */
    if (sc_read_block(buf, limit, &pos, &bs, &be) != 0) return -1;
    if (be - bs < 4u) return -1;

    size_t apos = bs;
    uint32_t acnt = sc_rd_u32(buf + apos);
    apos += 4u;
    if (acnt > 4096u) return -1;

    if (acnt > 0u) {
        out->agents = (twd_scene_agent_t *)calloc((size_t)acnt, sizeof(twd_scene_agent_t));
        if (!out->agents) return -1;
        out->agent_cap = acnt;
        for (uint32_t i = 0u; i < acnt; ++i) {
            twd_scene_agent_t *ag = &out->agents[out->agent_count];
            size_t ns = 0u, ne = 0u;
            if (sc_read_block(buf, be, &apos, &ns, &ne) != 0) {
                twd_scene_free(out);
                return -1;
            }
            sc_read_meta_string(buf + ns, ne - ns, ag->name, sizeof(ag->name));
            ag->visible = 1;

            if (apos + 12u > be) {
                twd_scene_free(out);
                return -1;
            }
            /* mAgentSceneProps may be directly inline PropertySet or prefixed by a 4-byte Meta block. */
            uint32_t head0 = sc_rd_u32(buf + apos);
            uint32_t head1 = sc_rd_u32(buf + apos + 4u);
            size_t prop_end = be;
            if (head0 >= 16u && head1 <= 16u && (size_t)head0 <= be - apos) {
                prop_end = apos + (size_t)head0;
                apos += 4u;
            }
            size_t consumed = 0u;
            if (twd_propset_parse_stream(buf + apos, prop_end - apos, &consumed, &ag->props) != 0) {
                twd_scene_free(out);
                return -1;
            }
            apos = (prop_end < be) ? prop_end : (apos + consumed);
            sc_populate_agent_fields(ag);
            out->agent_count++;
        }
    }
    return 0;
}

twd_scene_agent_t *twd_scene_find_agent(twd_scene_t *sc, const char *agent_name) {
    if (!sc || !sc->agents || !agent_name) return NULL;
    for (uint32_t i = 0u; i < sc->agent_count; ++i) {
        if (sc_str_eq_nocase(sc->agents[i].name, agent_name)) {
            return &sc->agents[i];
        }
    }
    return NULL;
}

/* ---------- Runtime Scene & Agent Registry ---------- */

#define TWD_SCENE_MGR_CAP 32u

struct twd_scene_mgr {
    twd_scene_t scenes[TWD_SCENE_MGR_CAP];
    uint32_t scene_count;
};

twd_scene_mgr_t *twd_scene_mgr_create(void) {
    return (twd_scene_mgr_t *)calloc(1u, sizeof(twd_scene_mgr_t));
}

void twd_scene_mgr_destroy(twd_scene_mgr_t *mgr) {
    if (!mgr) return;
    twd_scene_mgr_remove_all(mgr);
    free(mgr);
}

void twd_scene_mgr_remove_all(twd_scene_mgr_t *mgr) {
    if (!mgr) return;
    for (uint32_t i = 0u; i < mgr->scene_count; ++i) {
        twd_scene_free(&mgr->scenes[i]);
    }
    mgr->scene_count = 0u;
}

static int sc_try_load_vfs_scene(const char *scene_name, twd_scene_t *out) {
    if (!scene_name || scene_name[0] == '\0') return -1;
    const char *candidates[2];
    char with_ext[160];
    candidates[0] = scene_name;
    size_t sl = strlen(scene_name);
    if (sl < 6u || !sc_str_eq_nocase(scene_name + sl - 6u, ".scene")) {
        if (sl + 7u < sizeof(with_ext)) {
            memcpy(with_ext, scene_name, sl);
            memcpy(with_ext + sl, ".scene", 7u);
            candidates[1] = with_ext;
        } else {
            candidates[1] = NULL;
        }
    } else {
        candidates[1] = NULL;
    }

    for (size_t i = 0u; i < 2u; ++i) {
        if (!candidates[i]) continue;
        size_t sz = 0u;
        void *buf = twd_asset_read_alloc(candidates[i], &sz);
        if (buf) {
            int rc = twd_scene_parse_memory(buf, sz, out);
            twd_asset_free(buf);
            if (rc == 0) {
                if (out->name[0] == '\0') {
                    sc_copy_str(out->name, sizeof(out->name), scene_name);
                }
                return 0;
            }
        }
    }
    return -1;
}

int twd_scene_mgr_add(twd_scene_mgr_t *mgr, const char *scene_name) {
    if (!mgr || !scene_name || scene_name[0] == '\0') return -1;
    twd_scene_t *existing = twd_scene_mgr_find(mgr, scene_name);
    if (existing) {
        existing->active = 1;
        existing->hidden = 0;
        return 0;
    }
    if (mgr->scene_count >= TWD_SCENE_MGR_CAP) return -1;

    twd_scene_t *slot = &mgr->scenes[mgr->scene_count];
    memset(slot, 0, sizeof(*slot));
    if (sc_try_load_vfs_scene(scene_name, slot) != 0) {
        sc_copy_str(slot->name, sizeof(slot->name), scene_name);
        slot->active = 1;
        slot->hidden = 0;
    }
    mgr->scene_count++;
    return 0;
}

int twd_scene_mgr_open(twd_scene_mgr_t *mgr, const char *scene_name) {
    if (!mgr || !scene_name || scene_name[0] == '\0') return -1;
    twd_scene_mgr_remove_all(mgr);
    return twd_scene_mgr_add(mgr, scene_name);
}

int twd_scene_mgr_remove(twd_scene_mgr_t *mgr, const char *scene_name) {
    if (!mgr || !scene_name) return -1;
    for (uint32_t i = 0u; i < mgr->scene_count; ++i) {
        if (sc_str_eq_nocase(mgr->scenes[i].name, scene_name)) {
            twd_scene_free(&mgr->scenes[i]);
            for (uint32_t j = i + 1u; j < mgr->scene_count; ++j) {
                mgr->scenes[j - 1u] = mgr->scenes[j];
            }
            memset(&mgr->scenes[mgr->scene_count - 1u], 0, sizeof(twd_scene_t));
            mgr->scene_count--;
            return 0;
        }
    }
    return -1;
}

int twd_scene_mgr_hide(twd_scene_mgr_t *mgr, const char *scene_name, int hidden) {
    twd_scene_t *sc = twd_scene_mgr_find(mgr, scene_name);
    if (!sc) return -1;
    sc->hidden = hidden ? 1 : 0;
    return 0;
}

twd_scene_t *twd_scene_mgr_find(twd_scene_mgr_t *mgr, const char *scene_name) {
    if (!mgr || !scene_name) return NULL;
    for (uint32_t i = 0u; i < mgr->scene_count; ++i) {
        if (sc_str_eq_nocase(mgr->scenes[i].name, scene_name)) {
            return &mgr->scenes[i];
        }
    }
    return NULL;
}

int twd_scene_mgr_add_agent(
    twd_scene_mgr_t *mgr, const char *scene_name, const char *agent_name, const float pos[3]) {
    if (!mgr || !agent_name || agent_name[0] == '\0') return -1;
    twd_scene_t *sc = scene_name ? twd_scene_mgr_find(mgr, scene_name)
                                 : (mgr->scene_count > 0u ? &mgr->scenes[0] : NULL);
    if (!sc) return -1;
    twd_scene_agent_t *dup = twd_scene_find_agent(sc, agent_name);
    if (dup) {
        if (pos) memcpy(dup->pos, pos, sizeof(dup->pos));
        return 0;
    }
    if (sc->agent_count >= sc->agent_cap) {
        uint32_t ncap = (sc->agent_cap == 0u) ? 8u : sc->agent_cap * 2u;
        twd_scene_agent_t *na = (twd_scene_agent_t *)realloc(
            sc->agents, (size_t)ncap * sizeof(twd_scene_agent_t));
        if (!na) return -1;
        sc->agents = na;
        sc->agent_cap = ncap;
    }
    twd_scene_agent_t *ag = &sc->agents[sc->agent_count++];
    memset(ag, 0, sizeof(*ag));
    sc_copy_str(ag->name, sizeof(ag->name), agent_name);
    ag->visible = 1;
    if (pos) memcpy(ag->pos, pos, sizeof(ag->pos));
    return 0;
}

twd_scene_agent_t *twd_scene_mgr_find_agent(
    twd_scene_mgr_t *mgr, const char *agent_name, const char *opt_scene_name) {
    if (!mgr || !agent_name) return NULL;
    if (opt_scene_name && opt_scene_name[0] != '\0') {
        twd_scene_t *sc = twd_scene_mgr_find(mgr, opt_scene_name);
        return sc ? twd_scene_find_agent(sc, agent_name) : NULL;
    }
    for (uint32_t i = 0u; i < mgr->scene_count; ++i) {
        twd_scene_agent_t *ag = twd_scene_find_agent(&mgr->scenes[i], agent_name);
        if (ag) return ag;
    }
    return NULL;
}

uint32_t twd_scene_mgr_scene_count(const twd_scene_mgr_t *mgr) {
    return mgr ? mgr->scene_count : 0u;
}

uint32_t twd_scene_mgr_total_agent_count(const twd_scene_mgr_t *mgr) {
    if (!mgr) return 0u;
    uint32_t total = 0u;
    for (uint32_t i = 0u; i < mgr->scene_count; ++i) {
        total += mgr->scenes[i].agent_count;
    }
    return total;
}

const char *twd_scene_mgr_primary_name(const twd_scene_mgr_t *mgr) {
    if (!mgr || mgr->scene_count == 0u) return NULL;
    return mgr->scenes[0].name;
}
