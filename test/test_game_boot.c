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
    static const char s_boot_lua[] = "-- synthetic boot";
    static const char s_prefs[] = "window=1280x720";
    static const uint8_t s_bank[16] = {'R','I','F','F', 8,0,0,0, 'F','E','V',' ', 1,0,0,0};
    static const char names[] = "_boot.lua\0prefs.prop\0Master.bank\0";

    enum {
        N_ENT = 3,
        NAMES_SZ = sizeof(names) - 1,
        DATA_BASE = 12 + N_ENT * 28 + NAMES_SZ,
        BOOT_LEN = sizeof(s_boot_lua) - 1,
        PREFS_LEN = sizeof(s_prefs) - 1,
        BANK_LEN = sizeof(s_bank),
        PAYLOAD_SZ = DATA_BASE + BOOT_LEN + PREFS_LEN + BANK_LEN
    };

    uint8_t pkg[12 + PAYLOAD_SZ];
    memset(pkg, 0, sizeof(pkg));
    memcpy(pkg, "NCTT", 4u);
    wr_u64(pkg + 4u, (uint64_t)PAYLOAD_SZ);

    uint8_t *toc = pkg + 12u;
    wr_u32(toc + 0u, TWD_TTARCH2_TOC_TTA4);
    wr_u32(toc + 4u, (uint32_t)NAMES_SZ);
    wr_u32(toc + 8u, (uint32_t)N_ENT);

    wr_u64(toc + 12u + 0u, 1ULL);
    wr_u64(toc + 12u + 8u, 0ULL);
    wr_u32(toc + 12u + 16u, (uint32_t)BOOT_LEN);
    wr_u16(toc + 12u + 26u, 0u);

    wr_u64(toc + 40u + 0u, 2ULL);
    wr_u64(toc + 40u + 8u, (uint64_t)BOOT_LEN);
    wr_u32(toc + 40u + 16u, (uint32_t)PREFS_LEN);
    wr_u16(toc + 40u + 26u, 10u);

    wr_u64(toc + 68u + 0u, 3ULL);
    wr_u64(toc + 68u + 8u, (uint64_t)(BOOT_LEN + PREFS_LEN));
    wr_u32(toc + 68u + 16u, (uint32_t)BANK_LEN);
    wr_u16(toc + 68u + 26u, 21u);

    memcpy(toc + 12u + N_ENT * 28u, names, NAMES_SZ);
    memcpy(toc + DATA_BASE, s_boot_lua, BOOT_LEN);
    memcpy(toc + DATA_BASE + BOOT_LEN, s_prefs, PREFS_LEN);
    memcpy(toc + DATA_BASE + BOOT_LEN + PREFS_LEN, s_bank, BANK_LEN);

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
    if (stats.found_prefs_prop != 1 || stats.found_boot_lua != 1) return 8;
    if ((stats.audio_caps & TWD_AUDIO_CAP_MIXER) == 0) return 9;
    if (stats.width != 1280u || stats.height != 720u) return 10;
    if (stats.frames_rendered != 4ULL || stats.clean_shutdown != 1) return 11;

    printf("test_game_boot: ok (hooks=%u, archives=%d, frames=%llu)\n",
           stats.hook_count, stats.mounted_archives,
           (unsigned long long)stats.frames_rendered);
    return 0;
}
