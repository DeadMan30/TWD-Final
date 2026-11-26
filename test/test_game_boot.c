/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdio.h>
#include <string.h>

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_u64(uint8_t *p, uint64_t v) {
    wr_u32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    wr_u32(p + 4u, (uint32_t)((v >> 32) & 0xFFFFFFFFULL));
}

int main(void) {
    /* Build temporary game dir containing one synthetic NCTT+TTA4 archive with boot assets. */
    static const char s_boot_lua[] =
        "-- synthetic boot\n"
        "ResourceSetEnable(\"Boot\")\n"
        "RenderSetTextureQuality(2)\n"
        "DoFile(\"scripts/boot_sub.lua\")\n";
    static const char s_sub_lua[] =
        "ResourceSetEnable(\"Menu\")\n"
        "SceneOpen(\"Menu.scene\")\n"
        "SceneAddAgent(\"Menu.scene\", \"OverlayCursor\")\n"
        "AgentSetPos(\"OverlayCursor\", 10, 20, 0)\n";
    uint8_t s_prefs[128];
    memset(s_prefs, 0, sizeof(s_prefs));
    size_t po = 0u;
    memcpy(s_prefs + po, "6VSM", 4u); po += 4u;
    size_t def_sz_off = po; po += 4u; /* default_size */
    po += 8u; /* debug_size + async_size */
    wr_u32(s_prefs + po, 0u); po += 4u; /* type_count */
    size_t prop_start = po;
    wr_u32(s_prefs + po, 2u); po += 4u; /* mPropVersion */
    wr_u32(s_prefs + po, 0u); po += 4u; /* mPropertyFlags */
    size_t blk_sz_off = po; po += 4u;
    size_t blk_start = po;
    wr_u32(s_prefs + po, 0u); po += 4u; /* parent_count */
    wr_u32(s_prefs + po, 3u); po += 4u; /* num_type_groups */
    /* Group 1: bool ("Windowed" = 1) */
    wr_u64(s_prefs + po, twd_crc64(0ULL, "bool")); po += 8u;
    wr_u32(s_prefs + po, 1u); po += 4u;
    wr_u64(s_prefs + po, twd_crc64(0ULL, "Windowed")); po += 8u;
    s_prefs[po++] = 1u;
    /* Group 2: int32 ("FX Brush Quality" = 2) */
    wr_u64(s_prefs + po, twd_crc64(0ULL, "int32")); po += 8u;
    wr_u32(s_prefs + po, 1u); po += 4u;
    wr_u64(s_prefs + po, twd_crc64(0ULL, "FX Brush Quality")); po += 8u;
    wr_u32(s_prefs + po, 2u); po += 4u;
    /* Group 3: Vector2 ("Window Size" = 1600.0f x 900.0f) */
    wr_u64(s_prefs + po, twd_crc64(0ULL, "Vector2")); po += 8u;
    wr_u32(s_prefs + po, 1u); po += 4u;
    wr_u64(s_prefs + po, twd_crc64(0ULL, "Window Size")); po += 8u;
    float win_x = 1600.0f, win_y = 900.0f;
    memcpy(s_prefs + po, &win_x, 4u); po += 4u;
    memcpy(s_prefs + po, &win_y, 4u); po += 4u;
    wr_u32(s_prefs + blk_sz_off, (uint32_t)(po - blk_start));
    wr_u32(s_prefs + def_sz_off, (uint32_t)(po - prop_start));

    static const uint8_t s_bank[16] = {'R','I','F','F', 8,0,0,0, 'F','E','V',' ', 1,0,0,0};
    uint8_t s_d3dtx[211];
    memset(s_d3dtx, 0, sizeof(s_d3dtx));
    size_t to = 0u;
    memcpy(s_d3dtx + to, "6VSM", 4u); to += 4u;
    size_t td_def_off = to; to += 4u;
    to += 4u;
    size_t td_async_off = to; to += 4u;
    wr_u32(s_d3dtx + to, 0u); to += 4u;
    size_t td_start = to;
    wr_u32(s_d3dtx + to, 9u); to += 4u;   /* mVersion */
    wr_u32(s_d3dtx + to, 8u); to += 8u;   /* mSamplerState */
    wr_u32(s_d3dtx + to, 2u); to += 4u;   /* mPlatform */
    wr_u32(s_d3dtx + to, 14u); to += 4u;  /* mName block (6 chars) */
    wr_u32(s_d3dtx + to, 6u); to += 4u;
    memcpy(s_d3dtx + to, "splash", 6u); to += 6u;
    wr_u32(s_d3dtx + to, 8u); to += 8u;   /* mImportName */
    to += 5u;                             /* mImportScale + mbHasProps */
    wr_u32(s_d3dtx + to, 1u); to += 4u;   /* mNumMipLevels */
    wr_u32(s_d3dtx + to, 4u); to += 4u;   /* mWidth */
    wr_u32(s_d3dtx + to, 4u); to += 4u;   /* mHeight */
    wr_u32(s_d3dtx + to, 1u); to += 4u;   /* mDepth */
    wr_u32(s_d3dtx + to, 1u); to += 4u;   /* mArraySize */
    wr_u32(s_d3dtx + to, TWD_T3_FMT_DXT1); to += 4u;
    wr_u32(s_d3dtx + to, 1u); to += 20u;  /* layout/gamma/msaa/usage/type */
    wr_u32(s_d3dtx + to, 8u); to += 8u;   /* mSwizzle */
    to += 36u;                            /* gloss/hdr/toon/alpha/color/uv */
    wr_u32(s_d3dtx + to, 8u); to += 8u;   /* mArrayFrameNames */
    wr_u32(s_d3dtx + to, 8u); to += 8u;   /* mToonRegions */
    wr_u32(s_d3dtx + to, 1u); to += 4u;   /* mRegionCount */
    wr_u32(s_d3dtx + to, 0u); to += 4u;   /* mAuxDataCount */
    wr_u32(s_d3dtx + to, 8u); to += 4u;   /* mTotalDataSize */
    wr_u32(s_d3dtx + to, 0u); to += 4u;   /* face */
    wr_u32(s_d3dtx + to, 0u); to += 4u;   /* mip */
    wr_u32(s_d3dtx + to, 1u); to += 4u;   /* mip_count */
    wr_u32(s_d3dtx + to, 8u); to += 4u;   /* data_size */
    wr_u32(s_d3dtx + to, 8u); to += 4u;   /* pitch */
    wr_u32(s_d3dtx + to, 8u); to += 4u;   /* slice_pitch */
    wr_u32(s_d3dtx + td_def_off, (uint32_t)(to - td_start));
    wr_u32(s_d3dtx + td_async_off, 8u);
    s_d3dtx[to++] = 0xE0u; s_d3dtx[to++] = 0x07u;
    s_d3dtx[to++] = 0x00u; s_d3dtx[to++] = 0x00u;
    to += 4u;

    uint8_t s_scene[256];
    memset(s_scene, 0, sizeof(s_scene));
    size_t so = 0u;
    memcpy(s_scene + so, "6VSM", 4u); so += 4u;
    size_t sc_def_off = so; so += 4u;
    so += 8u; /* debug + async */
    wr_u32(s_scene + so, 0u); so += 4u;
    size_t sc_start = so;
    s_scene[so++] = 0u; /* mbHidden = 0 */
    wr_u32(s_scene + so, 18u); so += 4u; /* mName ("Menu.scene") */
    wr_u32(s_scene + so, 10u); so += 4u;
    memcpy(s_scene + so, "Menu.scene", 10u); so += 10u;
    wr_u32(s_scene + so, 8u); so += 4u; /* mReferencedScenes (0) */
    so += 4u;
    size_t al_off = so; so += 4u;
    wr_u32(s_scene + so, 2u); so += 4u; /* 2 agents */
    /* Agent 0: CamMain */
    wr_u32(s_scene + so, 15u); so += 4u;
    wr_u32(s_scene + so, 7u); so += 4u;
    memcpy(s_scene + so, "CamMain", 7u); so += 7u;
    wr_u32(s_scene + so, 2u); so += 4u;
    wr_u32(s_scene + so, 0u); so += 4u;
    wr_u32(s_scene + so, 8u); so += 4u;
    so += 8u;
    /* Agent 1: TitleLogo */
    wr_u32(s_scene + so, 17u); so += 4u;
    wr_u32(s_scene + so, 9u); so += 4u;
    memcpy(s_scene + so, "TitleLogo", 9u); so += 9u;
    wr_u32(s_scene + so, 2u); so += 4u;
    wr_u32(s_scene + so, 0u); so += 4u;
    wr_u32(s_scene + so, 8u); so += 4u;
    so += 8u;
    wr_u32(s_scene + al_off, (uint32_t)(so - al_off));
    wr_u32(s_scene + sc_def_off, (uint32_t)(so - sc_start));

    uint8_t s_mesh[400];
    memset(s_mesh, 0, sizeof(s_mesh));
    size_t mo = 0u;
    memcpy(s_mesh + mo, "6VSM", 4u); mo += 4u;
    size_t dm_def_off = mo; mo += 4u;
    mo += 8u;
    wr_u32(s_mesh + mo, 0u); mo += 4u;
    size_t dm_start = mo;
    wr_u32(s_mesh + mo, 11u); mo += 4u;
    wr_u32(s_mesh + mo, 3u); mo += 4u;
    memcpy(s_mesh + mo, "tri", 3u); mo += 3u;
    wr_u32(s_mesh + mo, 55u); mo += 4u;
    s_mesh[mo++] = 0u;
    { float lms = 1.0f; memcpy(s_mesh + mo, &lms, 4u); mo += 4u; }
    mo += 4u;
    wr_u64(s_mesh + mo, 0x11223344ULL); mo += 8u;
    wr_u32(s_mesh + mo, 0u); mo += 4u;
    s_mesh[mo++] = 0u;
    size_t md_blk_off = mo; mo += 4u;
    size_t md_start = mo;
    size_t lods_blk_off = mo; mo += 4u;
    size_t lods_start = mo;
    wr_u32(s_mesh + mo, 1u); mo += 4u;
    wr_u32(s_mesh + mo, 32u); mo += 4u;
    wr_u32(s_mesh + mo, 1u); mo += 4u;
    mo += 8u;
    wr_u32(s_mesh + mo, 1u); mo += 4u;
    wr_u32(s_mesh + mo, 3u); mo += 4u;
    mo += 8u;
    wr_u32(s_mesh + mo, 8u); mo += 4u; mo += 4u;
    wr_u32(s_mesh + mo, 1u); mo += 4u;
    mo += 24u + 16u + 4u;
    { float one = 1.0f; memcpy(s_mesh + mo, &one, 4u); memcpy(s_mesh + mo + 4u, &one, 4u); memcpy(s_mesh + mo + 8u, &one, 4u); }
    mo += 12u + 12u + 12u + 8u;
    wr_u32(s_mesh + mo, 3u); mo += 4u;
    mo += 4u;
    wr_u32(s_mesh + lods_blk_off, (uint32_t)(mo - lods_start) + 4u);
    for (uint32_t dci = 0u; dci < 6u; ++dci) {
        wr_u32(s_mesh + mo, 8u); mo += 4u; mo += 4u;
    }
    mo += 8u;
    wr_u32(s_mesh + mo, 1u); mo += 4u;
    mo += 32u;
    wr_u32(s_mesh + mo, 3u); mo += 4u;
    wr_u32(s_mesh + mo, 2u); mo += 4u;
    wr_u16(s_mesh + mo, 0u); wr_u16(s_mesh + mo + 2u, 1u); wr_u16(s_mesh + mo + 4u, 2u);
    mo += 6u;
    wr_u32(s_mesh + mo, 3u); mo += 4u;
    wr_u32(s_mesh + mo, 12u); mo += 4u;
    {
        float vtx[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        memcpy(s_mesh + mo, vtx, sizeof(vtx));
        mo += sizeof(vtx);
    }
    wr_u32(s_mesh + md_blk_off, (uint32_t)(mo - md_start) + 4u);
    wr_u32(s_mesh + dm_def_off, (uint32_t)(mo - dm_start));

    static const char names[] = "_boot.lua\0prefs.prop\0Master.bank\0scripts/boot_sub.lua\0splash.d3dtx\0Menu.scene\0ui_boot.d3dmesh\0";

    enum {
        N_ENT = 7,
        NAMES_SZ = sizeof(names) - 1,
        DATA_BASE = 12 + N_ENT * 28 + NAMES_SZ,
        BOOT_LEN = sizeof(s_boot_lua) - 1,
        PREFS_LEN = 113,
        BANK_LEN = sizeof(s_bank),
        SUB_LEN = sizeof(s_sub_lua) - 1,
        TEX_LEN = 211,
        SCENE_LEN = 127,
        MESH_LEN = 359,
        PAYLOAD_SZ = DATA_BASE + BOOT_LEN + PREFS_LEN + BANK_LEN + SUB_LEN + TEX_LEN + SCENE_LEN + MESH_LEN
    };
    if (po != (size_t)PREFS_LEN || to != (size_t)TEX_LEN ||
        so != (size_t)SCENE_LEN || mo != (size_t)MESH_LEN) return 99;

    uint8_t pkg[12 + PAYLOAD_SZ];
    memset(pkg, 0, sizeof(pkg));
    memcpy(pkg, "NCTT", 4u);
    wr_u64(pkg + 4u, (uint64_t)PAYLOAD_SZ);

    uint8_t *toc = pkg + 12u;
    wr_u32(toc + 0u, TWD_TTARCH2_TOC_TTA4);
    wr_u32(toc + 4u, (uint32_t)NAMES_SZ);
    wr_u32(toc + 8u, (uint32_t)N_ENT);

    wr_u64(toc + 12u + 0u * 28u + 0u, 1ULL);
    wr_u64(toc + 12u + 0u * 28u + 8u, 0ULL);
    wr_u32(toc + 12u + 0u * 28u + 16u, (uint32_t)BOOT_LEN);
    wr_u16(toc + 12u + 0u * 28u + 26u, 0u);

    wr_u64(toc + 12u + 1u * 28u + 0u, 2ULL);
    wr_u64(toc + 12u + 1u * 28u + 8u, (uint64_t)BOOT_LEN);
    wr_u32(toc + 12u + 1u * 28u + 16u, (uint32_t)PREFS_LEN);
    wr_u16(toc + 12u + 1u * 28u + 26u, 10u);

    wr_u64(toc + 12u + 2u * 28u + 0u, 3ULL);
    wr_u64(toc + 12u + 2u * 28u + 8u, (uint64_t)(BOOT_LEN + PREFS_LEN));
    wr_u32(toc + 12u + 2u * 28u + 16u, (uint32_t)BANK_LEN);
    wr_u16(toc + 12u + 2u * 28u + 26u, 21u);

    wr_u64(toc + 12u + 3u * 28u + 0u, twd_crc64(0ULL, "scripts/boot_sub.lua"));
    wr_u64(toc + 12u + 3u * 28u + 8u, (uint64_t)(BOOT_LEN + PREFS_LEN + BANK_LEN));
    wr_u32(toc + 12u + 3u * 28u + 16u, (uint32_t)SUB_LEN);
    wr_u16(toc + 12u + 3u * 28u + 26u, 33u);

    wr_u64(toc + 12u + 4u * 28u + 0u, twd_crc64(0ULL, "splash.d3dtx"));
    wr_u64(toc + 12u + 4u * 28u + 8u, (uint64_t)(BOOT_LEN + PREFS_LEN + BANK_LEN + SUB_LEN));
    wr_u32(toc + 12u + 4u * 28u + 16u, (uint32_t)TEX_LEN);
    wr_u16(toc + 12u + 4u * 28u + 26u, 54u);

    wr_u64(toc + 12u + 5u * 28u + 0u, twd_crc64(0ULL, "Menu.scene"));
    wr_u64(toc + 12u + 5u * 28u + 8u, (uint64_t)(BOOT_LEN + PREFS_LEN + BANK_LEN + SUB_LEN + TEX_LEN));
    wr_u32(toc + 12u + 5u * 28u + 16u, (uint32_t)SCENE_LEN);
    wr_u16(toc + 12u + 5u * 28u + 26u, 67u);

    wr_u64(toc + 12u + 6u * 28u + 0u, twd_crc64(0ULL, "ui_boot.d3dmesh"));
    wr_u64(toc + 12u + 6u * 28u + 8u, (uint64_t)(BOOT_LEN + PREFS_LEN + BANK_LEN + SUB_LEN + TEX_LEN + SCENE_LEN));
    wr_u32(toc + 12u + 6u * 28u + 16u, (uint32_t)MESH_LEN);
    wr_u16(toc + 12u + 6u * 28u + 26u, 78u);

    memcpy(toc + 12u + N_ENT * 28u, names, NAMES_SZ);
    memcpy(toc + DATA_BASE, s_boot_lua, BOOT_LEN);
    memcpy(toc + DATA_BASE + BOOT_LEN, s_prefs, PREFS_LEN);
    memcpy(toc + DATA_BASE + BOOT_LEN + PREFS_LEN, s_bank, BANK_LEN);
    memcpy(toc + DATA_BASE + BOOT_LEN + PREFS_LEN + BANK_LEN, s_sub_lua, SUB_LEN);
    memcpy(toc + DATA_BASE + BOOT_LEN + PREFS_LEN + BANK_LEN + SUB_LEN, s_d3dtx, TEX_LEN);
    memcpy(toc + DATA_BASE + BOOT_LEN + PREFS_LEN + BANK_LEN + SUB_LEN + TEX_LEN, s_scene, SCENE_LEN);
    memcpy(toc + DATA_BASE + BOOT_LEN + PREFS_LEN + BANK_LEN + SUB_LEN + TEX_LEN + SCENE_LEN, s_mesh, MESH_LEN);

    if (twd_mkdir("twd_boot_smoke_dir") != 0) return 1;
    twd_file_t f;
    if (twd_file_open("twd_boot_smoke_dir/boot0.ttarch2",
                      TWD_FILE_OPEN_WRITE | TWD_FILE_CREATE, &f) != 0) {
        return 2;
    }
    if (twd_file_write(&f, pkg, sizeof(pkg)) != (int64_t)sizeof(pkg)) return 3;
    if (twd_file_close(&f) != 0) return 4;

    twd_boot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.game_dir = "twd_boot_smoke_dir";
    cfg.width = 1280u;
    cfg.height = 720u;
    cfg.max_frames = 4u;
    cfg.preferred_backend = -1;

    twd_boot_stats_t stats;
    int rc = twd_game_boot_run(&cfg, &stats);

    (void)twd_file_delete("twd_boot_smoke_dir/boot0.ttarch2");
    (void)twd_rmdir("twd_boot_smoke_dir");

    if (rc != 0) return 5;
    if (stats.hook_count == 0u) return 6;
    if (stats.mounted_archives != 1) return 7;
    if (stats.found_prefs_prop != 1 || stats.prefs_meta_version != 6u ||
        stats.prefs_property_count != 3u || stats.found_boot_lua != 1 ||
        stats.boot_script_ran != 1 || stats.boot_scripts_executed != 2u ||
        stats.boot_textures_loaded != 1u ||
        stats.boot_meshes_loaded != 1u ||
        stats.boot_scenes_active != 1u || stats.boot_scene_agents != 3u ||
        strcmp(stats.active_scene, "Menu.scene") != 0) return 8;
    if ((stats.audio_caps & TWD_AUDIO_CAP_MIXER) == 0) return 9;
    if (stats.width != 1600u || stats.height != 900u) return 10;
    if (stats.frames_rendered != 4ULL || stats.draw_calls < 4ULL || stats.clean_shutdown != 1) return 11;

    char kv[32];
    if (twd_kv_get("prefs", "windowed", kv, sizeof(kv)) <= 0 || strcmp(kv, "1") != 0) return 12;
    if (twd_kv_get("prefs", "fx_brush_quality", kv, sizeof(kv)) <= 0 || strcmp(kv, "2") != 0) return 13;
    if (twd_kv_get("lua.resource_set", "Boot", kv, sizeof(kv)) <= 0 || strcmp(kv, "1") != 0) return 14;
    if (twd_kv_get("lua.resource_set", "Menu", kv, sizeof(kv)) <= 0 || strcmp(kv, "1") != 0) return 15;
    if (twd_kv_get("lua.render", "texture_quality", kv, sizeof(kv)) <= 0 || strcmp(kv, "2") != 0) return 16;

    printf("test_game_boot: ok (hooks=%u, archives=%d, props=%u, lua=%u, tex=%u, res=%ux%u, frames=%llu)\n",
           stats.hook_count, stats.mounted_archives, stats.prefs_property_count,
           stats.boot_scripts_executed, stats.boot_textures_loaded, stats.width, stats.height,
           (unsigned long long)stats.frames_rendered);
    return 0;
}
