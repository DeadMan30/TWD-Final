/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Portable Audio & FMOD Studio / FMOD Core PAL for the TWD runtime.
 *
 * Dynamically probes native FMOD Studio / Core shared libraries and OS audio
 * output libraries when available, and provides a deterministic stateful
 * software FMOD Studio (System, Bank, Bus, EventInstance) + 48 kHz stereo
 * PCM16 software mixer fallback so the engine loop runs identically across
 * host PC, headless CI tests, and Android arm64-v8a.
 */
#include "twd_runtime.h"

#include <stdlib.h>
#include <string.h>

#define TWD_AUDIO_MAX_BANKS     64u
#define TWD_AUDIO_MAX_BUSES     32u
#define TWD_AUDIO_MAX_EVENTS    256u
#define TWD_AUDIO_MAX_INSTANCES 256u
#define TWD_AUDIO_MAX_SOUNDS    256u
#define TWD_AUDIO_MAX_CHANNELS  128u
#define TWD_AUDIO_MAX_PARAMS    16u

static void *g_fmod_studio_lib = NULL;
static void *g_fmod_core_lib = NULL;
static void *g_os_audio_lib = NULL;
static int g_audio_probed = 0;

typedef int (*fmod_sys_create_fn)(void **out_sys);
typedef int (*fmod_sys_getver_fn)(void *sys, uint32_t *out_ver);
typedef int (*fmod_sys_release_fn)(void *sys);
typedef int (*fmod_st_create_fn)(void **out_sys, uint32_t header_ver);
typedef int (*fmod_st_init_fn)(void *sys, int maxchannels, uint32_t sflags, uint32_t flags, void *extra);
typedef int (*fmod_st_release_fn)(void *sys);
typedef int (*fmod_st_update_fn)(void *sys);
typedef int (*fmod_st_loadbankmem_fn)(void *sys, const char *buf, int len, int mode, uint32_t flags, void **out_bank);
typedef int (*fmod_st_bank_unload_fn)(void *bank);
typedef int (*fmod_st_bank_evcnt_fn)(void *bank, int *out_cnt);
typedef int (*fmod_st_bank_evlist_fn)(void *bank, void **arr, int cap, int *out_cnt);
typedef int (*fmod_st_get_event_fn)(void *sys, const char *path, void **out_desc);
typedef int (*fmod_st_evdesc_create_fn)(void *desc, void **out_inst);
typedef int (*fmod_st_evinst_start_fn)(void *inst);
typedef int (*fmod_st_evinst_stop_fn)(void *inst, int mode);
typedef int (*fmod_st_evinst_setvol_fn)(void *inst, float vol);
typedef int (*fmod_st_evinst_rel_fn)(void *inst);
typedef int (*fmod_st_get_lowlevel_fn)(void *sys, void **out_core);
typedef int (*fmod_core_mk_snd_fn)(void *csys, const char *data, uint32_t mode, void *exinfo, void **out_snd);
typedef int (*fmod_core_play_snd_fn)(void *csys, void *snd, void *cgroup, int paused, void **out_ch);
typedef int (*fmod_snd_rel_fn)(void *snd);
typedef int (*fmod_snd_nsub_fn)(void *snd, int *out_num);
typedef int (*fmod_snd_gsub_fn)(void *snd, int idx, void **out_sub);
typedef int (*fmod_ch_stop_fn)(void *ch);
typedef int (*fmod_ch_setvol_fn)(void *ch, float vol);

struct twd_audio_bank {
    int in_use;
    int valid;
    int loading_state;
    uint32_t container_magic; /* "RIFF", "FSB5", or 0 */
    size_t byte_size;
    char name[128];
    twd_audio_system_t *sys;
    void *native_bank;
    uint32_t native_event_count;
};

struct twd_audio_bus {
    int in_use;
    char path[128];
    float volume;
    int muted;
    int paused;
};

struct twd_audio_param {
    char name[64];
    float value;
};

struct twd_audio_event_desc {
    int in_use;
    char path[128];
    uint32_t length_ms;
    int oneshot;
    char bus_path[128];
};

struct twd_audio_event_instance {
    int in_use;
    uint32_t magic;
    twd_audio_system_t *sys;
    twd_audio_event_desc_t *desc;
    twd_audio_bus_t *bus;
    int state; /* TWD_AUDIO_STOPPED / TWD_AUDIO_PLAYING / TWD_AUDIO_PAUSED */
    uint32_t timeline_ms;
    float volume;
    float pos[3];
    void *user_data;
    struct twd_audio_param params[TWD_AUDIO_MAX_PARAMS];
    uint32_t param_count;
    void *native_inst;
};

struct twd_audio_sound {
    int in_use;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t num_frames;
    int loop;
    int16_t *pcm; /* mono or interleaved stereo normalized on load */
};

struct twd_audio_channel {
    int in_use;
    int playing;
    int paused;
    twd_audio_sound_t *sound;
    twd_audio_bus_t *bus;
    uint64_t frame_cursor_fp; /* 32.32 fixed-point frame cursor for pitch support */
    float volume;
    float pan;
    float pitch;
};

struct twd_audio_system {
    uint32_t magic;
    int initialized;
    int suspended;
    uint32_t max_channels;
    uint32_t sample_rate;
    uint64_t update_count;
    uint64_t mixed_frames;
    float listener_pos[3];
    void *native_studio;
    void *native_core;
    void *voice_sound;
    void *voice_channel;
    uint32_t voice_play_count;
    fmod_st_release_fn fn_studio_release;
    fmod_st_update_fn fn_studio_update;
    fmod_st_loadbankmem_fn fn_load_bank_mem;
    fmod_st_bank_unload_fn fn_bank_unload;
    fmod_st_bank_evcnt_fn fn_bank_evcnt;
    fmod_st_bank_evlist_fn fn_bank_evlist;
    fmod_st_get_event_fn fn_studio_get_event;
    fmod_st_evdesc_create_fn fn_evdesc_create;
    fmod_st_evinst_start_fn fn_evinst_start;
    fmod_st_evinst_stop_fn fn_evinst_stop;
    fmod_st_evinst_setvol_fn fn_evinst_setvol;
    fmod_st_evinst_rel_fn fn_evinst_rel;
    fmod_st_get_lowlevel_fn fn_get_lowlevel;
    fmod_core_mk_snd_fn fn_core_create_sound;
    fmod_core_play_snd_fn fn_core_play_sound;
    fmod_snd_rel_fn fn_snd_release;
    fmod_snd_nsub_fn fn_snd_num_sub;
    fmod_snd_gsub_fn fn_snd_get_sub;
    fmod_ch_stop_fn fn_ch_stop;
    fmod_ch_setvol_fn fn_ch_setvol;
    twd_audio_bank_t banks[TWD_AUDIO_MAX_BANKS];
    twd_audio_bus_t buses[TWD_AUDIO_MAX_BUSES];
    twd_audio_event_desc_t events[TWD_AUDIO_MAX_EVENTS];
    twd_audio_event_instance_t instances[TWD_AUDIO_MAX_INSTANCES];
    twd_audio_sound_t sounds[TWD_AUDIO_MAX_SOUNDS];
    twd_audio_channel_t channels[TWD_AUDIO_MAX_CHANNELS];
};

#define TWD_AUDIO_SYS_MAGIC  0x41554453u
#define TWD_AUDIO_INST_MAGIC 0x41494E53u

static void copy_str_safe(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0u) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1u;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static uint32_t rd_au_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void probe_native_audio_libs(const char *studio_path, const char *core_path) {
    if (core_path && core_path[0] != '\0' && !g_fmod_core_lib) {
        g_fmod_core_lib = twd_dynlib_open(core_path);
    }
    if (studio_path && studio_path[0] != '\0' && !g_fmod_studio_lib) {
        g_fmod_studio_lib = twd_dynlib_open(studio_path);
    }
    if (g_audio_probed) return;
    g_audio_probed = 1;
#ifdef _WIN32
    if (!g_fmod_core_lib) g_fmod_core_lib = twd_dynlib_open("fmod64.dll");
    if (!g_fmod_core_lib) g_fmod_core_lib = twd_dynlib_open("game/fmod64.dll");
    if (!g_fmod_studio_lib) g_fmod_studio_lib = twd_dynlib_open("fmodstudio64.dll");
    if (!g_fmod_studio_lib) g_fmod_studio_lib = twd_dynlib_open("game/fmodstudio64.dll");
    if (!g_os_audio_lib) g_os_audio_lib = twd_dynlib_open("winmm.dll");
#elif defined(__ANDROID__)
    if (!g_fmod_core_lib) g_fmod_core_lib = twd_dynlib_open("libfmod.so");
    if (!g_fmod_studio_lib) g_fmod_studio_lib = twd_dynlib_open("libfmodstudio.so");
    if (!g_os_audio_lib) g_os_audio_lib = twd_dynlib_open("libOpenSLES.so");
#else
    if (!g_fmod_core_lib) g_fmod_core_lib = twd_dynlib_open("libfmod.so");
    if (!g_fmod_studio_lib) g_fmod_studio_lib = twd_dynlib_open("libfmodstudio.so");
#endif
}

int twd_audio_init(const char *studio_lib_or_null, const char *core_lib_or_null) {
    probe_native_audio_libs(studio_lib_or_null, core_lib_or_null);
    return twd_audio_backend_caps();
}

void twd_audio_shutdown(void) {
    if (g_fmod_studio_lib) {
        twd_dynlib_close(g_fmod_studio_lib);
        g_fmod_studio_lib = NULL;
    }
    if (g_fmod_core_lib) {
        twd_dynlib_close(g_fmod_core_lib);
        g_fmod_core_lib = NULL;
    }
    if (g_os_audio_lib) {
        twd_dynlib_close(g_os_audio_lib);
        g_os_audio_lib = NULL;
    }
    g_audio_probed = 0;
}

int twd_audio_backend_caps(void) {
    int caps = TWD_AUDIO_CAP_MIXER;
    if (g_fmod_studio_lib) caps |= TWD_AUDIO_CAP_FMOD_STUDIO;
    if (g_fmod_core_lib) caps |= TWD_AUDIO_CAP_FMOD_CORE;
    if (g_os_audio_lib) caps |= TWD_AUDIO_CAP_OS_OUTPUT;
    return caps;
}

static twd_audio_bus_t *ensure_bus(twd_audio_system_t *sys, const char *path) {
    if (!sys) return NULL;
    const char *p = (path && path[0] != '\0') ? path : "bus:/";
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_BUSES; ++i) {
        if (sys->buses[i].in_use && strcmp(sys->buses[i].path, p) == 0) {
            return &sys->buses[i];
        }
    }
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_BUSES; ++i) {
        if (!sys->buses[i].in_use) {
            twd_audio_bus_t *b = &sys->buses[i];
            memset(b, 0, sizeof(*b));
            b->in_use = 1;
            copy_str_safe(b->path, sizeof(b->path), p);
            b->volume = 1.0f;
            return b;
        }
    }
    return NULL;
}

twd_audio_system_t *twd_audio_system_create(void) {
    twd_audio_system_t *sys = (twd_audio_system_t *)calloc(1u, sizeof(twd_audio_system_t));
    if (!sys) return NULL;
    sys->magic = TWD_AUDIO_SYS_MAGIC;
    sys->sample_rate = 48000u;
    sys->max_channels = 64u;
    ensure_bus(sys, "bus:/");
    ensure_bus(sys, "bus:/Music");
    ensure_bus(sys, "bus:/SFX");
    ensure_bus(sys, "bus:/Dialogue");
    return sys;
}

int twd_audio_system_initialize(
    twd_audio_system_t *sys, uint32_t max_channels, uint32_t sample_rate_hz, uint32_t flags) {
    (void)flags;
    if (!sys || sys->magic != TWD_AUDIO_SYS_MAGIC) return -1;
    sys->max_channels = (max_channels > 0u && max_channels <= TWD_AUDIO_MAX_CHANNELS)
                            ? max_channels
                            : TWD_AUDIO_MAX_CHANNELS;
    sys->sample_rate = (sample_rate_hz >= 8000u && sample_rate_hz <= 192000u)
                           ? sample_rate_hz
                           : 48000u;
    if (g_fmod_studio_lib && !sys->native_studio) {
        uint32_t fmod_ver = 0x00011002u;
        if (g_fmod_core_lib) {
            fmod_sys_create_fn c_mk = (fmod_sys_create_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_System_Create");
            fmod_sys_getver_fn c_gv = (fmod_sys_getver_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_System_GetVersion");
            fmod_sys_release_fn c_rl = (fmod_sys_release_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_System_Release");
            void *csys = NULL;
            if (c_mk && c_gv && c_rl && c_mk(&csys) == 0 && csys) {
                uint32_t qv = 0u;
                if (c_gv(csys, &qv) == 0 && qv != 0u) fmod_ver = qv;
                c_rl(csys);
            }
        }
        fmod_st_create_fn st_mk = (fmod_st_create_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_System_Create");
        fmod_st_init_fn st_in = (fmod_st_init_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_System_Initialize");
        sys->fn_studio_release = (fmod_st_release_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_System_Release");
        sys->fn_studio_update = (fmod_st_update_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_System_Update");
        sys->fn_load_bank_mem = (fmod_st_loadbankmem_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_System_LoadBankMemory");
        sys->fn_bank_unload = (fmod_st_bank_unload_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_Bank_Unload");
        sys->fn_bank_evcnt = (fmod_st_bank_evcnt_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_Bank_GetEventCount");
        sys->fn_bank_evlist = (fmod_st_bank_evlist_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_Bank_GetEventList");
        sys->fn_studio_get_event = (fmod_st_get_event_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_System_GetEvent");
        sys->fn_evdesc_create = (fmod_st_evdesc_create_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_EventDescription_CreateInstance");
        sys->fn_evinst_start = (fmod_st_evinst_start_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_EventInstance_Start");
        sys->fn_evinst_stop = (fmod_st_evinst_stop_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_EventInstance_Stop");
        sys->fn_evinst_setvol = (fmod_st_evinst_setvol_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_EventInstance_SetVolume");
        sys->fn_evinst_rel = (fmod_st_evinst_rel_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_EventInstance_Release");
        sys->fn_get_lowlevel = (fmod_st_get_lowlevel_fn)(uintptr_t)twd_dynlib_sym(g_fmod_studio_lib, "FMOD_Studio_System_GetLowLevelSystem");
        if (g_fmod_core_lib) {
            sys->fn_core_create_sound = (fmod_core_mk_snd_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_System_CreateSound");
            sys->fn_core_play_sound = (fmod_core_play_snd_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_System_PlaySound");
            sys->fn_snd_release = (fmod_snd_rel_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_Sound_Release");
            sys->fn_snd_num_sub = (fmod_snd_nsub_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_Sound_GetNumSubSounds");
            sys->fn_snd_get_sub = (fmod_snd_gsub_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_Sound_GetSubSound");
            sys->fn_ch_stop = (fmod_ch_stop_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_Channel_Stop");
            sys->fn_ch_setvol = (fmod_ch_setvol_fn)(uintptr_t)twd_dynlib_sym(g_fmod_core_lib, "FMOD_Channel_SetVolume");
        }
        if (st_mk && st_in) {
            void *nsys = NULL;
            if (st_mk(&nsys, fmod_ver) == 0 && nsys) {
                if (st_in(nsys, (int)sys->max_channels, 0u, 0u, NULL) == 0) {
                    sys->native_studio = nsys;
                    if (sys->fn_get_lowlevel) {
                        (void)sys->fn_get_lowlevel(nsys, &sys->native_core);
                    }
                } else if (sys->fn_studio_release) {
                    sys->fn_studio_release(nsys);
                }
            }
        }
    }
    sys->initialized = 1;
    return 0;
}

static void release_voice_slot(twd_audio_system_t *sys) {
    if (!sys) return;
    if (sys->voice_channel && sys->fn_ch_stop) {
        (void)sys->fn_ch_stop(sys->voice_channel);
    }
    sys->voice_channel = NULL;
    if (sys->voice_sound && sys->fn_snd_release) {
        (void)sys->fn_snd_release(sys->voice_sound);
    }
    sys->voice_sound = NULL;
}

void twd_audio_system_release(twd_audio_system_t *sys) {
    if (!sys) return;
    release_voice_slot(sys);
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_INSTANCES; ++i) {
        if (sys->instances[i].in_use && sys->instances[i].native_inst) {
            if (sys->fn_evinst_stop) (void)sys->fn_evinst_stop(sys->instances[i].native_inst, 1);
            if (sys->fn_evinst_rel) (void)sys->fn_evinst_rel(sys->instances[i].native_inst);
            sys->instances[i].native_inst = NULL;
        }
    }
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_BANKS; ++i) {
        if (sys->banks[i].in_use && sys->banks[i].native_bank) {
            if (sys->fn_bank_unload) (void)sys->fn_bank_unload(sys->banks[i].native_bank);
            sys->banks[i].native_bank = NULL;
        }
    }
    if (sys->native_studio && sys->fn_studio_release) {
        (void)sys->fn_studio_release(sys->native_studio);
        sys->native_studio = NULL;
    }
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_SOUNDS; ++i) {
        if (sys->sounds[i].in_use) {
            free(sys->sounds[i].pcm);
            sys->sounds[i].pcm = NULL;
            sys->sounds[i].in_use = 0;
        }
    }
    memset(sys, 0, sizeof(*sys));
    free(sys);
}

int twd_audio_system_mixer_suspend(twd_audio_system_t *sys) {
    if (!sys || !sys->initialized) return -1;
    sys->suspended = 1;
    return 0;
}

int twd_audio_system_mixer_resume(twd_audio_system_t *sys) {
    if (!sys || !sys->initialized) return -1;
    sys->suspended = 0;
    return 0;
}

int twd_audio_system_is_suspended(const twd_audio_system_t *sys) {
    return (sys && sys->suspended) ? 1 : 0;
}

int twd_audio_system_set_listener(twd_audio_system_t *sys, float x, float y, float z) {
    if (!sys || !sys->initialized) return -1;
    sys->listener_pos[0] = x;
    sys->listener_pos[1] = y;
    sys->listener_pos[2] = z;
    return 0;
}

int twd_audio_system_update(twd_audio_system_t *sys, uint32_t dt_ms) {
    if (!sys || !sys->initialized) return -1;
    sys->update_count++;
    if (sys->native_studio && sys->fn_studio_update) {
        (void)sys->fn_studio_update(sys->native_studio);
    }
    if (sys->suspended) return 0;

    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_INSTANCES; ++i) {
        twd_audio_event_instance_t *inst = &sys->instances[i];
        if (!inst->in_use || inst->state != TWD_AUDIO_PLAYING) continue;
        if (inst->bus && inst->bus->paused) continue;
        inst->timeline_ms += dt_ms;
        if (inst->desc && inst->desc->oneshot && inst->desc->length_ms > 0u &&
            inst->timeline_ms >= inst->desc->length_ms) {
            inst->timeline_ms = inst->desc->length_ms;
            inst->state = TWD_AUDIO_STOPPED;
        }
    }
    return 0;
}

twd_audio_bank_t *twd_audio_load_bank_memory(
    twd_audio_system_t *sys, const char *name, const void *data, size_t len) {
    if (!sys || !sys->initialized || !data || len < 4u) return NULL;
    const uint8_t *b = (const uint8_t *)data;
    uint32_t magic = rd_au_u32(b);
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_BANKS; ++i) {
        if (!sys->banks[i].in_use) {
            twd_audio_bank_t *bk = &sys->banks[i];
            memset(bk, 0, sizeof(*bk));
            bk->in_use = 1;
            bk->valid = 1;
            bk->loading_state = TWD_AUDIO_LOAD_LOADED;
            bk->container_magic = magic;
            bk->byte_size = len;
            bk->sys = sys;
            copy_str_safe(bk->name, sizeof(bk->name), name ? name : "memory.bank");
            if (sys->native_studio && sys->fn_load_bank_mem && len <= 0x7fffffffu) {
                void *nb = NULL;
                if (sys->fn_load_bank_mem(sys->native_studio, (const char *)data, (int)len, 0, 0u, &nb) == 0 && nb) {
                    bk->native_bank = nb;
                    if (sys->fn_bank_evcnt) {
                        int ec = 0;
                        if (sys->fn_bank_evcnt(nb, &ec) == 0 && ec > 0) {
                            bk->native_event_count = (uint32_t)ec;
                        }
                    }
                }
            }
            return bk;
        }
    }
    return NULL;
}

twd_audio_bank_t *twd_audio_load_bank_vfs(twd_audio_system_t *sys, const char *rel_path) {
    if (!sys || !sys->initialized || !rel_path || rel_path[0] == '\0') return NULL;
    size_t sz = 0u;
    void *data = twd_asset_read_alloc(rel_path, &sz);
    if (!data) return NULL;
    twd_audio_bank_t *bk = twd_audio_load_bank_memory(sys, rel_path, data, sz);
    twd_asset_free(data);
    return bk;
}

int twd_audio_bank_unload(twd_audio_bank_t *bank) {
    if (!bank || !bank->in_use) return -1;
    if (bank->native_bank && bank->sys && bank->sys->fn_bank_unload) {
        (void)bank->sys->fn_bank_unload(bank->native_bank);
        bank->native_bank = NULL;
    }
    memset(bank, 0, sizeof(*bank));
    return 0;
}

int twd_audio_bank_is_valid(const twd_audio_bank_t *bank) {
    return (bank && bank->in_use && bank->valid) ? 1 : 0;
}

uint32_t twd_audio_bank_count(const twd_audio_system_t *sys) {
    if (!sys) return 0u;
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_BANKS; ++i) {
        if (sys->banks[i].in_use) ++n;
    }
    return n;
}

uint32_t twd_audio_bank_event_count(const twd_audio_bank_t *bank) {
    if (!bank || !bank->in_use) return 0u;
    return bank->native_event_count;
}

int twd_audio_play_bank_event(
    twd_audio_system_t *sys, twd_audio_bank_t *bank, uint32_t event_index, float volume) {
    if (!sys || !sys->initialized || !bank || !bank->in_use || !bank->native_bank) return -1;
    if (!sys->fn_bank_evlist || !sys->fn_evdesc_create || !sys->fn_evinst_start) return -1;
    if (bank->native_event_count == 0u || event_index >= bank->native_event_count) return -1;

    uint32_t cap = bank->native_event_count;
    if (cap > 128u) cap = 128u;
    if (event_index >= cap) return -1;
    void *ev_arr[128];
    int got = 0;
    if (sys->fn_bank_evlist(bank->native_bank, ev_arr, (int)cap, &got) != 0 ||
        got <= (int)event_index || !ev_arr[event_index]) {
        return -1;
    }

    void *ninst = NULL;
    if (sys->fn_evdesc_create(ev_arr[event_index], &ninst) != 0 || !ninst) return -1;
    if (volume < 0.0f) volume = 0.0f;
    if (sys->fn_evinst_setvol) (void)sys->fn_evinst_setvol(ninst, volume);
    if (sys->fn_evinst_start(ninst) != 0) {
        if (sys->fn_evinst_rel) (void)sys->fn_evinst_rel(ninst);
        return -1;
    }

    twd_audio_event_desc_t *desc = twd_audio_register_event(sys, bank->name, 0u, 0, "bus:/Music");
    twd_audio_event_instance_t *slot = NULL;
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_INSTANCES; ++i) {
        if (!sys->instances[i].in_use) {
            slot = &sys->instances[i];
            memset(slot, 0, sizeof(*slot));
            slot->in_use = 1;
            slot->magic = TWD_AUDIO_INST_MAGIC;
            slot->sys = sys;
            slot->desc = desc;
            slot->bus = ensure_bus(sys, "bus:/Music");
            slot->state = TWD_AUDIO_PLAYING;
            slot->volume = volume;
            slot->native_inst = ninst;
            break;
        }
    }
    if (!slot) {
        if (sys->fn_evinst_stop) (void)sys->fn_evinst_stop(ninst, 1);
        if (sys->fn_evinst_rel) (void)sys->fn_evinst_rel(ninst);
        return -1;
    }
    if (sys->fn_studio_update) (void)sys->fn_studio_update(sys->native_studio);
    return 0;
}

int twd_audio_play_fsb_memory(
    twd_audio_system_t *sys, const char *name, const void *data, size_t len, float volume) {
    (void)name;
    if (!sys || !sys->initialized || !data || len < 4u || len > 0x7fffffffu) return -1;
    const uint8_t *b = (const uint8_t *)data;
    int is_valid_hdr = (memcmp(b, "FSB5", 4u) == 0 || memcmp(b, "RIFF", 4u) == 0 || memcmp(b, "OggS", 4u) == 0);
    if (!is_valid_hdr) return -1;

    release_voice_slot(sys);
    sys->voice_play_count++;

    if (!sys->native_core || !sys->fn_core_create_sound || !sys->fn_core_play_sound) {
        return 0; /* Valid container tracked cleanly by software voice state when native FMOD absent */
    }
    uint8_t exinfo[224];
    memset(exinfo, 0, sizeof(exinfo));
    uint32_t cbsize = (uint32_t)sizeof(exinfo);
    uint32_t ulen = (uint32_t)len;
    memcpy(exinfo + 0u, &cbsize, 4u);
    memcpy(exinfo + 4u, &ulen, 4u);

    void *snd = NULL;
    if (sys->fn_core_create_sound(sys->native_core, (const char *)data, 0x00000800u, exinfo, &snd) != 0 || !snd) {
        return -1;
    }
    void *play_target = snd;
    if (sys->fn_snd_num_sub && sys->fn_snd_get_sub) {
        int nsub = 0;
        if (sys->fn_snd_num_sub(snd, &nsub) == 0 && nsub > 0) {
            void *sub = NULL;
            if (sys->fn_snd_get_sub(snd, 0, &sub) == 0 && sub) {
                play_target = sub;
            }
        }
    }
    void *ch = NULL;
    if (sys->fn_core_play_sound(sys->native_core, play_target, NULL, 0, &ch) != 0) {
        if (sys->fn_snd_release) (void)sys->fn_snd_release(snd);
        return -1;
    }
    if (volume < 0.0f) volume = 0.0f;
    if (ch && sys->fn_ch_setvol) (void)sys->fn_ch_setvol(ch, volume);
    sys->voice_sound = snd;
    sys->voice_channel = ch;
    if (sys->native_studio && sys->fn_studio_update) (void)sys->fn_studio_update(sys->native_studio);
    return 0;
}

int twd_audio_play_fsb_vfs(
    twd_audio_system_t *sys, const char *rel_path, float volume) {
    if (!sys || !sys->initialized || !rel_path || rel_path[0] == '\0') return -1;
    size_t sz = 0u;
    void *data = twd_asset_read_alloc(rel_path, &sz);
    if (!data) return -1;
    int rc = twd_audio_play_fsb_memory(sys, rel_path, data, sz, volume);
    twd_asset_free(data);
    return rc;
}

uint32_t twd_audio_voice_play_count(const twd_audio_system_t *sys) {
    return sys ? sys->voice_play_count : 0u;
}

twd_audio_bus_t *twd_audio_get_bus(twd_audio_system_t *sys, const char *bus_path) {
    return ensure_bus(sys, bus_path);
}

int twd_audio_bus_set_volume(twd_audio_bus_t *bus, float volume) {
    if (!bus || !bus->in_use) return -1;
    if (volume < 0.0f) volume = 0.0f;
    bus->volume = volume;
    return 0;
}

float twd_audio_bus_get_volume(const twd_audio_bus_t *bus) {
    return (bus && bus->in_use) ? bus->volume : 0.0f;
}

int twd_audio_bus_set_mute(twd_audio_bus_t *bus, int muted) {
    if (!bus || !bus->in_use) return -1;
    bus->muted = muted ? 1 : 0;
    return 0;
}

int twd_audio_bus_set_paused(twd_audio_bus_t *bus, int paused) {
    if (!bus || !bus->in_use) return -1;
    bus->paused = paused ? 1 : 0;
    return 0;
}

static float bus_effective_gain(const twd_audio_system_t *sys, const twd_audio_bus_t *bus) {
    if (!sys || sys->suspended) return 0.0f;
    const twd_audio_bus_t *master = &sys->buses[0];
    float g = (master->in_use && !master->muted) ? master->volume : 0.0f;
    if (bus && bus != master) {
        if (!bus->in_use || bus->muted) return 0.0f;
        g *= bus->volume;
    }
    return g;
}

twd_audio_event_desc_t *twd_audio_register_event(
    twd_audio_system_t *sys, const char *path, uint32_t length_ms, int oneshot,
    const char *bus_path_or_null) {
    if (!sys || !sys->initialized || !path || path[0] == '\0') return NULL;
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_EVENTS; ++i) {
        if (sys->events[i].in_use && strcmp(sys->events[i].path, path) == 0) {
            sys->events[i].length_ms = length_ms;
            sys->events[i].oneshot = oneshot ? 1 : 0;
            copy_str_safe(sys->events[i].bus_path, sizeof(sys->events[i].bus_path),
                          bus_path_or_null ? bus_path_or_null : "bus:/");
            return &sys->events[i];
        }
    }
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_EVENTS; ++i) {
        if (!sys->events[i].in_use) {
            twd_audio_event_desc_t *d = &sys->events[i];
            memset(d, 0, sizeof(*d));
            d->in_use = 1;
            copy_str_safe(d->path, sizeof(d->path), path);
            d->length_ms = length_ms;
            d->oneshot = oneshot ? 1 : 0;
            copy_str_safe(d->bus_path, sizeof(d->bus_path),
                          bus_path_or_null ? bus_path_or_null : "bus:/");
            return d;
        }
    }
    return NULL;
}

twd_audio_event_desc_t *twd_audio_get_event(twd_audio_system_t *sys, const char *path) {
    if (!sys || !sys->initialized || !path || path[0] == '\0') return NULL;
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_EVENTS; ++i) {
        if (sys->events[i].in_use && strcmp(sys->events[i].path, path) == 0) {
            return &sys->events[i];
        }
    }
    /* Auto-register dynamic FMOD event references when banks are mounted. */
    if (twd_audio_bank_count(sys) > 0u) {
        return twd_audio_register_event(sys, path, 1000u, 1, "bus:/");
    }
    return NULL;
}

twd_audio_event_instance_t *twd_audio_create_instance(
    twd_audio_system_t *sys, twd_audio_event_desc_t *desc) {
    if (!sys || !sys->initialized || !desc || !desc->in_use) return NULL;
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_INSTANCES; ++i) {
        if (!sys->instances[i].in_use) {
            twd_audio_event_instance_t *inst = &sys->instances[i];
            memset(inst, 0, sizeof(*inst));
            inst->in_use = 1;
            inst->magic = TWD_AUDIO_INST_MAGIC;
            inst->sys = sys;
            inst->desc = desc;
            inst->bus = ensure_bus(sys, desc->bus_path);
            inst->state = TWD_AUDIO_STOPPED;
            inst->volume = 1.0f;
            if (sys->native_studio && sys->fn_studio_get_event && sys->fn_evdesc_create) {
                void *edesc = NULL;
                if (sys->fn_studio_get_event(sys->native_studio, desc->path, &edesc) == 0 && edesc) {
                    (void)sys->fn_evdesc_create(edesc, &inst->native_inst);
                }
            }
            return inst;
        }
    }
    return NULL;
}

void twd_audio_instance_release(twd_audio_event_instance_t *inst) {
    if (!inst || inst->magic != TWD_AUDIO_INST_MAGIC) return;
    if (inst->native_inst && inst->sys) {
        if (inst->sys->fn_evinst_stop) (void)inst->sys->fn_evinst_stop(inst->native_inst, 1);
        if (inst->sys->fn_evinst_rel) (void)inst->sys->fn_evinst_rel(inst->native_inst);
        inst->native_inst = NULL;
    }
    memset(inst, 0, sizeof(*inst));
}

int twd_audio_instance_start(twd_audio_event_instance_t *inst) {
    if (!inst || !inst->in_use || inst->magic != TWD_AUDIO_INST_MAGIC) return -1;
    inst->timeline_ms = 0u;
    inst->state = TWD_AUDIO_PLAYING;
    if (inst->native_inst && inst->sys && inst->sys->fn_evinst_start) {
        (void)inst->sys->fn_evinst_start(inst->native_inst);
    }
    return 0;
}

int twd_audio_instance_stop(twd_audio_event_instance_t *inst, int immediate) {
    if (!inst || !inst->in_use || inst->magic != TWD_AUDIO_INST_MAGIC) return -1;
    inst->state = TWD_AUDIO_STOPPED;
    if (inst->native_inst && inst->sys && inst->sys->fn_evinst_stop) {
        (void)inst->sys->fn_evinst_stop(inst->native_inst, immediate ? 1 : 0);
    }
    return 0;
}

int twd_audio_instance_set_paused(twd_audio_event_instance_t *inst, int paused) {
    if (!inst || !inst->in_use || inst->magic != TWD_AUDIO_INST_MAGIC) return -1;
    if (paused && inst->state == TWD_AUDIO_PLAYING) {
        inst->state = TWD_AUDIO_PAUSED;
    } else if (!paused && inst->state == TWD_AUDIO_PAUSED) {
        inst->state = TWD_AUDIO_PLAYING;
    }
    return 0;
}

int twd_audio_instance_set_volume(twd_audio_event_instance_t *inst, float volume) {
    if (!inst || !inst->in_use || inst->magic != TWD_AUDIO_INST_MAGIC) return -1;
    if (volume < 0.0f) volume = 0.0f;
    inst->volume = volume;
    if (inst->native_inst && inst->sys && inst->sys->fn_evinst_setvol) {
        (void)inst->sys->fn_evinst_setvol(inst->native_inst, volume);
    }
    return 0;
}

int twd_audio_instance_set_timeline_ms(twd_audio_event_instance_t *inst, uint32_t ms) {
    if (!inst || !inst->in_use || inst->magic != TWD_AUDIO_INST_MAGIC) return -1;
    inst->timeline_ms = ms;
    return 0;
}

uint32_t twd_audio_instance_get_timeline_ms(const twd_audio_event_instance_t *inst) {
    return (inst && inst->in_use) ? inst->timeline_ms : 0u;
}

int twd_audio_instance_get_state(const twd_audio_event_instance_t *inst) {
    return (inst && inst->in_use) ? inst->state : TWD_AUDIO_STOPPED;
}

int twd_audio_instance_set_parameter(
    twd_audio_event_instance_t *inst, const char *name, float value) {
    if (!inst || !inst->in_use || !name || name[0] == '\0') return -1;
    for (uint32_t i = 0u; i < inst->param_count; ++i) {
        if (strcmp(inst->params[i].name, name) == 0) {
            inst->params[i].value = value;
            return 0;
        }
    }
    if (inst->param_count >= TWD_AUDIO_MAX_PARAMS) return -1;
    copy_str_safe(inst->params[inst->param_count].name, 64u, name);
    inst->params[inst->param_count].value = value;
    inst->param_count++;
    return 0;
}

int twd_audio_instance_get_parameter(
    const twd_audio_event_instance_t *inst, const char *name, float *out_val) {
    if (out_val) *out_val = 0.0f;
    if (!inst || !inst->in_use || !name || name[0] == '\0') return -1;
    for (uint32_t i = 0u; i < inst->param_count; ++i) {
        if (strcmp(inst->params[i].name, name) == 0) {
            if (out_val) *out_val = inst->params[i].value;
            return 0;
        }
    }
    return -1;
}

void twd_audio_instance_set_3d(
    twd_audio_event_instance_t *inst, float x, float y, float z) {
    if (!inst || !inst->in_use) return;
    inst->pos[0] = x;
    inst->pos[1] = y;
    inst->pos[2] = z;
}

void twd_audio_instance_set_userdata(twd_audio_event_instance_t *inst, void *user) {
    if (inst && inst->in_use) inst->user_data = user;
}

void *twd_audio_instance_get_userdata(const twd_audio_event_instance_t *inst) {
    return (inst && inst->in_use) ? inst->user_data : NULL;
}

twd_audio_sound_t *twd_audio_create_sound_pcm16(
    twd_audio_system_t *sys, const int16_t *samples, uint32_t num_frames,
    uint32_t channels, uint32_t sample_rate, int loop) {
    if (!sys || !sys->initialized || !samples || num_frames == 0u ||
        (channels != 1u && channels != 2u) || sample_rate == 0u) {
        return NULL;
    }
    for (uint32_t i = 0u; i < TWD_AUDIO_MAX_SOUNDS; ++i) {
        if (!sys->sounds[i].in_use) {
            twd_audio_sound_t *s = &sys->sounds[i];
            size_t total = (size_t)num_frames * (size_t)channels;
            int16_t *copy = (int16_t *)malloc(total * sizeof(int16_t));
            if (!copy) return NULL;
            memcpy(copy, samples, total * sizeof(int16_t));
            s->in_use = 1;
            s->channels = channels;
            s->sample_rate = sample_rate;
            s->num_frames = num_frames;
            s->loop = loop ? 1 : 0;
            s->pcm = copy;
            return s;
        }
    }
    return NULL;
}

void twd_audio_sound_release(twd_audio_sound_t *sound) {
    if (!sound || !sound->in_use) return;
    free(sound->pcm);
    memset(sound, 0, sizeof(*sound));
}

twd_audio_channel_t *twd_audio_play_sound(
    twd_audio_system_t *sys, twd_audio_sound_t *sound, twd_audio_bus_t *bus, int paused) {
    if (!sys || !sys->initialized || !sound || !sound->in_use) return NULL;
    uint32_t limit = sys->max_channels;
    if (limit > TWD_AUDIO_MAX_CHANNELS) limit = TWD_AUDIO_MAX_CHANNELS;
    for (uint32_t i = 0u; i < limit; ++i) {
        if (!sys->channels[i].in_use) {
            twd_audio_channel_t *ch = &sys->channels[i];
            memset(ch, 0, sizeof(*ch));
            ch->in_use = 1;
            ch->playing = 1;
            ch->paused = paused ? 1 : 0;
            ch->sound = sound;
            ch->bus = bus ? bus : &sys->buses[0];
            ch->volume = 1.0f;
            ch->pan = 0.0f;
            ch->pitch = 1.0f;
            return ch;
        }
    }
    return NULL;
}

int twd_audio_channel_stop(twd_audio_channel_t *ch) {
    if (!ch || !ch->in_use) return -1;
    ch->playing = 0;
    ch->in_use = 0;
    return 0;
}

int twd_audio_channel_set_paused(twd_audio_channel_t *ch, int paused) {
    if (!ch || !ch->in_use) return -1;
    ch->paused = paused ? 1 : 0;
    return 0;
}

int twd_audio_channel_set_volume(twd_audio_channel_t *ch, float volume) {
    if (!ch || !ch->in_use) return -1;
    if (volume < 0.0f) volume = 0.0f;
    ch->volume = volume;
    return 0;
}

int twd_audio_channel_set_pan(twd_audio_channel_t *ch, float pan) {
    if (!ch || !ch->in_use) return -1;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    ch->pan = pan;
    return 0;
}

int twd_audio_channel_set_pitch(twd_audio_channel_t *ch, float pitch) {
    if (!ch || !ch->in_use || pitch <= 0.0f) return -1;
    ch->pitch = pitch;
    return 0;
}

int twd_audio_channel_is_playing(const twd_audio_channel_t *ch) {
    return (ch && ch->in_use && ch->playing) ? 1 : 0;
}

static int16_t clamp_i16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

uint32_t twd_audio_mix_pcm_i16(
    twd_audio_system_t *sys, int16_t *out_stereo_i16, uint32_t num_frames) {
    if (!sys || !sys->initialized || !out_stereo_i16 || num_frames == 0u) return 0u;
    memset(out_stereo_i16, 0, (size_t)num_frames * 2u * sizeof(int16_t));
    if (sys->suspended) {
        sys->mixed_frames += (uint64_t)num_frames;
        return num_frames;
    }

    int32_t *acc = (int32_t *)calloc((size_t)num_frames * 2u, sizeof(int32_t));
    if (!acc) return 0u;

    for (uint32_t ci = 0u; ci < TWD_AUDIO_MAX_CHANNELS; ++ci) {
        twd_audio_channel_t *ch = &sys->channels[ci];
        if (!ch->in_use || !ch->playing || ch->paused || !ch->sound || !ch->sound->pcm) continue;
        if (ch->bus && ch->bus->paused) continue;

        float bg = bus_effective_gain(sys, ch->bus) * ch->volume;
        if (bg <= 0.00001f) continue;

        float pan_norm = (ch->pan + 1.0f) * 0.5f;
        float gain_l = bg * ((pan_norm <= 0.5f) ? 1.0f : (2.0f * (1.0f - pan_norm)));
        float gain_r = bg * ((pan_norm >= 0.5f) ? 1.0f : (2.0f * pan_norm));

        double step_d = ((double)ch->sound->sample_rate / (double)sys->sample_rate) * (double)ch->pitch;
        if (step_d <= 0.0) step_d = 1.0;
        uint64_t step_fp = (uint64_t)(step_d * 65536.0 + 0.5);
        if (step_fp == 0ULL) step_fp = 1ULL;

        const twd_audio_sound_t *snd = ch->sound;
        for (uint32_t f = 0u; f < num_frames; ++f) {
            uint64_t idx = ch->frame_cursor_fp >> 16;
            if (idx >= (uint64_t)snd->num_frames) {
                if (snd->loop) {
                    idx %= (uint64_t)snd->num_frames;
                    ch->frame_cursor_fp = (idx << 16) | (ch->frame_cursor_fp & 0xFFFFULL);
                } else {
                    ch->playing = 0;
                    ch->in_use = 0;
                    break;
                }
            }
            int32_t s_l = 0;
            int32_t s_r = 0;
            if (snd->channels == 1u) {
                s_l = snd->pcm[(size_t)idx];
                s_r = s_l;
            } else {
                s_l = snd->pcm[(size_t)idx * 2u];
                s_r = snd->pcm[(size_t)idx * 2u + 1u];
            }
            float fl = (float)s_l * gain_l;
            float fr = (float)s_r * gain_r;
            acc[(size_t)f * 2u] += (fl >= 0.0f) ? (int32_t)(fl + 0.5f) : (int32_t)(fl - 0.5f);
            acc[(size_t)f * 2u + 1u] += (fr >= 0.0f) ? (int32_t)(fr + 0.5f) : (int32_t)(fr - 0.5f);
            ch->frame_cursor_fp += step_fp;
        }
    }

    for (size_t i = 0u; i < (size_t)num_frames * 2u; ++i) {
        out_stereo_i16[i] = clamp_i16(acc[i]);
    }
    free(acc);
    sys->mixed_frames += (uint64_t)num_frames;
    return num_frames;
}
