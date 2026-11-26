/* SPDX-License-Identifier: GPL-2.0-only */
#include "twd_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u32_arg(const char *s, uint32_t *out) {
    if (!s || s[0] == '\0' || !out) return -1;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = (uint32_t)v;
    return 0;
}

int main(int argc, char **argv) {
    twd_boot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.preferred_backend = -1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc) {
            cfg.game_dir = argv[++i];
        } else if (strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) {
            cfg.save_dir = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if (parse_u32_arg(argv[++i], &cfg.width) != 0) return 2;
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if (parse_u32_arg(argv[++i], &cfg.height) != 0) return 2;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (parse_u32_arg(argv[++i], &cfg.max_frames) != 0) return 2;
        } else if (strcmp(argv[i], "--sw") == 0) {
            cfg.preferred_backend = TWD_GFX_BACKEND_NONE;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: twd_boot [--game-dir DIR] [--save-dir DIR] [--width W] [--height H] [--frames N] [--sw]\n");
            printf("Optional archive decryption keys are read from TWD_ARCHIVE_KEY or TWD_ARCHIVE_KEY_HEX.\n");
            return 0;
        } else {
            fprintf(stderr, "twd_boot: unknown or incomplete option '%s'\n", argv[i]);
            return 2;
        }
    }

    twd_boot_stats_t stats;
    int rc = twd_game_boot_run(&cfg, &stats);
    printf("twd_boot: rc=%d archives=%d lua=%d tex=%u mesh=%u scenes=%u agents=%u active='%s' frames=%llu draws=%llu clean=%d\n",
           rc, stats.mounted_archives, stats.boot_script_ran, stats.boot_textures_loaded,
           stats.boot_meshes_loaded, stats.boot_scenes_active, stats.boot_scene_agents,
           stats.active_scene, (unsigned long long)stats.frames_rendered,
           (unsigned long long)stats.draw_calls, stats.clean_shutdown);
    return rc == 0 ? 0 : 1;
}
