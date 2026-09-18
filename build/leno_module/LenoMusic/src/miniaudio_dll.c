/*
 * miniaudio DLL v4 -- streaming playback, UTF-8 path support, enhanced controls
 * Changes from v3:
 *   - UTF-8 path support on Windows (fixes Chinese/non-ASCII file paths)
 *   - Uses ma_sound_init_from_file_w / ma_decoder_init_file_w internally
 * Changes from v2:
 *   - MA_SOUND_FLAG_STREAM instead of MA_SOUND_FLAG_DECODE (low memory)
 *   - Sound: looping, pitch, pan controls
 *   - Seek: uses actual engine sample rate, not hardcoded 48000
 *   - Removed math utilities and debug helpers (Leno has its own math module)
 * Compile: gcc -shared -o miniaudio.dll miniaudio_dll.c -O2 -Wall -lwinmm -lole32 -lksuser -lm -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
 */
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "stb_vorbis.c"

#ifndef STB_VORBIS_INCLUDE_STB_VORBIS_H
#define STB_VORBIS_INCLUDE_STB_VORBIS_H
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <math.h>

#ifdef _WIN32
    #include <windows.h>
    #define DLLEXPORT __declspec(dllexport)
#else
    #define DLLEXPORT __attribute__((visibility("default")))
#endif

/* ==================== Handles ==================== */
typedef struct { ma_engine* engine; } EngineHandle;
typedef struct { ma_sound* sound; int owned; } SoundHandle;
typedef struct { ma_decoder* decoder; } DecoderHandle;
typedef struct { ma_waveform* wf; } WaveformHandle;
typedef struct { ma_noise* noise; void* heap; } NoiseHandle;
typedef struct { ma_lpf* lpf; void* heap; } LPFHandle;
typedef struct { ma_hpf* hpf; void* heap; } HPFHandle;

/* ==================== UTF-8 path support (Windows) ==================== */
#ifdef _WIN32
/* Convert UTF-8 string to wide-char string (for _w file APIs) */
static wchar_t* utf8_to_wchar(const char* utf8) {
    if (!utf8) return NULL;
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0) return NULL;
    wchar_t* wstr = (wchar_t*)malloc((size_t)len * sizeof(wchar_t));
    if (!wstr) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wstr, len);
    return wstr;
}
#endif

/* ==================== Version ==================== */
DLLEXPORT unsigned int ma_dll_get_version(void) {
    return (MA_VERSION_MAJOR << 16) | (MA_VERSION_MINOR << 8) | MA_VERSION_REVISION;
}

DLLEXPORT int ma_dll_has_vorbis(void) {
#ifdef MA_HAS_VORBIS
    return 1;
#else
    return 0;
#endif
}

/* ==================== Engine (singleton + ref-count) ==================== */
static ma_engine g_engine;
static int g_engine_ref = 0;

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_engine_ref > 0) {
            ma_engine_stop(&g_engine);
            ma_engine_uninit(&g_engine);
            g_engine_ref = 0;
        }
    }
    return TRUE;
}
#endif

DLLEXPORT int ma_dll_engine_init(void) {
    if (g_engine_ref > 0) { g_engine_ref++; return 0; }
    if (ma_engine_init(NULL, &g_engine) != MA_SUCCESS) return -1;
    g_engine_ref = 1;
    return 0;
}

DLLEXPORT void ma_dll_engine_uninit(void) {
    if (g_engine_ref <= 0) return;
    g_engine_ref--;
    if (g_engine_ref == 0) {
        ma_engine_stop(&g_engine);
        ma_engine_uninit(&g_engine);
    }
}

DLLEXPORT unsigned int ma_dll_engine_get_sample_rate(void) { return g_engine_ref ? ma_engine_get_sample_rate(&g_engine) : 0; }
DLLEXPORT int ma_dll_engine_get_channels(void) { return g_engine_ref ? ma_engine_get_channels(&g_engine) : 0; }
DLLEXPORT void ma_dll_engine_set_volume(double v) { if (g_engine_ref) ma_engine_set_volume(&g_engine, (float)v); }
DLLEXPORT double ma_dll_engine_get_volume(void) { return g_engine_ref ? (double)ma_engine_get_volume(&g_engine) : 0.0; }

/* Simple fire-and-forget playback (compat) */
DLLEXPORT int ma_dll_engine_play_sound(const char* path) {
    if (!g_engine_ref || !path || !path[0]) return -1;
#ifdef _WIN32
    /* Use wide-char path for UTF-8 support; create+start a temporary sound */
    wchar_t* wpath = utf8_to_wchar(path);
    if (wpath) {
        ma_sound* s = (ma_sound*)malloc(sizeof(ma_sound));
        if (s) {
            if (ma_sound_init_from_file_w(&g_engine, wpath, MA_SOUND_FLAG_STREAM, NULL, NULL, s) == MA_SUCCESS) {
                ma_sound_start(s);
                /* Note: sound is leaked for fire-and-forget; use Sound::createSound for managed playback */
            } else {
                free(s);
            }
        }
        free(wpath);
        return 0;
    }
#endif
    return ma_engine_play_sound(&g_engine, path, NULL) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT int ma_dll_engine_stop(void) {
    if (!g_engine_ref) return -1;
    return ma_engine_stop(&g_engine) == MA_SUCCESS ? 0 : -1;
}

/* ==================== Sound (streaming, multi-instance) ==================== */
DLLEXPORT SoundHandle* ma_dll_sound_create(const char* path) {
    if (!g_engine_ref || !path || !path[0]) return NULL;
    SoundHandle* h = (SoundHandle*)malloc(sizeof(SoundHandle));
    if (!h) return NULL;
    h->sound = (ma_sound*)malloc(sizeof(ma_sound));
    if (!h->sound) { free(h); return NULL; }
    h->owned = 0;
    /* STREAM: reads from file in chunks, minimal memory footprint */
#ifdef _WIN32
    /* Use wide-char path for UTF-8 support (fixes Chinese/non-ASCII paths) */
    wchar_t* wpath = utf8_to_wchar(path);
    if (wpath) {
        if (ma_sound_init_from_file_w(&g_engine, wpath, MA_SOUND_FLAG_STREAM, NULL, NULL, h->sound) == MA_SUCCESS) {
            h->owned = 1;
        }
        free(wpath);
    } else {
        /* Fallback to char version if conversion fails */
        if (ma_sound_init_from_file(&g_engine, path, MA_SOUND_FLAG_STREAM, NULL, NULL, h->sound) == MA_SUCCESS) {
            h->owned = 1;
        }
    }
#else
    if (ma_sound_init_from_file(&g_engine, path, MA_SOUND_FLAG_STREAM, NULL, NULL, h->sound) == MA_SUCCESS) {
        h->owned = 1;
    }
#endif
    if (!h->owned) {
        free(h->sound); free(h); return NULL;
    }
    return h;
}

DLLEXPORT void ma_dll_sound_destroy(SoundHandle* h) {
    if (!h) return;
    if (h->owned && h->sound) {
        if (g_engine_ref > 0) {
            ma_sound_stop(h->sound);
            ma_sound_uninit(h->sound);
        }
        free(h->sound);
    }
    free(h);
}

DLLEXPORT int ma_dll_sound_start(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return -1;
    return ma_sound_start(h->sound) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT int ma_dll_sound_stop(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return -1;
    return ma_sound_stop(h->sound) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT int ma_dll_sound_is_playing(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return 0;
    return ma_sound_is_playing(h->sound) ? 1 : 0;
}

DLLEXPORT void ma_dll_sound_set_volume(SoundHandle* h, double v) {
    if (h && h->sound && g_engine_ref > 0) ma_sound_set_volume(h->sound, (float)v);
}

DLLEXPORT double ma_dll_sound_get_volume(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return 0.0;
    return (double)ma_sound_get_volume(h->sound);
}

DLLEXPORT int ma_dll_sound_seek(SoundHandle* h, double sec) {
    if (!h || !h->sound || g_engine_ref <= 0) return -1;
    /* Use actual engine sample rate, not hardcoded 48000 */
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    if (sr == 0) sr = 48000;
    return ma_sound_seek_to_pcm_frame(h->sound, (ma_uint64)(sec * sr)) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT float ma_dll_sound_get_position(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return 0.0f;
    float cursor;
    return ma_sound_get_cursor_in_seconds(h->sound, &cursor) == MA_SUCCESS ? cursor : 0.0f;
}

DLLEXPORT float ma_dll_sound_get_duration(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return 0.0f;
    float len;
    return ma_sound_get_length_in_seconds(h->sound, &len) == MA_SUCCESS ? len : 0.0f;
}

/* --- New: looping, pitch, pan --- */
DLLEXPORT void ma_dll_sound_set_looping(SoundHandle* h, int looping) {
    if (h && h->sound && g_engine_ref > 0) ma_sound_set_looping(h->sound, looping ? MA_TRUE : MA_FALSE);
}

DLLEXPORT int ma_dll_sound_is_looping(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return 0;
    return ma_sound_is_looping(h->sound) ? 1 : 0;
}

DLLEXPORT void ma_dll_sound_set_pitch(SoundHandle* h, double pitch) {
    if (h && h->sound && g_engine_ref > 0) ma_sound_set_pitch(h->sound, (float)pitch);
}

DLLEXPORT double ma_dll_sound_get_pitch(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return 0.0;
    return (double)ma_sound_get_pitch(h->sound);
}

DLLEXPORT void ma_dll_sound_set_pan(SoundHandle* h, double pan) {
    if (h && h->sound && g_engine_ref > 0) ma_sound_set_pan(h->sound, (float)pan);
}

DLLEXPORT double ma_dll_sound_get_pan(SoundHandle* h) {
    if (!h || !h->sound || g_engine_ref <= 0) return 0.0;
    return (double)ma_sound_get_pan(h->sound);
}

/* ==================== Decoder ==================== */
DLLEXPORT ma_decoder* ma_dll_decoder_open(const char* path) {
    if (!path || !path[0]) return NULL;
    ma_decoder* d = (ma_decoder*)malloc(sizeof(ma_decoder));
    if (!d) return NULL;
#ifdef _WIN32
    /* Use wide-char path for UTF-8 support (fixes Chinese/non-ASCII paths) */
    wchar_t* wpath = utf8_to_wchar(path);
    if (wpath) {
        if (ma_decoder_init_file_w(wpath, NULL, d) != MA_SUCCESS) {
            free(wpath); free(d); return NULL;
        }
        free(wpath);
    } else {
        if (ma_decoder_init_file(path, NULL, d) != MA_SUCCESS) { free(d); return NULL; }
    }
#else
    if (ma_decoder_init_file(path, NULL, d) != MA_SUCCESS) { free(d); return NULL; }
#endif
    return d;
}

DLLEXPORT void ma_dll_decoder_close(ma_decoder* d) {
    if (d) { ma_decoder_uninit(d); free(d); }
}

DLLEXPORT unsigned long long ma_dll_decoder_read_pcm_frames(ma_decoder* d, void* out, unsigned long long n) {
    if (!d) return 0;
    ma_uint64 r = 0;
    ma_decoder_read_pcm_frames(d, out, n, &r);
    return r;
}

DLLEXPORT unsigned int ma_dll_decoder_get_sample_rate(ma_decoder* d) { return d ? d->outputSampleRate : 0; }
DLLEXPORT int ma_dll_decoder_get_channels(ma_decoder* d) { return d ? d->outputChannels : 0; }
DLLEXPORT int ma_dll_decoder_get_format(ma_decoder* d) { return d ? (int)d->outputFormat : -1; }

DLLEXPORT int ma_dll_decoder_seek_to_pcm_frame(ma_decoder* d, unsigned long long idx) {
    if (!d) return -1;
    return ma_decoder_seek_to_pcm_frame(d, idx) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT unsigned long long ma_dll_decoder_get_length_in_pcm_frames(ma_decoder* d) {
    if (!d) return 0;
    ma_uint64 len;
    return ma_decoder_get_length_in_pcm_frames(d, &len) == MA_SUCCESS ? len : 0;
}

/* ==================== Waveform ==================== */
DLLEXPORT WaveformHandle* ma_dll_waveform_create(int fmt, int ch, int sr, int type, double amp, double freq) {
    ma_waveform_type t;
    switch (type) { case 0:t=ma_waveform_type_sine;break; case 1:t=ma_waveform_type_square;break; case 2:t=ma_waveform_type_triangle;break; case 3:t=ma_waveform_type_sawtooth;break; default:t=ma_waveform_type_sine; }
    ma_format f;
    switch (fmt) { case 0:f=ma_format_f32;break; case 1:f=ma_format_s16;break; case 2:f=ma_format_s24;break; case 3:f=ma_format_s32;break; case 4:f=ma_format_u8;break; default:f=ma_format_f32; }
    ma_waveform_config cfg = ma_waveform_config_init(f, ch, sr, t, amp, freq);
    WaveformHandle* h = (WaveformHandle*)malloc(sizeof(WaveformHandle));
    if (!h) return NULL;
    h->wf = (ma_waveform*)malloc(sizeof(ma_waveform));
    if (!h->wf) { free(h); return NULL; }
    if (ma_waveform_init(&cfg, h->wf) != MA_SUCCESS) { free(h->wf); free(h); return NULL; }
    return h;
}

DLLEXPORT void ma_dll_waveform_destroy(WaveformHandle* h) {
    if (h) { if (h->wf) { ma_waveform_uninit(h->wf); free(h->wf); } free(h); }
}

DLLEXPORT unsigned long long ma_dll_waveform_read(WaveformHandle* h, void* out, unsigned long long n) {
    if (!h || !h->wf) return 0;
    ma_uint64 r = 0;
    ma_waveform_read_pcm_frames(h->wf, out, n, &r);
    return r;
}

DLLEXPORT int ma_dll_waveform_set_amplitude(WaveformHandle* h, double v) { return (h&&h->wf) ? (ma_waveform_set_amplitude(h->wf,v)==MA_SUCCESS?0:-1) : -1; }
DLLEXPORT int ma_dll_waveform_set_frequency(WaveformHandle* h, double v) { return (h&&h->wf) ? (ma_waveform_set_frequency(h->wf,v)==MA_SUCCESS?0:-1) : -1; }

DLLEXPORT int ma_dll_waveform_set_type(WaveformHandle* h, int type) {
    if (!h || !h->wf) return -1;
    ma_waveform_type t;
    switch (type) { case 0:t=ma_waveform_type_sine;break; case 1:t=ma_waveform_type_square;break; case 2:t=ma_waveform_type_triangle;break; case 3:t=ma_waveform_type_sawtooth;break; default:t=ma_waveform_type_sine; }
    return ma_waveform_set_type(h->wf, t) == MA_SUCCESS ? 0 : -1;
}

/* ==================== Noise ==================== */
DLLEXPORT NoiseHandle* ma_dll_noise_create(int fmt, int ch, int type, int seed, double amp) {
    ma_noise_type t;
    switch (type) { case 0:t=ma_noise_type_white;break; case 1:t=ma_noise_type_pink;break; case 2:t=ma_noise_type_brownian;break; default:t=ma_noise_type_white; }
    ma_format f;
    switch (fmt) { case 0:f=ma_format_f32;break; case 1:f=ma_format_s16;break; case 2:f=ma_format_s24;break; case 3:f=ma_format_s32;break; case 4:f=ma_format_u8;break; default:f=ma_format_f32; }
    ma_noise_config cfg = ma_noise_config_init(f, ch, t, seed, amp);
    NoiseHandle* h = (NoiseHandle*)malloc(sizeof(NoiseHandle));
    if (!h) return NULL;
    h->noise = (ma_noise*)malloc(sizeof(ma_noise));
    if (!h->noise) { free(h); return NULL; }
    size_t hs = 0;
    if (ma_noise_get_heap_size(&cfg, &hs) != MA_SUCCESS) { free(h->noise); free(h); return NULL; }
    h->heap = malloc(hs);
    if (!h->heap) { free(h->noise); free(h); return NULL; }
    if (ma_noise_init_preallocated(&cfg, h->heap, h->noise) != MA_SUCCESS) { free(h->heap); free(h->noise); free(h); return NULL; }
    return h;
}

DLLEXPORT void ma_dll_noise_destroy(NoiseHandle* h) {
    if (h) { if (h->noise) { ma_noise_uninit(h->noise, NULL); free(h->noise); } if (h->heap) free(h->heap); free(h); }
}

DLLEXPORT unsigned long long ma_dll_noise_read(NoiseHandle* h, void* out, unsigned long long n) {
    if (!h || !h->noise) return 0;
    ma_uint64 r = 0;
    ma_noise_read_pcm_frames(h->noise, out, n, &r);
    return r;
}

DLLEXPORT int ma_dll_noise_set_amplitude(NoiseHandle* h, double v) { return (h&&h->noise) ? (ma_noise_set_amplitude(h->noise,v)==MA_SUCCESS?0:-1) : -1; }
DLLEXPORT int ma_dll_noise_set_seed(NoiseHandle* h, int s) { return (h&&h->noise) ? (ma_noise_set_seed(h->noise,s)==MA_SUCCESS?0:-1) : -1; }

/* ==================== LPF / HPF ==================== */
static ma_format to_fmt(int f) {
    switch (f) { case 0:return ma_format_f32; case 1:return ma_format_s16; case 2:return ma_format_s24; case 3:return ma_format_s32; case 4:return ma_format_u8; default:return ma_format_f32; }
}

DLLEXPORT LPFHandle* ma_dll_lpf_create(int fmt, int ch, int sr, int order, float cutoff) {
    ma_format f = to_fmt(fmt);
    ma_lpf_config cfg = ma_lpf_config_init(f, ch, sr, cutoff, order);
    LPFHandle* h = (LPFHandle*)malloc(sizeof(LPFHandle));
    if (!h) return NULL;
    h->lpf = (ma_lpf*)malloc(sizeof(ma_lpf));
    if (!h->lpf) { free(h); return NULL; }
    size_t hs = 0;
    if (ma_lpf_get_heap_size(&cfg, &hs) != MA_SUCCESS) { free(h->lpf); free(h); return NULL; }
    h->heap = malloc(hs);
    if (!h->heap) { free(h->lpf); free(h); return NULL; }
    if (ma_lpf_init_preallocated(&cfg, h->heap, h->lpf) != MA_SUCCESS) { free(h->heap); free(h->lpf); free(h); return NULL; }
    return h;
}

DLLEXPORT void ma_dll_lpf_destroy(LPFHandle* h) {
    if (h) { if (h->lpf) { ma_lpf_uninit(h->lpf, NULL); free(h->lpf); } if (h->heap) free(h->heap); free(h); }
}

DLLEXPORT unsigned long long ma_dll_lpf_process(LPFHandle* h, void* out, const void* in, unsigned long long n) {
    if (!h || !h->lpf) return 0;
    ma_lpf_process_pcm_frames(h->lpf, out, in, n);
    return n;
}

DLLEXPORT int ma_dll_lpf_reinit(LPFHandle* h, int fmt, int ch, int sr, int order, float cutoff) {
    if (!h || !h->lpf) return -1;
    ma_lpf_config cfg = ma_lpf_config_init(to_fmt(fmt), ch, sr, cutoff, order);
    return ma_lpf_reinit(&cfg, h->lpf) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT HPFHandle* ma_dll_hpf_create(int fmt, int ch, int sr, int order, float cutoff) {
    ma_format f = to_fmt(fmt);
    ma_hpf_config cfg = ma_hpf_config_init(f, ch, sr, cutoff, order);
    HPFHandle* h = (HPFHandle*)malloc(sizeof(HPFHandle));
    if (!h) return NULL;
    h->hpf = (ma_hpf*)malloc(sizeof(ma_hpf));
    if (!h->hpf) { free(h); return NULL; }
    size_t hs = 0;
    if (ma_hpf_get_heap_size(&cfg, &hs) != MA_SUCCESS) { free(h->hpf); free(h); return NULL; }
    h->heap = malloc(hs);
    if (!h->heap) { free(h->hpf); free(h); return NULL; }
    if (ma_hpf_init_preallocated(&cfg, h->heap, h->hpf) != MA_SUCCESS) { free(h->heap); free(h->hpf); free(h); return NULL; }
    return h;
}

DLLEXPORT void ma_dll_hpf_destroy(HPFHandle* h) {
    if (h) { if (h->hpf) { ma_hpf_uninit(h->hpf, NULL); free(h->hpf); } if (h->heap) free(h->heap); free(h); }
}

DLLEXPORT unsigned long long ma_dll_hpf_process(HPFHandle* h, void* out, const void* in, unsigned long long n) {
    if (!h || !h->hpf) return 0;
    ma_hpf_process_pcm_frames(h->hpf, out, in, n);
    return n;
}

DLLEXPORT int ma_dll_hpf_reinit(HPFHandle* h, int fmt, int ch, int sr, int order, float cutoff) {
    if (!h || !h->hpf) return -1;
    ma_hpf_config cfg = ma_hpf_config_init(to_fmt(fmt), ch, sr, cutoff, order);
    return ma_hpf_reinit(&cfg, h->hpf) == MA_SUCCESS ? 0 : -1;
}
