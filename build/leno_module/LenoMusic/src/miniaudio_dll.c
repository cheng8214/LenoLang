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

/* ==================== Spectrum (FFT 频谱) ==================== */
/*
 * 为什么要另开一个解码器（旁路解码），而不是从 ma_sound 里取 PCM：
 *   1) 本 DLL 的 Sound 走 MA_SOUND_FLAG_STREAM，PCM 是在引擎的音频线程里被
 *      解码并直接送去混音的，外面拿不到（miniaudio 没有 "tap / 监视" 钩子）。
 *   2) 想挂钩就得把 Sound 改成 ma_sound_init_from_data_source + 自写 data
 *      source 包装器，会动到全部播放路径（现网所有应用都靠它），风险不划算。
 *   3) 频谱只是视觉反馈：分析窗口取【播放位置前 fftSize 帧】就够，几十毫秒的
 *      相位差肉眼看不出。所以这里自带一个 ma_decoder，按播放位置追着解码，
 *      漂移超过 1/4 秒才 seek 一次（MP3 seek 要扫帧，别每帧都 seek）。
 * 成本：稳定播放时每次 update 只解码 ~一帧音频（16~33ms 的样本），可忽略。
 * 本文件里没有 ma_fft（这份 miniaudio.h 是裁剪过的 0.11，不含 FFT），
 *   所以下面自写一个 radix-2 迭代 FFT（输入长度恒为 2 的幂）。
 */
typedef struct {
    ma_decoder decoder;
    ma_uint32  sampleRate;
    ma_uint32  channels;
    ma_uint64  cursor;      /* 已解码到的绝对帧号（用于跟 pos 对齐） */
    int        fftSize;
    float*     ring;        /* fftSize 个单声道样本（环形，最近的一个窗口） */
    int        ringPos;     /* 下一个写入位置 = 当前最旧样本位置 */
    float*     re;
    float*     im;
    float*     win;         /* Hann 窗 */
    float*     pcm;         /* 解码暂存：4096 帧 × channels */
} SpectrumHandle;

#define MA_SPECTRUM_CHUNK 4096

/* 原地基-2 FFT（re/im 长度必须是 2 的幂） */
static void spectrum_fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -6.283185307179586 / (double)len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < half; k++) {
                int a = i + k, b = a + half;
                float vr = re[b] * cr - im[b] * ci;
                float vi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] += vr;        im[a] += vi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

DLLEXPORT SpectrumHandle* ma_dll_spectrum_create(const char* path, int fft_size) {
    if (!path || !path[0]) return NULL;
    if (fft_size < 256) fft_size = 256;
    if (fft_size > 16384) fft_size = 16384;
    int n = 1;
    while (n < fft_size) n <<= 1;         /* 归一到 2 的幂 */
    fft_size = n;

    SpectrumHandle* h = (SpectrumHandle*)calloc(1, sizeof(SpectrumHandle));
    if (!h) return NULL;

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
#ifdef _WIN32
    ma_result r;
    wchar_t* wpath = utf8_to_wchar(path);
    if (wpath) {
        r = ma_decoder_init_file_w(wpath, &cfg, &h->decoder);
        free(wpath);
    } else {
        r = ma_decoder_init_file(path, &cfg, &h->decoder);
    }
#else
    ma_result r = ma_decoder_init_file(path, &cfg, &h->decoder);
#endif
    if (r != MA_SUCCESS) { free(h); return NULL; }

    h->sampleRate = h->decoder.outputSampleRate ? h->decoder.outputSampleRate : 48000;
    h->channels   = h->decoder.outputChannels ? h->decoder.outputChannels : 1;
    h->fftSize    = fft_size;
    h->cursor     = 0;
    h->ringPos    = 0;

    h->ring = (float*)calloc((size_t)fft_size, sizeof(float));
    h->re   = (float*)calloc((size_t)fft_size, sizeof(float));
    h->im   = (float*)calloc((size_t)fft_size, sizeof(float));
    h->win  = (float*)calloc((size_t)fft_size, sizeof(float));
    h->pcm  = (float*)calloc((size_t)MA_SPECTRUM_CHUNK * h->channels, sizeof(float));
    if (!h->ring || !h->re || !h->im || !h->win || !h->pcm) {
        if (h->ring) free(h->ring);
        if (h->re)   free(h->re);
        if (h->im)   free(h->im);
        if (h->win)  free(h->win);
        if (h->pcm)  free(h->pcm);
        ma_decoder_uninit(&h->decoder);
        free(h);
        return NULL;
    }
    for (int i = 0; i < fft_size; i++) {
        h->win[i] = (float)(0.5 - 0.5 * cos(6.283185307179586 * (double)i / (double)(fft_size - 1)));
    }
    return h;
}

DLLEXPORT void ma_dll_spectrum_destroy(SpectrumHandle* h) {
    if (!h) return;
    ma_decoder_uninit(&h->decoder);
    free(h->ring); free(h->re); free(h->im); free(h->win); free(h->pcm);
    free(h);
}

DLLEXPORT int ma_dll_spectrum_get_sample_rate(SpectrumHandle* h) { return h ? (int)h->sampleRate : 0; }
DLLEXPORT int ma_dll_spectrum_get_channels(SpectrumHandle* h) { return h ? (int)h->channels : 0; }
DLLEXPORT int ma_dll_spectrum_get_fft_size(SpectrumHandle* h) { return h ? h->fftSize : 0; }

/* 返回实际写入的频段数；out 每个元素是 0..1 的柱高 */
DLLEXPORT int ma_dll_spectrum_update(SpectrumHandle* h, double pos_sec, int bands, float* out, int out_cap) {
    if (!h || !out || bands <= 0 || out_cap <= 0) return 0;
    if (bands > out_cap) bands = out_cap;

    const ma_uint64 N  = (ma_uint64)h->fftSize;
    const ma_uint64 sr = (ma_uint64)h->sampleRate;
    if (pos_sec < 0.0) pos_sec = 0.0;
    ma_uint64 target = (ma_uint64)(pos_sec * (double)sr);
    /* 开头 N 帧以内"往前取不满一个窗口" ⇒ 用文件开头那一窗代替（图不会一开始就空着） */
    if (target < N) target = N;

    ma_uint64 tol = sr / 4;
    if (tol < N) tol = N;

    if (h->cursor > target || (target - h->cursor) > tol) {
        ma_uint64 start = target - N;
        if (ma_decoder_seek_to_pcm_frame(&h->decoder, start) == MA_SUCCESS) {
            h->cursor = start;
        } else {
            h->cursor = start;
        }
        /* seek 后旧样本已不属于这个位置 ⇒ 窗口清零重新攒 */
        memset(h->ring, 0, sizeof(float) * (size_t)N);
        h->ringPos = 0;
    }

    while (h->cursor < target) {
        ma_uint64 want = target - h->cursor;
        if (want > MA_SPECTRUM_CHUNK) want = MA_SPECTRUM_CHUNK;
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&h->decoder, h->pcm, want, &got);
        if (got == 0) break;                     /* 到文件尾 / 数据还没下完 */
        ma_uint32 ch = h->channels;
        for (ma_uint64 i = 0; i < got; i++) {
            float s = 0.0f;
            const float* f = h->pcm + i * ch;
            for (ma_uint32 c = 0; c < ch; c++) s += f[c];
            h->ring[h->ringPos] = s / (float)ch;  /* 混成单声道 */
            if (++h->ringPos >= (int)N) h->ringPos = 0;
        }
        h->cursor += got;
        if (got < want) break;
    }

    /* 把环形缓冲按时间顺序展开 + 加窗 */
    int start = h->ringPos;                      /* 最旧样本 */
    for (int i = 0; i < (int)N; i++) {
        int idx = start + i;
        if (idx >= (int)N) idx -= (int)N;
        h->re[i] = h->ring[idx] * h->win[i];
        h->im[i] = 0.0f;
    }
    spectrum_fft(h->re, h->im, (int)N);

    /* 对数频段聚合：40Hz ~ min(16kHz, nyq) */
    double nyq = (double)sr * 0.5;
    double f0 = 40.0;
    double f1 = nyq < 16000.0 ? nyq : 16000.0;
    if (f1 < f0 * 2.0) f1 = f0 * 2.0;
    double ratio = f1 / f0;
    int half = (int)N / 2;

    /* 第一遍：每段取平均幅度（先不做 dB）。bands 上限 128 ⇒ 栈上数组足够 */
    double seg[128];
    int nb = bands > 128 ? 128 : bands;
    for (int b = 0; b < nb; b++) {
        double lo = f0 * pow(ratio, (double)b / (double)nb);
        double hi = f0 * pow(ratio, (double)(b + 1) / (double)nb);
        int k0 = (int)(lo * (double)N / (double)sr);
        int k1 = (int)(hi * (double)N / (double)sr);
        if (k0 < 1) k0 = 1;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > half) k1 = half;
        double sum = 0.0;
        int cnt = 0;
        for (int k = k0; k < k1; k++) {
            double re = (double)h->re[k], im = (double)h->im[k];
            sum += sqrt(re * re + im * im) * 2.0 / (double)N;   /* 单边幅度谱 */
            cnt++;
        }
        seg[b] = cnt ? (sum / (double)cnt) : 0.0;
    }

    /* 第二遍：邻域平滑 → 倾斜补偿 → dB 归一化 */
    for (int b = 0; b < nb; b++) {
        /* 3 点平滑（0.2/0.6/0.2）：单根尖刺看着像"坏了一格"，平滑后才是曲线 ✓
           ⚠ 别用 0.25/0.5/0.25：抹得太狠 ⇒ 整排变成一条单调斜坡，没有起伏 ✗ */
        double mag = seg[b] * 3.0;
        if (b > 0)        mag += seg[b - 1];
        if (b + 1 < nb)   mag += seg[b + 1];
        mag /= 5.0;
        /* 频率倾斜（tilt）：音乐能量天然压在低频，原样画出来就是"左边一堵墙、右边一排小点" ✗
           ⇒ 以 500Hz 为轴每倍频程 +3dB（夹在 ±10dB）：低频压一点、高频抬一点。
           ⚠ 别补太多（试过 4dB/±14 + gamma 0.72）：各段被拉成**一样高** ⇒ 像一排等长方块，没有起伏 ✗ */
        double lo   = f0 * pow(ratio, (double)b / (double)nb);
        double hi   = f0 * pow(ratio, (double)(b + 1) / (double)nb);
        double fc   = sqrt(lo * hi);                          /* 该频段的中心频率 */
        double tilt = 3.0 * (log(fc / 500.0) / 0.6931471805599453);
        if (tilt > 10.0) tilt = 10.0;
        if (tilt < -10.0) tilt = -10.0;
        mag *= pow(10.0, tilt / 20.0);
        double db = 20.0 * log10(mag + 1e-9);
        double v  = (db + 66.0) / 66.0;      /* -66dB → 0，0dB → 1（比 -72 灵敏，柱子更有生气 ✓） */
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        /* 轻度提亮（gamma 0.85）：不改的话中低幅度的柱整体偏瘫 ✗；再压狠了就全顶格 ✗ */
        v = pow(v, 0.85);
        out[b] = (float)v;
    }
    return nb;
}
