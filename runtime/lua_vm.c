/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Telltale Lua 5.2 chunk unwrapper ("\x1bLua", "\x1bLEn", "\x1bLEo", text) and
 * lightweight portable Lua VM for boot scripts (_boot.lua, DoFile, ResourceSet*).
 *
 * Verified against WalkingDead4.exe:
 *   - 0x140230da0..0x140230e55: script header unwrap (\x1bLEn -> \x1bLua bytecode,
 *     \x1bLEo -> stripped 4-byte prefix + decrypted Lua text, \x1bLua, text).
 *   - 0x1409670a0 (LoadHeader): Lua 5.2 (0x52), LE (1), int=4, size_t=4, instr=4,
 *     lua_Number=4 (IEEE-754 float), integral=0, tail="\x19\x93\r\n\x1a\n".
 */
#include "twd_runtime.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static twd_lua_decrypt_fn g_lua_decrypt_fn = NULL;
static void *g_lua_decrypt_user = NULL;

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static float rd_f32_le(const uint8_t *p) {
    uint32_t u = rd_u32_le(p);
    float f = 0.0f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static char *lua_strdup_n(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1u);
    if (!p) return NULL;
    if (n > 0u && s) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void twd_lua_set_decrypt_callback(twd_lua_decrypt_fn fn, void *user) {
    g_lua_decrypt_fn = fn;
    g_lua_decrypt_user = user;
}

void twd_lua_free_buf(void *ptr) {
    free(ptr);
}

int twd_lua_unwrap_chunk(const void *data,
                         size_t len,
                         twd_lua_chunk_kind_t *out_kind,
                         uint8_t **out_buf,
                         size_t *out_len) {
    if (!data || len == 0u || !out_kind || !out_buf || !out_len) return -1;
    *out_kind = TWD_LUA_CHUNK_UNKNOWN;
    *out_buf = NULL;
    *out_len = 0u;

    const uint8_t *b = (const uint8_t *)data;
    if (len >= 4u && b[0] == 0x1Bu && b[1] == 'L') {
        if (b[2] == 'u' && b[3] == 'a') {
            uint8_t *cp = (uint8_t *)malloc(len);
            if (!cp) return -1;
            memcpy(cp, b, len);
            *out_kind = TWD_LUA_CHUNK_BYTECODE;
            *out_buf = cp;
            *out_len = len;
            return 0;
        }
        if (b[2] == 'E' && b[3] == 'n') {
            *out_kind = TWD_LUA_CHUNK_LEN;
            if (!g_lua_decrypt_fn) return -2;
            uint8_t *cp = (uint8_t *)malloc(len);
            if (!cp) return -1;
            memcpy(cp, b, len);
            cp[2] = 'u';
            cp[3] = 'a';
            size_t dec_len = (len - 4u) & ~(size_t)7u;
            if (dec_len > 0u && g_lua_decrypt_fn(cp + 4u, dec_len, g_lua_decrypt_user) != 0) {
                free(cp);
                return -3;
            }
            *out_buf = cp;
            *out_len = len;
            return 0;
        }
        if (b[2] == 'E' && b[3] == 'o') {
            *out_kind = TWD_LUA_CHUNK_LEO;
            if (!g_lua_decrypt_fn) return -2;
            size_t plen = len - 4u;
            uint8_t *cp = (uint8_t *)malloc(plen + 1u);
            if (!cp) return -1;
            if (plen > 0u) memcpy(cp, b + 4u, plen);
            cp[plen] = '\0';
            size_t dec_len = plen & ~(size_t)7u;
            if (dec_len > 0u && g_lua_decrypt_fn(cp, dec_len, g_lua_decrypt_user) != 0) {
                free(cp);
                return -3;
            }
            *out_buf = cp;
            *out_len = plen;
            return 0;
        }
        return -1;
    }

    uint8_t *cp = (uint8_t *)malloc(len + 1u);
    if (!cp) return -1;
    memcpy(cp, b, len);
    cp[len] = '\0';
    *out_kind = TWD_LUA_CHUNK_TEXT;
    *out_buf = cp;
    *out_len = len;
    return 0;
}

/* --- Internal value, table, and prototype representations --- */

typedef struct lua_kv_entry {
    char *key;
    twd_lua_val_t val;
} lua_kv_entry_t;

typedef struct lua_table {
    uint32_t count;
    uint32_t cap;
    lua_kv_entry_t *entries;
    uint32_t ref_owned;
} lua_table_t;

typedef struct lua_cfunc_binding {
    twd_lua_cfunc_t fn;
    void *user;
} lua_cfunc_binding_t;

static void val_clear(twd_lua_val_t *v) {
    if (!v) return;
    if (v->kind == TWD_LUA_T_STR) {
        free(v->u.str);
    } else if (v->kind == TWD_LUA_T_CFUNC) {
        free(v->u.ptr);
    }
    memset(v, 0, sizeof(*v));
}

static int val_copy(twd_lua_val_t *dst, const twd_lua_val_t *src) {
    val_clear(dst);
    if (!src) return 0;
    *dst = *src;
    if (src->kind == TWD_LUA_T_STR) {
        const char *s = src->u.str ? src->u.str : "";
        dst->u.str = lua_strdup_n(s, strlen(s));
        if (!dst->u.str) {
            dst->kind = TWD_LUA_T_NIL;
            return -1;
        }
    } else if (src->kind == TWD_LUA_T_CFUNC && src->u.ptr) {
        lua_cfunc_binding_t *b = (lua_cfunc_binding_t *)malloc(sizeof(*b));
        if (!b) {
            dst->kind = TWD_LUA_T_NIL;
            return -1;
        }
        *b = *(const lua_cfunc_binding_t *)src->u.ptr;
        dst->u.ptr = b;
    }
    return 0;
}

static lua_table_t *table_create(void) {
    lua_table_t *t = (lua_table_t *)calloc(1u, sizeof(*t));
    if (t) t->ref_owned = 1u;
    return t;
}

static void table_destroy(lua_table_t *t) {
    if (!t) return;
    for (uint32_t i = 0u; i < t->count; ++i) {
        free(t->entries[i].key);
        val_clear(&t->entries[i].val);
    }
    free(t->entries);
    free(t);
}

static const twd_lua_val_t *table_get(const lua_table_t *t, const char *key) {
    if (!t || !key) return NULL;
    for (uint32_t i = 0u; i < t->count; ++i) {
        if (strcmp(t->entries[i].key, key) == 0) return &t->entries[i].val;
    }
    return NULL;
}

static int table_set(lua_table_t *t, const char *key, const twd_lua_val_t *val) {
    if (!t || !key) return -1;
    for (uint32_t i = 0u; i < t->count; ++i) {
        if (strcmp(t->entries[i].key, key) == 0) {
            return val_copy(&t->entries[i].val, val);
        }
    }
    if (t->count == t->cap) {
        uint32_t nc = (t->cap == 0u) ? 16u : t->cap * 2u;
        lua_kv_entry_t *ne = (lua_kv_entry_t *)realloc(t->entries, (size_t)nc * sizeof(*ne));
        if (!ne) return -1;
        t->entries = ne;
        t->cap = nc;
    }
    char *kc = lua_strdup_n(key, strlen(key));
    if (!kc) return -1;
    lua_kv_entry_t *ent = &t->entries[t->count++];
    memset(ent, 0, sizeof(*ent));
    ent->key = kc;
    return val_copy(&ent->val, val);
}

/* Track all heap tables created during VM lifetime for deterministic cleanup. */
struct twd_lua_vm {
    lua_table_t *globals;
    lua_table_t **tables;
    uint32_t table_count;
    uint32_t table_cap;
    uint32_t executed_count;
    uint32_t call_depth;
    twd_lua_chunk_kind_t last_kind;
};

static lua_table_t *vm_alloc_table(twd_lua_vm_t *vm) {
    lua_table_t *t = table_create();
    if (!t) return NULL;
    if (vm->table_count == vm->table_cap) {
        uint32_t nc = (vm->table_cap == 0u) ? 16u : vm->table_cap * 2u;
        lua_table_t **nt = (lua_table_t **)realloc(vm->tables, (size_t)nc * sizeof(*nt));
        if (!nt) {
            table_destroy(t);
            return NULL;
        }
        vm->tables = nt;
        vm->table_cap = nc;
    }
    vm->tables[vm->table_count++] = t;
    return t;
}

/* --- Built-in WD4 boot globals --- */

static int builtin_dofile(twd_lua_vm_t *vm,
                          void *user,
                          int nargs,
                          const twd_lua_val_t *args,
                          twd_lua_val_t *out_ret) {
    (void)user;
    if (out_ret) out_ret->kind = TWD_LUA_T_NIL;
    if (nargs < 1 || args[0].kind != TWD_LUA_T_STR || !args[0].u.str) return -1;
    int rc = twd_lua_vm_exec_vfs(vm, args[0].u.str);
    if (out_ret) {
        out_ret->kind = TWD_LUA_T_BOOL;
        out_ret->u.b = (rc == 0) ? 1 : 0;
    }
    return rc;
}

static int builtin_resource_set(twd_lua_vm_t *vm,
                                void *user,
                                int nargs,
                                const twd_lua_val_t *args,
                                twd_lua_val_t *out_ret) {
    (void)vm;
    int enable = (user != NULL) ? 1 : 0;
    if (out_ret) {
        out_ret->kind = TWD_LUA_T_BOOL;
        out_ret->u.b = 0;
    }
    if (nargs >= 1 && args[0].kind == TWD_LUA_T_STR && args[0].u.str) {
        (void)twd_kv_set("lua.resource_set", args[0].u.str, enable ? "1" : "0");
        if (out_ret) out_ret->u.b = 1;
    }
    return 0;
}

static int builtin_render_kv(twd_lua_vm_t *vm,
                             void *user,
                             int nargs,
                             const twd_lua_val_t *args,
                             twd_lua_val_t *out_ret) {
    (void)vm;
    if (out_ret) out_ret->kind = TWD_LUA_T_NIL;
    const char *key = (const char *)user;
    if (key && nargs >= 1) {
        if (args[0].kind == TWD_LUA_T_STR && args[0].u.str) {
            (void)twd_kv_set("lua.render", key, args[0].u.str);
        } else if (args[0].kind == TWD_LUA_T_NUM) {
            char buf[32];
            int iv = (int)args[0].u.num;
            size_t pos = sizeof(buf);
            buf[--pos] = '\0';
            unsigned u = (iv < 0) ? (unsigned)(-iv) : (unsigned)iv;
            do {
                buf[--pos] = (char)('0' + (u % 10u));
                u /= 10u;
            } while (u != 0u && pos > 1u);
            if (iv < 0 && pos > 0u) buf[--pos] = '-';
            (void)twd_kv_set("lua.render", key, &buf[pos]);
        } else if (args[0].kind == TWD_LUA_T_BOOL) {
            (void)twd_kv_set("lua.render", key, args[0].u.b ? "1" : "0");
        }
    }
    return 0;
}

static int builtin_noop(twd_lua_vm_t *vm,
                        void *user,
                        int nargs,
                        const twd_lua_val_t *args,
                        twd_lua_val_t *out_ret) {
    (void)vm; (void)user; (void)nargs; (void)args;
    if (out_ret) out_ret->kind = TWD_LUA_T_NIL;
    return 0;
}

twd_lua_vm_t *twd_lua_vm_create(void) {
    twd_lua_vm_t *vm = (twd_lua_vm_t *)calloc(1u, sizeof(*vm));
    if (!vm) return NULL;
    vm->globals = vm_alloc_table(vm);
    if (!vm->globals) {
        free(vm);
        return NULL;
    }
    (void)twd_lua_vm_register(vm, "DoFile", builtin_dofile, NULL);
    (void)twd_lua_vm_register(vm, "ResourceSetEnable", builtin_resource_set, (void *)(uintptr_t)1u);
    (void)twd_lua_vm_register(vm, "ResourceSetDisable", builtin_resource_set, NULL);
    (void)twd_lua_vm_register(vm, "RenderSetTextureQuality", builtin_render_kv, (void *)"texture_quality");
    (void)twd_lua_vm_register(vm, "RenderSetFXToonRenderingQuality", builtin_render_kv, (void *)"toon_quality");
    (void)twd_lua_vm_register(vm, "RenderSetFXShadowsRenderingQuality", builtin_render_kv, (void *)"shadow_quality");
    (void)twd_lua_vm_register(vm, "RenderSetGamma", builtin_render_kv, (void *)"gamma");
    (void)twd_lua_vm_register(vm, "print", builtin_noop, NULL);
    (void)twd_lua_vm_register(vm, "Print", builtin_noop, NULL);
    return vm;
}

void twd_lua_vm_destroy(twd_lua_vm_t *vm) {
    if (!vm) return;
    for (uint32_t i = 0u; i < vm->table_count; ++i) {
        table_destroy(vm->tables[i]);
    }
    free(vm->tables);
    free(vm);
}

int twd_lua_vm_register(twd_lua_vm_t *vm, const char *name, twd_lua_cfunc_t fn, void *user) {
    if (!vm || !name || !fn) return -1;
    lua_cfunc_binding_t b;
    b.fn = fn;
    b.user = user;
    twd_lua_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = TWD_LUA_T_CFUNC;
    v.u.ptr = &b;
    return table_set(vm->globals, name, &v);
}

int twd_lua_vm_set_global_str(twd_lua_vm_t *vm, const char *name, const char *val) {
    if (!vm || !name) return -1;
    twd_lua_val_t v;
    memset(&v, 0, sizeof(v));
    if (val) {
        v.kind = TWD_LUA_T_STR;
        v.u.str = (char *)val;
    } else {
        v.kind = TWD_LUA_T_NIL;
    }
    return table_set(vm->globals, name, &v);
}

int twd_lua_vm_set_global_num(twd_lua_vm_t *vm, const char *name, float val) {
    if (!vm || !name) return -1;
    twd_lua_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = TWD_LUA_T_NUM;
    v.u.num = val;
    return table_set(vm->globals, name, &v);
}

int twd_lua_vm_set_global_bool(twd_lua_vm_t *vm, const char *name, int val) {
    if (!vm || !name) return -1;
    twd_lua_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = TWD_LUA_T_BOOL;
    v.u.b = (val != 0) ? 1 : 0;
    return table_set(vm->globals, name, &v);
}

const twd_lua_val_t *twd_lua_vm_get_global(const twd_lua_vm_t *vm, const char *name) {
    if (!vm || !name) return NULL;
    return table_get(vm->globals, name);
}

uint32_t twd_lua_vm_executed_count(const twd_lua_vm_t *vm) {
    return vm ? vm->executed_count : 0u;
}

twd_lua_chunk_kind_t twd_lua_vm_last_chunk_kind(const twd_lua_vm_t *vm) {
    return vm ? vm->last_kind : TWD_LUA_CHUNK_UNKNOWN;
}

/* --- Lua 5.2 Telltale bytecode parser + VM --- */

typedef struct lua_proto {
    uint8_t numparams;
    uint8_t is_vararg;
    uint8_t maxstacksize;
    uint32_t sizecode;
    uint32_t *code;
    uint32_t sizek;
    twd_lua_val_t *k;
    uint32_t sizep;
    struct lua_proto **p;
} lua_proto_t;

static void proto_free(lua_proto_t *p) {
    if (!p) return;
    free(p->code);
    if (p->k) {
        for (uint32_t i = 0u; i < p->sizek; ++i) val_clear(&p->k[i]);
        free(p->k);
    }
    if (p->p) {
        for (uint32_t i = 0u; i < p->sizep; ++i) proto_free(p->p[i]);
        free(p->p);
    }
    free(p);
}

static int bc_read_str(const uint8_t *b, size_t *pos, size_t len, uint8_t sz_size_t, char **out_s) {
    if (out_s) *out_s = NULL;
    if (sz_size_t != 4u && sz_size_t != 8u) return -1;
    uint64_t slen = 0u;
    if (sz_size_t == 4u) {
        if (*pos + 4u > len) return -1;
        slen = (uint64_t)rd_u32_le(b + *pos);
        *pos += 4u;
    } else {
        if (*pos + 8u > len) return -1;
        slen = (uint64_t)rd_u32_le(b + *pos) | ((uint64_t)rd_u32_le(b + *pos + 4u) << 32u);
        *pos += 8u;
    }
    if (slen == 0u) return 0;
    if (slen > (uint64_t)(len - *pos) || slen > 1048576u) return -1;
    if (out_s) {
        size_t n = (size_t)slen;
        if (b[*pos + n - 1u] == '\0') n--;
        *out_s = lua_strdup_n((const char *)(b + *pos), n);
        if (!*out_s) return -1;
    }
    *pos += (size_t)slen;
    return 0;
}

static int bc_read_proto(const uint8_t *b,
                         size_t *pos,
                         size_t len,
                         uint8_t sz_size_t,
                         uint8_t sz_num,
                         int depth,
                         lua_proto_t **out_p) {
    if (depth > 32 || *pos + 11u > len) return -1;
    lua_proto_t *pr = (lua_proto_t *)calloc(1u, sizeof(*pr));
    if (!pr) return -1;

    *pos += 8u; /* linedefined(4) + lastlinedefined(4) */
    pr->numparams = b[(*pos)++];
    pr->is_vararg = b[(*pos)++];
    pr->maxstacksize = b[(*pos)++];

    if (*pos + 4u > len) { proto_free(pr); return -1; }
    pr->sizecode = rd_u32_le(b + *pos);
    *pos += 4u;
    if (pr->sizecode > 65536u || (size_t)pr->sizecode * 4u > len - *pos) {
        proto_free(pr);
        return -1;
    }
    if (pr->sizecode > 0u) {
        pr->code = (uint32_t *)malloc((size_t)pr->sizecode * sizeof(uint32_t));
        if (!pr->code) { proto_free(pr); return -1; }
        for (uint32_t i = 0u; i < pr->sizecode; ++i) {
            pr->code[i] = rd_u32_le(b + *pos);
            *pos += 4u;
        }
    }

    if (*pos + 4u > len) { proto_free(pr); return -1; }
    pr->sizek = rd_u32_le(b + *pos);
    *pos += 4u;
    if (pr->sizek > 65536u) { proto_free(pr); return -1; }
    if (pr->sizek > 0u) {
        pr->k = (twd_lua_val_t *)calloc((size_t)pr->sizek, sizeof(twd_lua_val_t));
        if (!pr->k) { proto_free(pr); return -1; }
        for (uint32_t i = 0u; i < pr->sizek; ++i) {
            if (*pos >= len) { proto_free(pr); return -1; }
            uint8_t tt = b[(*pos)++];
            if (tt == 0u) {
                pr->k[i].kind = TWD_LUA_T_NIL;
            } else if (tt == 1u) {
                if (*pos >= len) { proto_free(pr); return -1; }
                pr->k[i].kind = TWD_LUA_T_BOOL;
                pr->k[i].u.b = (b[(*pos)++] != 0u) ? 1 : 0;
            } else if (tt == 3u) {
                if (sz_num != 4u && sz_num != 8u) { proto_free(pr); return -1; }
                if (*pos + sz_num > len) { proto_free(pr); return -1; }
                pr->k[i].kind = TWD_LUA_T_NUM;
                if (sz_num == 4u) {
                    pr->k[i].u.num = rd_f32_le(b + *pos);
                } else {
                    uint64_t u64 = (uint64_t)rd_u32_le(b + *pos) | ((uint64_t)rd_u32_le(b + *pos + 4u) << 32u);
                    double d = 0.0;
                    memcpy(&d, &u64, sizeof(d));
                    pr->k[i].u.num = (float)d;
                }
                *pos += sz_num;
            } else if (tt == 4u) {
                pr->k[i].kind = TWD_LUA_T_STR;
                if (bc_read_str(b, pos, len, sz_size_t, &pr->k[i].u.str) != 0) {
                    proto_free(pr);
                    return -1;
                }
            } else {
                proto_free(pr);
                return -1;
            }
        }
    }

    if (*pos + 4u > len) { proto_free(pr); return -1; }
    pr->sizep = rd_u32_le(b + *pos);
    *pos += 4u;
    if (pr->sizep > 4096u) { proto_free(pr); return -1; }
    if (pr->sizep > 0u) {
        pr->p = (lua_proto_t **)calloc((size_t)pr->sizep, sizeof(lua_proto_t *));
        if (!pr->p) { proto_free(pr); return -1; }
        for (uint32_t i = 0u; i < pr->sizep; ++i) {
            if (bc_read_proto(b, pos, len, sz_size_t, sz_num, depth + 1, &pr->p[i]) != 0) {
                proto_free(pr);
                return -1;
            }
        }
    }

    if (*pos + 4u > len) { proto_free(pr); return -1; }
    uint32_t sizeup = rd_u32_le(b + *pos);
    *pos += 4u;
    if (sizeup > 4096u || (size_t)sizeup * 2u > len - *pos) { proto_free(pr); return -1; }
    *pos += (size_t)sizeup * 2u;

    /* Debug info: source str, lineinfo, locvars, upvalue names */
    if (bc_read_str(b, pos, len, sz_size_t, NULL) != 0) { proto_free(pr); return -1; }
    if (*pos + 4u > len) { proto_free(pr); return -1; }
    uint32_t nlines = rd_u32_le(b + *pos); *pos += 4u;
    if (nlines > 65536u || (size_t)nlines * 4u > len - *pos) { proto_free(pr); return -1; }
    *pos += (size_t)nlines * 4u;
    if (*pos + 4u > len) { proto_free(pr); return -1; }
    uint32_t nloc = rd_u32_le(b + *pos); *pos += 4u;
    if (nloc > 4096u) { proto_free(pr); return -1; }
    for (uint32_t i = 0u; i < nloc; ++i) {
        if (bc_read_str(b, pos, len, sz_size_t, NULL) != 0 || *pos + 8u > len) {
            proto_free(pr);
            return -1;
        }
        *pos += 8u;
    }
    if (*pos + 4u > len) { proto_free(pr); return -1; }
    uint32_t nupnames = rd_u32_le(b + *pos); *pos += 4u;
    if (nupnames > 4096u) { proto_free(pr); return -1; }
    for (uint32_t i = 0u; i < nupnames; ++i) {
        if (bc_read_str(b, pos, len, sz_size_t, NULL) != 0) {
            proto_free(pr);
            return -1;
        }
    }

    *out_p = pr;
    return 0;
}

static const twd_lua_val_t *vm_rk(const twd_lua_val_t *reg,
                                  uint32_t nreg,
                                  const lua_proto_t *pr,
                                  uint32_t idx) {
    if ((idx & 0x100u) != 0u) {
        uint32_t k = idx & 0xFFu;
        return (k < pr->sizek) ? &pr->k[k] : NULL;
    }
    return (idx < nreg) ? &reg[idx] : NULL;
}

static int vm_is_truthy(const twd_lua_val_t *v) {
    if (!v || v->kind == TWD_LUA_T_NIL) return 0;
    if (v->kind == TWD_LUA_T_BOOL) return (v->u.b != 0) ? 1 : 0;
    return 1;
}

static int vm_exec_proto(twd_lua_vm_t *vm, const lua_proto_t *pr) {
    uint32_t nreg = (pr->maxstacksize < 32u) ? 64u : (uint32_t)pr->maxstacksize + 16u;
    twd_lua_val_t *reg = (twd_lua_val_t *)calloc((size_t)nreg, sizeof(twd_lua_val_t));
    if (!reg) return -1;

    int rc = 0;
    uint32_t pc = 0u;
    uint32_t steps = 0u;
    while (pc < pr->sizecode && steps < 1000000u) {
        steps++;
        uint32_t ins = pr->code[pc++];
        uint32_t op = ins & 0x3Fu;
        uint32_t a = (ins >> 6u) & 0xFFu;
        uint32_t c = (ins >> 14u) & 0x1FFu;
        uint32_t b = (ins >> 23u) & 0x1FFu;
        uint32_t bx = (ins >> 14u) & 0x3FFFFu;
        int32_t sbx = (int32_t)bx - 131071;

        if (a >= nreg) continue;
        switch (op) {
        case 0u: /* OP_MOVE */
            if (b < nreg) (void)val_copy(&reg[a], &reg[b]);
            break;
        case 1u: /* OP_LOADK */
            if (bx < pr->sizek) (void)val_copy(&reg[a], &pr->k[bx]);
            break;
        case 3u: /* OP_LOADBOOL */
            val_clear(&reg[a]);
            reg[a].kind = TWD_LUA_T_BOOL;
            reg[a].u.b = (b != 0u) ? 1 : 0;
            if (c != 0u) pc++;
            break;
        case 4u: /* OP_LOADNIL */
            for (uint32_t i = a; i <= a + b && i < nreg; ++i) val_clear(&reg[i]);
            break;
        case 6u: { /* OP_GETTABUP (up0 = _G) */
            const twd_lua_val_t *kv = vm_rk(reg, nreg, pr, c);
            if (b == 0u && kv && kv->kind == TWD_LUA_T_STR && kv->u.str) {
                const twd_lua_val_t *gv = table_get(vm->globals, kv->u.str);
                if (gv) (void)val_copy(&reg[a], gv);
                else val_clear(&reg[a]);
            } else {
                val_clear(&reg[a]);
            }
            break;
        }
        case 7u: { /* OP_GETTABLE */
            const twd_lua_val_t *kv = vm_rk(reg, nreg, pr, c);
            if (b < nreg && reg[b].kind == TWD_LUA_T_TABLE &&
                kv && kv->kind == TWD_LUA_T_STR && kv->u.str) {
                const twd_lua_val_t *tv = table_get((const lua_table_t *)reg[b].u.ptr, kv->u.str);
                if (tv) (void)val_copy(&reg[a], tv);
                else val_clear(&reg[a]);
            } else {
                val_clear(&reg[a]);
            }
            break;
        }
        case 8u: { /* OP_SETTABUP (up0 = _G) */
            const twd_lua_val_t *kv = vm_rk(reg, nreg, pr, b);
            const twd_lua_val_t *vv = vm_rk(reg, nreg, pr, c);
            if (a == 0u && kv && kv->kind == TWD_LUA_T_STR && kv->u.str && vv) {
                (void)table_set(vm->globals, kv->u.str, vv);
            }
            break;
        }
        case 10u: { /* OP_SETTABLE */
            const twd_lua_val_t *kv = vm_rk(reg, nreg, pr, b);
            const twd_lua_val_t *vv = vm_rk(reg, nreg, pr, c);
            if (reg[a].kind == TWD_LUA_T_TABLE &&
                kv && kv->kind == TWD_LUA_T_STR && kv->u.str && vv) {
                (void)table_set((lua_table_t *)reg[a].u.ptr, kv->u.str, vv);
            }
            break;
        }
        case 11u: { /* OP_NEWTABLE */
            val_clear(&reg[a]);
            lua_table_t *nt = vm_alloc_table(vm);
            if (nt) {
                reg[a].kind = TWD_LUA_T_TABLE;
                reg[a].u.ptr = nt;
            }
            break;
        }
        case 13u: case 14u: case 15u: case 16u: { /* ADD, SUB, MUL, DIV */
            const twd_lua_val_t *vb = vm_rk(reg, nreg, pr, b);
            const twd_lua_val_t *vc = vm_rk(reg, nreg, pr, c);
            if (vb && vc && vb->kind == TWD_LUA_T_NUM && vc->kind == TWD_LUA_T_NUM) {
                float x = vb->u.num, y = vc->u.num, res = 0.0f;
                if (op == 13u) res = x + y;
                else if (op == 14u) res = x - y;
                else if (op == 15u) res = x * y;
                else res = (y != 0.0f) ? (x / y) : 0.0f;
                val_clear(&reg[a]);
                reg[a].kind = TWD_LUA_T_NUM;
                reg[a].u.num = res;
            }
            break;
        }
        case 20u: /* OP_NOT */
            if (b < nreg) {
                int r = !vm_is_truthy(&reg[b]);
                val_clear(&reg[a]);
                reg[a].kind = TWD_LUA_T_BOOL;
                reg[a].u.b = r;
            }
            break;
        case 23u: /* OP_JMP */
            if (sbx >= 0) pc += (uint32_t)sbx;
            else if ((uint32_t)(-sbx) <= pc) pc -= (uint32_t)(-sbx);
            break;
        case 27u: /* OP_TEST */
            if (vm_is_truthy(&reg[a]) != (c != 0u)) pc++;
            break;
        case 29u: /* OP_CALL */
        case 30u: { /* OP_TAILCALL */
            int nargs = (b == 0u) ? 0 : (int)(b - 1u);
            twd_lua_val_t ret;
            memset(&ret, 0, sizeof(ret));
            if (reg[a].kind == TWD_LUA_T_CFUNC && reg[a].u.ptr) {
                const lua_cfunc_binding_t *bind = (const lua_cfunc_binding_t *)reg[a].u.ptr;
                const twd_lua_val_t *args = (a + 1u + (uint32_t)nargs <= nreg) ? &reg[a + 1u] : NULL;
                int crc = bind->fn(vm, bind->user, args ? nargs : 0, args, &ret);
                if (crc != 0) {
                    val_clear(&ret);
                    rc = crc;
                    goto done;
                }
            }
            if (c != 1u) {
                (void)val_copy(&reg[a], &ret);
            }
            val_clear(&ret);
            if (op == 30u) goto done;
            break;
        }
        case 31u: /* OP_RETURN */
            goto done;
        default:
            break;
        }
    }

done:
    for (uint32_t i = 0u; i < nreg; ++i) val_clear(&reg[i]);
    free(reg);
    return rc;
}

static int vm_exec_bytecode(twd_lua_vm_t *vm, const uint8_t *b, size_t len) {
    static const uint8_t s_tail[6] = {0x19u, 0x93u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    if (len < 18u) return -1;
    if (b[0] != 0x1Bu || b[1] != 'L' || b[2] != 'u' || b[3] != 'a') return -1;
    if (b[4] != 0x52u || b[5] != 0u || b[6] != 1u) return -1;
    if (memcmp(b + 12u, s_tail, 6u) != 0) return -1;

    uint8_t sz_size_t = b[8];
    uint8_t sz_num = b[10];
    size_t pos = 18u;
    lua_proto_t *root = NULL;
    if (bc_read_proto(b, &pos, len, sz_size_t, sz_num, 0, &root) != 0) return -1;
    int rc = vm_exec_proto(vm, root);
    proto_free(root);
    return rc;
}

/* --- Lightweight Lua text script runner --- */

static const char *skip_ws_comments(const char *p, const char *end) {
    while (p < end) {
        unsigned char ch = (unsigned char)*p;
        if (isspace(ch) || ch == ';') {
            p++;
            continue;
        }
        if (p + 1 < end && p[0] == '-' && p[1] == '-') {
            p += 2;
            if (p + 1 < end && p[0] == '[' && p[1] == '[') {
                p += 2;
                while (p + 1 < end && !(p[0] == ']' && p[1] == ']')) p++;
                if (p + 1 < end) p += 2;
            } else {
                while (p < end && *p != '\n' && *p != '\r') p++;
            }
            continue;
        }
        break;
    }
    return p;
}

static int parse_expr(const twd_lua_vm_t *vm, const char **pp, const char *end, twd_lua_val_t *out) {
    val_clear(out);
    const char *p = skip_ws_comments(*pp, end);
    if (p >= end) { *pp = p; return -1; }

    if (*p == '"' || *p == '\'') {
        char q = *p++;
        const char *st = p;
        while (p < end && *p != q) {
            if (*p == '\\' && p + 1 < end) p += 2;
            else p++;
        }
        size_t n = (size_t)(p - st);
        if (p < end) p++;
        out->kind = TWD_LUA_T_STR;
        out->u.str = lua_strdup_n(st, n);
        *pp = out->u.str ? p : *pp;
        return out->u.str ? 0 : -1;
    }
    if (isdigit((unsigned char)*p) ||
        (*p == '-' && p + 1 < end && isdigit((unsigned char)p[1]))) {
        char tmp[64];
        size_t ti = 0u;
        if (*p == '-') tmp[ti++] = *p++;
        while (p < end && (isdigit((unsigned char)*p) || *p == '.') && ti + 1u < sizeof(tmp)) {
            tmp[ti++] = *p++;
        }
        tmp[ti] = '\0';
        out->kind = TWD_LUA_T_NUM;
        out->u.num = (float)atof(tmp);
        *pp = p;
        return 0;
    }
    if (isalpha((unsigned char)*p) || *p == '_') {
        const char *st = p++;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
        size_t n = (size_t)(p - st);
        if (n == 4u && strncmp(st, "true", 4u) == 0) {
            out->kind = TWD_LUA_T_BOOL;
            out->u.b = 1;
        } else if (n == 5u && strncmp(st, "false", 5u) == 0) {
            out->kind = TWD_LUA_T_BOOL;
            out->u.b = 0;
        } else if (n == 3u && strncmp(st, "nil", 3u) == 0) {
            out->kind = TWD_LUA_T_NIL;
        } else if (n < 128u) {
            char ident[128];
            memcpy(ident, st, n);
            ident[n] = '\0';
            const twd_lua_val_t *gv = table_get(vm->globals, ident);
            if (gv) (void)val_copy(out, gv);
        }
        *pp = p;
        return 0;
    }
    *pp = p;
    return -1;
}

static int vm_exec_text(twd_lua_vm_t *vm, const uint8_t *buf, size_t len) {
    const char *p = (const char *)buf;
    const char *end = p + len;

    while (1) {
        p = skip_ws_comments(p, end);
        if (p >= end) break;
        if (!isalpha((unsigned char)*p) && *p != '_') {
            p++;
            continue;
        }
        const char *st = p++;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
        size_t n = (size_t)(p - st);
        if (n == 5u && strncmp(st, "local", 5u) == 0) {
            p = skip_ws_comments(p, end);
            if (p >= end || (!isalpha((unsigned char)*p) && *p != '_')) continue;
            st = p++;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
            n = (size_t)(p - st);
        }
        if (n >= 128u) continue;
        char ident[128];
        memcpy(ident, st, n);
        ident[n] = '\0';

        p = skip_ws_comments(p, end);
        if (p < end && *p == '=') {
            p++;
            twd_lua_val_t rhs;
            memset(&rhs, 0, sizeof(rhs));
            if (parse_expr(vm, &p, end, &rhs) == 0) {
                (void)table_set(vm->globals, ident, &rhs);
            }
            val_clear(&rhs);
        } else if (p < end && (*p == '(' || *p == '"')) {
            int paren = (*p == '(') ? 1 : 0;
            if (paren) p++;
            twd_lua_val_t args[8];
            memset(args, 0, sizeof(args));
            int nargs = 0;
            if (!paren) {
                if (parse_expr(vm, &p, end, &args[0]) == 0) nargs = 1;
            } else {
                p = skip_ws_comments(p, end);
                if (p < end && *p == ')') {
                    p++;
                } else {
                    while (p < end && nargs < 8) {
                        if (parse_expr(vm, &p, end, &args[nargs]) == 0) nargs++;
                        p = skip_ws_comments(p, end);
                        if (p < end && *p == ',') {
                            p++;
                            continue;
                        }
                        if (p < end && *p == ')') {
                            p++;
                        }
                        break;
                    }
                }
            }
            const twd_lua_val_t *fnv = table_get(vm->globals, ident);
            if (fnv && fnv->kind == TWD_LUA_T_CFUNC && fnv->u.ptr) {
                const lua_cfunc_binding_t *bind = (const lua_cfunc_binding_t *)fnv->u.ptr;
                twd_lua_val_t ret;
                memset(&ret, 0, sizeof(ret));
                int crc = bind->fn(vm, bind->user, nargs, args, &ret);
                val_clear(&ret);
                for (int i = 0; i < nargs; ++i) val_clear(&args[i]);
                if (crc != 0) return crc;
            } else {
                for (int i = 0; i < nargs; ++i) val_clear(&args[i]);
            }
        }
    }
    return 0;
}

int twd_lua_vm_exec_buffer(twd_lua_vm_t *vm, const void *data, size_t len, const char *chunk_name) {
    (void)chunk_name;
    if (!vm || !data || len == 0u) return -1;
    if (vm->call_depth >= 16u) return -1;

    twd_lua_chunk_kind_t kind = TWD_LUA_CHUNK_UNKNOWN;
    uint8_t *unwrapped = NULL;
    size_t ulen = 0u;
    int uw_rc = twd_lua_unwrap_chunk(data, len, &kind, &unwrapped, &ulen);
    vm->last_kind = kind;
    if (uw_rc != 0) return uw_rc;

    vm->call_depth++;
    int rc = 0;
    if (kind == TWD_LUA_CHUNK_BYTECODE || kind == TWD_LUA_CHUNK_LEN) {
        rc = vm_exec_bytecode(vm, unwrapped, ulen);
    } else {
        rc = vm_exec_text(vm, unwrapped, ulen);
    }
    vm->call_depth--;
    if (rc == 0) vm->executed_count++;
    twd_lua_free_buf(unwrapped);
    return rc;
}

int twd_lua_vm_exec_vfs(twd_lua_vm_t *vm, const char *rel_path) {
    if (!vm || !rel_path) return -1;
    size_t sz = 0u;
    void *buf = twd_asset_read_alloc(rel_path, &sz);
    if (!buf) return -1;
    int rc = twd_lua_vm_exec_buffer(vm, buf, sz, rel_path);
    twd_asset_free(buf);
    return rc;
}
