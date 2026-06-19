/*
 * miniaudio DLL 包装器 v2 — heap-allocated handles for multi-instance support
 * 编译: gcc -shared -o miniaudio.dll miniaudio_dll.c -O2 -lwinmm -lole32 -lksuser
 */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <math.h>

#ifdef _WIN32
    #define DLLEXPORT __declspec(dllexport)
#else
    #define DLLEXPORT __attribute__((visibility("default")))
#endif

/* ==================== 句柄结构 (每个实例独立) ==================== */
typedef struct { ma_engine* engine; } EngineHandle;
typedef struct { ma_sound* sound; int owned; } SoundHandle;
typedef struct { ma_decoder* decoder; } DecoderHandle;
typedef struct { ma_waveform* wf; } WaveformHandle;
typedef struct { ma_noise* noise; void* heap; } NoiseHandle;
typedef struct { ma_lpf* lpf; void* heap; } LPFHandle;
typedef struct { ma_hpf* hpf; void* heap; } HPFHandle;

/* ==================== Version ==================== */
DLLEXPORT unsigned int ma_dll_get_version(void) {
    return (MA_VERSION_MAJOR << 16) | (MA_VERSION_MINOR << 8) | MA_VERSION_REVISION;
}

/* ==================== Engine (单例 + 引用计数 — 多 Sound 共享) ==================== */
static ma_engine g_engine;
static int g_engine_ref = 0;

DLLEXPORT int ma_dll_engine_init(void) {
    if (g_engine_ref > 0) { g_engine_ref++; return 0; }
    if (ma_engine_init(NULL, &g_engine) != MA_SUCCESS) return -1;
    g_engine_ref = 1;
    return 0;
}

DLLEXPORT void ma_dll_engine_uninit(void) {
    if (g_engine_ref <= 0) return;
    g_engine_ref--;
    if (g_engine_ref == 0) { ma_engine_uninit(&g_engine); }
}

DLLEXPORT unsigned int ma_dll_engine_get_sample_rate(void) { return g_engine_ref ? ma_engine_get_sample_rate(&g_engine) : 0; }
DLLEXPORT int ma_dll_engine_get_channels(void) { return g_engine_ref ? ma_engine_get_channels(&g_engine) : 0; }

DLLEXPORT void ma_dll_engine_set_volume(double v) { if (g_engine_ref) ma_engine_set_volume(&g_engine, (float)v); }
DLLEXPORT double ma_dll_engine_get_volume(void) { return g_engine_ref ? (double)ma_engine_get_volume(&g_engine) : 0.0; }

/* 简单的播放(不返回句柄) — 兼容旧API */
DLLEXPORT int ma_dll_engine_play_sound(const char* path) {
    if (!g_engine_ref) return -1;
    return ma_engine_play_sound(&g_engine, path, NULL) == MA_SUCCESS ? 0 : -1;
}
DLLEXPORT int ma_dll_engine_stop(void) {
    if (!g_engine_ref) return -1;
    return ma_engine_stop(&g_engine) == MA_SUCCESS ? 0 : -1;
}

/* ==================== Sound (heap-allocated — 支持多实例) ==================== */
DLLEXPORT SoundHandle* ma_dll_sound_create(const char* path) {
    if (!g_engine_ref || !path) return NULL;
    SoundHandle* h = (SoundHandle*)malloc(sizeof(SoundHandle));
    if (!h) return NULL;
    h->sound = (ma_sound*)malloc(sizeof(ma_sound));
    if (!h->sound) { free(h); return NULL; }
    if (ma_sound_init_from_file(&g_engine, path, 0, NULL, NULL, h->sound) != MA_SUCCESS) {
        free(h->sound); free(h); return NULL;
    }
    h->owned = 1;
    return h;
}

DLLEXPORT void ma_dll_sound_destroy(SoundHandle* h) {
    if (!h) return;
    if (h->owned && h->sound) { ma_sound_uninit(h->sound); free(h->sound); }
    free(h);
}

DLLEXPORT int ma_dll_sound_start(SoundHandle* h) {
    if (!h || !h->sound) return -1;
    return ma_sound_start(h->sound) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT int ma_dll_sound_stop(SoundHandle* h) {
    if (!h || !h->sound) return -1;
    return ma_sound_stop(h->sound) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT int ma_dll_sound_is_playing(SoundHandle* h) {
    if (!h || !h->sound) return 0;
    return ma_sound_is_playing(h->sound) ? 1 : 0;
}

DLLEXPORT void ma_dll_sound_set_volume(SoundHandle* h, float v) {
    if (h && h->sound) ma_sound_set_volume(h->sound, v);
}

DLLEXPORT float ma_dll_sound_get_volume(SoundHandle* h) {
    if (!h || !h->sound) return 0.0f;
    return ma_sound_get_volume(h->sound);
}

DLLEXPORT int ma_dll_sound_seek(SoundHandle* h, double sec) {
    if (!h || !h->sound) return -1;
    return ma_sound_seek_to_pcm_frame(h->sound, (ma_uint64)(sec * 48000)) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT float ma_dll_sound_get_position(SoundHandle* h) {
    if (!h || !h->sound) return 0.0f;
    float cursor;
    return ma_sound_get_cursor_in_seconds(h->sound, &cursor) == MA_SUCCESS ? cursor : 0.0f;
}

DLLEXPORT float ma_dll_sound_get_duration(SoundHandle* h) {
    if (!h || !h->sound) return 0.0f;
    float len;
    return ma_sound_get_length_in_seconds(h->sound, &len) == MA_SUCCESS ? len : 0.0f;
}

/* ==================== Decoder (已是 heap，保持兼容) ==================== */
DLLEXPORT ma_decoder* ma_dll_decoder_open(const char* path) {
    ma_decoder* d = (ma_decoder*)malloc(sizeof(ma_decoder));
    if (!d) return NULL;
    if (ma_decoder_init_file(path, NULL, d) != MA_SUCCESS) { free(d); return NULL; }
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

/* ==================== Waveform (heap-allocated) ==================== */
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

/* ==================== Noise (heap-allocated) ==================== */
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

/* ==================== LPF / HPF (heap-allocated) ==================== */
static ma_format to_fmt(int f) {
    switch (f) { case 0:return ma_format_f32; case 1:return ma_format_s16; case 2:return ma_format_s24; case 3:return ma_format_s32; case 4:return ma_format_u8; default:return ma_format_f32; }
}

DLLEXPORT LPFHandle* ma_dll_lpf_create(int fmt, int ch, int sr, float cutoff, int order) {
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

DLLEXPORT int ma_dll_lpf_reinit(LPFHandle* h, int fmt, int ch, int sr, float cutoff, int order) {
    if (!h || !h->lpf) return -1;
    ma_lpf_config cfg = ma_lpf_config_init(to_fmt(fmt), ch, sr, cutoff, order);
    return ma_lpf_reinit(&cfg, h->lpf) == MA_SUCCESS ? 0 : -1;
}

DLLEXPORT HPFHandle* ma_dll_hpf_create(int fmt, int ch, int sr, float cutoff, int order) {
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

DLLEXPORT int ma_dll_hpf_reinit(HPFHandle* h, int fmt, int ch, int sr, float cutoff, int order) {
    if (!h || !h->hpf) return -1;
    ma_hpf_config cfg = ma_hpf_config_init(to_fmt(fmt), ch, sr, cutoff, order);
    return ma_hpf_reinit(&cfg, h->hpf) == MA_SUCCESS ? 0 : -1;
}

/* ==================== 数学工具 ==================== */
DLLEXPORT double ma_dll_sin(double x) { return sin(x); }
DLLEXPORT double ma_dll_cos(double x) { return cos(x); }
DLLEXPORT double ma_dll_sqrt(double x) { return sqrt(x); }
DLLEXPORT double ma_dll_pow(double b, double e) { return pow(b, e); }

/* ==================== 调试辅助 ==================== */
DLLEXPORT void ma_dll_generate_sine_f32(float* buf, unsigned int n, int ch, float freq, float amp, double* phase) {
    double inc = freq / 48000.0 * 2.0 * 3.14159265359;
    for (unsigned int i = 0; i < n; i++) {
        float s = (float)(sin(*phase) * amp);
        for (int c = 0; c < ch; c++) buf[i * ch + c] = s;
        *phase += inc;
        if (*phase > 2.0 * 3.14159265359) *phase -= 2.0 * 3.14159265359;
    }
}
