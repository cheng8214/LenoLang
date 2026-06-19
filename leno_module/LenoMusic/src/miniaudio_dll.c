/*
 * miniaudio DLL 包装器
 * 用于导出函数供 Leno FFI 调用
 *
 * 编译命令 (MinGW):
 *   gcc -shared -o miniaudio.dll miniaudio_dll.c -O2 -lwinmm -lole32 -lksuser
 */

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#ifdef _WIN32
    #define DLLEXPORT __declspec(dllexport)
#else
    #define DLLEXPORT __attribute__((visibility("default")))
#endif

/* ==================== 全局设备实例 ==================== */
static ma_device g_device;
static ma_device_config g_config;
static int g_initialized = 0;

/* ==================== 回调相关 ==================== */
/* Leno 回调函数指针类型 */
typedef void (*leno_data_callback_t)(void* output, void* input, unsigned int frameCount, int channels);

static leno_data_callback_t g_leno_callback = NULL;

/* 内部数据回调，转发给 Leno */
static void internal_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    if (g_leno_callback != NULL) {
        g_leno_callback(pOutput, (void*)pInput, frameCount, pDevice->playback.channels);
    } else {
        /* 如果没有设置回调，输出静音 */
        if (pOutput) {
            ma_silence_pcm_frames(pOutput, frameCount, pDevice->playback.format, pDevice->playback.channels);
        }
    }
}

/* ==================== 导出函数 ==================== */

/* 获取版本信息 */
DLLEXPORT unsigned int ma_dll_get_version(void)
{
    return (MA_VERSION_MAJOR << 16) | (MA_VERSION_MINOR << 8) | MA_VERSION_REVISION;
}

/* 初始化播放设备 */
DLLEXPORT int ma_dll_init_device(int format, int channels, unsigned int sampleRate)
{
    if (g_initialized) {
        return -1; /* 已经初始化 */
    }

    g_config = ma_device_config_init(ma_device_type_playback);
    
    /* 设置格式 */
    switch (format) {
        case 0: g_config.playback.format = ma_format_f32; break;
        case 1: g_config.playback.format = ma_format_s16; break;
        case 2: g_config.playback.format = ma_format_s24; break;
        case 3: g_config.playback.format = ma_format_s32; break;
        case 4: g_config.playback.format = ma_format_u8; break;
        default: g_config.playback.format = ma_format_f32; break;
    }
    
    g_config.playback.channels = channels;
    g_config.sampleRate = sampleRate;
    g_config.dataCallback = internal_data_callback;

    if (ma_device_init(NULL, &g_config, &g_device) != MA_SUCCESS) {
        return -2; /* 初始化失败 */
    }

    g_initialized = 1;
    return 0;
}

/* 开始播放 */
DLLEXPORT int ma_dll_start(void)
{
    if (!g_initialized) return -1;
    return ma_device_start(&g_device) == MA_SUCCESS ? 0 : -1;
}

/* 停止播放 */
DLLEXPORT int ma_dll_stop(void)
{
    if (!g_initialized) return -1;
    return ma_device_stop(&g_device) == MA_SUCCESS ? 0 : -1;
}

/* 反初始化设备 */
DLLEXPORT void ma_dll_uninit(void)
{
    if (g_initialized) {
        ma_device_uninit(&g_device);
        g_initialized = 0;
        g_leno_callback = NULL;
    }
}

/* 设置数据回调 */
DLLEXPORT void ma_dll_set_callback(leno_data_callback_t callback)
{
    g_leno_callback = callback;
}

/* 获取当前采样率 */
DLLEXPORT unsigned int ma_dll_get_sample_rate(void)
{
    if (!g_initialized) return 0;
    return g_device.sampleRate;
}

/* 获取当前通道数 */
DLLEXPORT int ma_dll_get_channels(void)
{
    if (!g_initialized) return 0;
    return g_device.playback.channels;
}

/* ==================== 音频生成辅助函数 ==================== */

/* 生成正弦波到缓冲区 (f32格式) */
DLLEXPORT void ma_dll_generate_sine_f32(float* buffer, unsigned int frameCount, int channels, float frequency, float amplitude, double* phase)
{
    double phaseIncrement = frequency / 48000.0 * 2.0 * 3.14159265359;
    
    for (unsigned int i = 0; i < frameCount; i++) {
        float sample = (float)(sin(*phase) * amplitude);
        for (int c = 0; c < channels; c++) {
            buffer[i * channels + c] = sample;
        }
        *phase += phaseIncrement;
        if (*phase > 2.0 * 3.14159265359) {
            *phase -= 2.0 * 3.14159265359;
        }
    }
}

/* 生成方波到缓冲区 (f32格式) */
DLLEXPORT void ma_dll_generate_square_f32(float* buffer, unsigned int frameCount, int channels, float frequency, float amplitude, double* phase)
{
    double phaseIncrement = frequency / 48000.0 * 2.0 * 3.14159265359;
    
    for (unsigned int i = 0; i < frameCount; i++) {
        float sample = (float)((sin(*phase) > 0 ? 1.0 : -1.0) * amplitude);
        for (int c = 0; c < channels; c++) {
            buffer[i * channels + c] = sample;
        }
        *phase += phaseIncrement;
        if (*phase > 2.0 * 3.14159265359) {
            *phase -= 2.0 * 3.14159265359;
        }
    }
}

/* 清零缓冲区 */
DLLEXPORT void ma_dll_silence_f32(float* buffer, unsigned int frameCount, int channels)
{
    ma_silence_pcm_frames(buffer, frameCount, ma_format_f32, channels);
}

/* ==================== 解码器功能 ==================== */

/* 打开音频文件解码器 */
DLLEXPORT ma_decoder* ma_dll_decoder_open(const char* filepath)
{
    ma_decoder* decoder = (ma_decoder*)malloc(sizeof(ma_decoder));
    if (!decoder) return NULL;
    
    if (ma_decoder_init_file(filepath, NULL, decoder) != MA_SUCCESS) {
        free(decoder);
        return NULL;
    }
    
    return decoder;
}

/* 关闭解码器 */
DLLEXPORT void ma_dll_decoder_close(ma_decoder* decoder)
{
    if (decoder) {
        ma_decoder_uninit(decoder);
        free(decoder);
    }
}

/* 读取PCM帧 */
DLLEXPORT unsigned long long ma_dll_decoder_read_pcm_frames(ma_decoder* decoder, void* framesOut, unsigned long long frameCount)
{
    if (!decoder) return 0;
    ma_uint64 framesRead = 0;
    ma_decoder_read_pcm_frames(decoder, framesOut, frameCount, &framesRead);
    return framesRead;
}

/* 获取解码器信息 */
DLLEXPORT unsigned int ma_dll_decoder_get_sample_rate(ma_decoder* decoder)
{
    if (!decoder) return 0;
    return decoder->outputSampleRate;
}

DLLEXPORT int ma_dll_decoder_get_channels(ma_decoder* decoder)
{
    if (!decoder) return 0;
    return decoder->outputChannels;
}

DLLEXPORT int ma_dll_decoder_get_format(ma_decoder* decoder)
{
    if (!decoder) return -1;
    return (int)decoder->outputFormat;
}

/* 定位到指定帧 */
DLLEXPORT int ma_dll_decoder_seek_to_pcm_frame(ma_decoder* decoder, unsigned long long frameIndex)
{
    if (!decoder) return -1;
    return ma_decoder_seek_to_pcm_frame(decoder, frameIndex) == MA_SUCCESS ? 0 : -1;
}

/* 获取总帧数 */
DLLEXPORT unsigned long long ma_dll_decoder_get_length_in_pcm_frames(ma_decoder* decoder)
{
    if (!decoder) return 0;
    ma_uint64 length;
    if (ma_decoder_get_length_in_pcm_frames(decoder, &length) == MA_SUCCESS) {
        return length;
    }
    return 0;
}

/* ==================== 引擎功能 (高级API) ==================== */

static ma_engine g_engine;
static int g_engine_initialized = 0;

/* 初始化音频引擎 */
DLLEXPORT int ma_dll_engine_init(void)
{
    if (g_engine_initialized) return 0;
    
    if (ma_engine_init(NULL, &g_engine) != MA_SUCCESS) {
        return -1;
    }
    
    g_engine_initialized = 1;
    return 0;
}

/* 反初始化音频引擎 */
DLLEXPORT void ma_dll_engine_uninit(void)
{
    if (g_engine_initialized) {
        ma_engine_uninit(&g_engine);
        g_engine_initialized = 0;
    }
}

/* ==================== Sound 对象管理 (支持暂停/恢复) ==================== */

static ma_sound g_sound;
static int g_sound_initialized = 0;

/* 从文件创建 Sound 对象 */
DLLEXPORT int ma_dll_sound_init_from_file(const char* filepath)
{
    if (!g_engine_initialized) return -1;
    if (g_sound_initialized) {
        ma_sound_uninit(&g_sound);
        g_sound_initialized = 0;
    }
    
    if (ma_sound_init_from_file(&g_engine, filepath, 0, NULL, NULL, &g_sound) != MA_SUCCESS) {
        return -1;
    }
    
    g_sound_initialized = 1;
    return 0;
}

/* 反初始化 Sound */
DLLEXPORT void ma_dll_sound_uninit(void)
{
    if (g_sound_initialized) {
        ma_sound_uninit(&g_sound);
        g_sound_initialized = 0;
    }
}

/* 开始播放 Sound */
DLLEXPORT int ma_dll_sound_start(void)
{
    if (!g_sound_initialized) return -1;
    return ma_sound_start(&g_sound) == MA_SUCCESS ? 0 : -1;
}

/* 停止播放 Sound (可恢复) */
DLLEXPORT int ma_dll_sound_stop(void)
{
    if (!g_sound_initialized) return -1;
    return ma_sound_stop(&g_sound) == MA_SUCCESS ? 0 : -1;
}

/* 暂停 */
DLLEXPORT int ma_dll_sound_pause(void)
{
    if (!g_sound_initialized) return -1;
    /* miniaudio 没有直接的 pause，用 stop 实现 */
    return ma_sound_stop(&g_sound) == MA_SUCCESS ? 0 : -1;
}

/* 恢复 */
DLLEXPORT int ma_dll_sound_resume(void)
{
    if (!g_sound_initialized) return -1;
    return ma_sound_start(&g_sound) == MA_SUCCESS ? 0 : -1;
}

/* 是否正在播放 */
DLLEXPORT int ma_dll_sound_is_playing(void)
{
    if (!g_sound_initialized) return 0;
    return ma_sound_is_playing(&g_sound) ? 1 : 0;
}

/* 设置音量 (0.0 - 1.0) */
DLLEXPORT void ma_dll_sound_set_volume(float volume)
{
    if (!g_sound_initialized) return;
    ma_sound_set_volume(&g_sound, volume);
}

/* 获取音量 */
DLLEXPORT float ma_dll_sound_get_volume(void)
{
    if (!g_sound_initialized) return 0.0f;
    return ma_sound_get_volume(&g_sound);
}

/* 设置位置 (秒) */
DLLEXPORT int ma_dll_sound_seek_to_second(float second)
{
    if (!g_sound_initialized) return -1;
    return ma_sound_seek_to_pcm_frame(&g_sound, (ma_uint64)(second * 48000)) == MA_SUCCESS ? 0 : -1;
}

/* 获取当前位置 (秒) */
DLLEXPORT float ma_dll_sound_get_position(void)
{
    if (!g_sound_initialized) return 0.0f;
    float cursor;
    if (ma_sound_get_cursor_in_seconds(&g_sound, &cursor) != MA_SUCCESS) {
        return 0.0f;
    }
    return cursor;
}

/* 获取总时长 (秒) */
DLLEXPORT float ma_dll_sound_get_duration(void)
{
    if (!g_sound_initialized) return 0.0f;
    float length;
    if (ma_sound_get_length_in_seconds(&g_sound, &length) != MA_SUCCESS) {
        return 0.0f;
    }
    return length;
}

/* 简单的播放声音文件 (旧版API，不支持暂停) */
DLLEXPORT int ma_dll_engine_play_sound(const char* filepath)
{
    if (!g_engine_initialized) return -1;
    
    if (ma_engine_play_sound(&g_engine, filepath, NULL) != MA_SUCCESS) {
        return -1;
    }
    
    return 0;
}

/* 停止引擎播放 */
DLLEXPORT int ma_dll_engine_stop(void)
{
    if (!g_engine_initialized) return -1;
    return ma_engine_stop(&g_engine) == MA_SUCCESS ? 0 : -1;
}

/* 设置引擎主音量 (0.0 - 1.0) */
DLLEXPORT void ma_dll_engine_set_volume(double volume)
{
    if (!g_engine_initialized) return;
    ma_engine_set_volume(&g_engine, (float)volume);
}

/* 获取引擎主音量 */
DLLEXPORT double ma_dll_engine_get_volume(void)
{
    if (!g_engine_initialized) return 0.0;
    return (double)ma_engine_get_volume(&g_engine);
}

/* 获取引擎采样率 */
DLLEXPORT unsigned int ma_dll_engine_get_sample_rate(void)
{
    if (!g_engine_initialized) return 0;
    return ma_engine_get_sample_rate(&g_engine);
}

/* 获取引擎通道数 */
DLLEXPORT int ma_dll_engine_get_channels(void)
{
    if (!g_engine_initialized) return 0;
    return ma_engine_get_channels(&g_engine);
}

/* ==================== 数学工具 ==================== */

#include <math.h>

DLLEXPORT double ma_dll_sin(double x)
{
    return sin(x);
}

DLLEXPORT double ma_dll_cos(double x)
{
    return cos(x);
}

DLLEXPORT double ma_dll_sqrt(double x)
{
    return sqrt(x);
}

DLLEXPORT double ma_dll_pow(double base, double exp)
{
    return pow(base, exp);
}

/* ==================== 波形生成器 ==================== */

static ma_waveform g_waveform;
static int g_waveform_initialized = 0;

/* 初始化波形生成器
 * type: 0=sine, 1=square, 2=triangle, 3=sawtooth
 */
DLLEXPORT int ma_dll_waveform_init(int format, int channels, int sampleRate, int type, double amplitude, double frequency)
{
    ma_waveform_type waveformType;
    switch (type) {
        case 0: waveformType = ma_waveform_type_sine; break;
        case 1: waveformType = ma_waveform_type_square; break;
        case 2: waveformType = ma_waveform_type_triangle; break;
        case 3: waveformType = ma_waveform_type_sawtooth; break;
        default: waveformType = ma_waveform_type_sine; break;
    }

    ma_format fmt;
    switch (format) {
        case 0: fmt = ma_format_f32; break;
        case 1: fmt = ma_format_s16; break;
        case 2: fmt = ma_format_s24; break;
        case 3: fmt = ma_format_s32; break;
        case 4: fmt = ma_format_u8; break;
        default: fmt = ma_format_f32; break;
    }

    ma_waveform_config config = ma_waveform_config_init(fmt, channels, sampleRate, waveformType, amplitude, frequency);

    if (ma_waveform_init(&config, &g_waveform) != MA_SUCCESS) {
        return -1;
    }

    g_waveform_initialized = 1;
    return 0;
}

/* 反初始化波形生成器 */
DLLEXPORT void ma_dll_waveform_uninit(void)
{
    if (g_waveform_initialized) {
        ma_waveform_uninit(&g_waveform);
        g_waveform_initialized = 0;
    }
}

/* 读取波形 PCM 帧 */
DLLEXPORT unsigned long long ma_dll_waveform_read_pcm_frames(void* framesOut, unsigned long long frameCount)
{
    if (!g_waveform_initialized) return 0;
    ma_uint64 framesRead = 0;
    ma_waveform_read_pcm_frames(&g_waveform, framesOut, frameCount, &framesRead);
    return framesRead;
}

/* 设置波形振幅 */
DLLEXPORT int ma_dll_waveform_set_amplitude(double amplitude)
{
    if (!g_waveform_initialized) return -1;
    return ma_waveform_set_amplitude(&g_waveform, amplitude) == MA_SUCCESS ? 0 : -1;
}

/* 设置波形频率 */
DLLEXPORT int ma_dll_waveform_set_frequency(double frequency)
{
    if (!g_waveform_initialized) return -1;
    return ma_waveform_set_frequency(&g_waveform, frequency) == MA_SUCCESS ? 0 : -1;
}

/* 设置波形类型 */
DLLEXPORT int ma_dll_waveform_set_type(int type)
{
    if (!g_waveform_initialized) return -1;
    ma_waveform_type waveformType;
    switch (type) {
        case 0: waveformType = ma_waveform_type_sine; break;
        case 1: waveformType = ma_waveform_type_square; break;
        case 2: waveformType = ma_waveform_type_triangle; break;
        case 3: waveformType = ma_waveform_type_sawtooth; break;
        default: waveformType = ma_waveform_type_sine; break;
    }
    return ma_waveform_set_type(&g_waveform, waveformType) == MA_SUCCESS ? 0 : -1;
}

/* ==================== 噪声生成器 ==================== */

static ma_noise g_noise;
static void* g_noise_heap = NULL;
static int g_noise_initialized = 0;

/* 初始化噪声生成器
 * type: 0=white, 1=pink, 2=brownian
 */
DLLEXPORT int ma_dll_noise_init(int format, int channels, int type, int seed, double amplitude)
{
    if (g_noise_initialized) {
        ma_noise_uninit(&g_noise, NULL);
        if (g_noise_heap) {
            free(g_noise_heap);
            g_noise_heap = NULL;
        }
        g_noise_initialized = 0;
    }

    ma_noise_type noiseType;
    switch (type) {
        case 0: noiseType = ma_noise_type_white; break;
        case 1: noiseType = ma_noise_type_pink; break;
        case 2: noiseType = ma_noise_type_brownian; break;
        default: noiseType = ma_noise_type_white; break;
    }

    ma_format fmt;
    switch (format) {
        case 0: fmt = ma_format_f32; break;
        case 1: fmt = ma_format_s16; break;
        case 2: fmt = ma_format_s24; break;
        case 3: fmt = ma_format_s32; break;
        case 4: fmt = ma_format_u8; break;
        default: fmt = ma_format_f32; break;
    }

    ma_noise_config config = ma_noise_config_init(fmt, channels, noiseType, seed, amplitude);

    size_t heapSize = 0;
    if (ma_noise_get_heap_size(&config, &heapSize) != MA_SUCCESS) {
        return -1;
    }

    g_noise_heap = malloc(heapSize);
    if (!g_noise_heap) {
        return -2;
    }

    if (ma_noise_init_preallocated(&config, g_noise_heap, &g_noise) != MA_SUCCESS) {
        free(g_noise_heap);
        g_noise_heap = NULL;
        return -3;
    }

    g_noise_initialized = 1;
    return 0;
}

/* 反初始化噪声生成器 */
DLLEXPORT void ma_dll_noise_uninit(void)
{
    if (g_noise_initialized) {
        ma_noise_uninit(&g_noise, NULL);
        if (g_noise_heap) {
            free(g_noise_heap);
            g_noise_heap = NULL;
        }
        g_noise_initialized = 0;
    }
}

/* 读取噪声 PCM 帧 */
DLLEXPORT unsigned long long ma_dll_noise_read_pcm_frames(void* framesOut, unsigned long long frameCount)
{
    if (!g_noise_initialized) return 0;
    ma_uint64 framesRead = 0;
    ma_noise_read_pcm_frames(&g_noise, framesOut, frameCount, &framesRead);
    return framesRead;
}

/* 设置噪声振幅 */
DLLEXPORT int ma_dll_noise_set_amplitude(double amplitude)
{
    if (!g_noise_initialized) return -1;
    return ma_noise_set_amplitude(&g_noise, amplitude) == MA_SUCCESS ? 0 : -1;
}

/* 设置噪声种子 */
DLLEXPORT int ma_dll_noise_set_seed(int seed)
{
    if (!g_noise_initialized) return -1;
    return ma_noise_set_seed(&g_noise, seed) == MA_SUCCESS ? 0 : -1;
}

/* 设置噪声类型 */
DLLEXPORT int ma_dll_noise_set_type(int type)
{
    if (!g_noise_initialized) return -1;
    ma_noise_type noiseType;
    switch (type) {
        case 0: noiseType = ma_noise_type_white; break;
        case 1: noiseType = ma_noise_type_pink; break;
        case 2: noiseType = ma_noise_type_brownian; break;
        default: noiseType = ma_noise_type_white; break;
    }
    return ma_noise_set_type(&g_noise, noiseType) == MA_SUCCESS ? 0 : -1;
}

/* ==================== 滤波器 (DSP Effects) ==================== */

/* 低通滤波器 (LPF) */
static ma_lpf g_lpf;
static void* g_lpf_heap = NULL;
static int g_lpf_initialized = 0;

/* 初始化 LPF */
DLLEXPORT int ma_dll_lpf_init(int format, int channels, int sampleRate, float cutoffFrequency, int order)
{
    if (g_lpf_initialized) {
        ma_lpf_uninit(&g_lpf, NULL);
        if (g_lpf_heap) {
            free(g_lpf_heap);
            g_lpf_heap = NULL;
        }
        g_lpf_initialized = 0;
    }

    ma_format fmt;
    switch (format) {
        case 0: fmt = ma_format_f32; break;
        case 1: fmt = ma_format_s16; break;
        case 2: fmt = ma_format_s24; break;
        case 3: fmt = ma_format_s32; break;
        case 4: fmt = ma_format_u8; break;
        default: fmt = ma_format_f32; break;
    }

    ma_lpf_config config = ma_lpf_config_init(fmt, channels, sampleRate, cutoffFrequency, order);

    size_t heapSize = 0;
    if (ma_lpf_get_heap_size(&config, &heapSize) != MA_SUCCESS) {
        return -1;
    }

    g_lpf_heap = malloc(heapSize);
    if (!g_lpf_heap) {
        return -2;
    }

    if (ma_lpf_init_preallocated(&config, g_lpf_heap, &g_lpf) != MA_SUCCESS) {
        free(g_lpf_heap);
        g_lpf_heap = NULL;
        return -3;
    }

    g_lpf_initialized = 1;
    return 0;
}

/* 反初始化 LPF */
DLLEXPORT void ma_dll_lpf_uninit(void)
{
    if (g_lpf_initialized) {
        ma_lpf_uninit(&g_lpf, NULL);
        if (g_lpf_heap) {
            free(g_lpf_heap);
            g_lpf_heap = NULL;
        }
        g_lpf_initialized = 0;
    }
}

/* 处理 PCM 帧 (低通滤波) */
DLLEXPORT unsigned long long ma_dll_lpf_process_pcm_frames(void* framesOut, const void* framesIn, unsigned long long frameCount)
{
    if (!g_lpf_initialized) return 0;
    ma_lpf_process_pcm_frames(&g_lpf, framesOut, framesIn, frameCount);
    return frameCount;
}

/* 重新初始化 LPF (修改截止频率) */
DLLEXPORT int ma_dll_lpf_reinit(int format, int channels, int sampleRate, float cutoffFrequency, int order)
{
    if (!g_lpf_initialized) return -1;

    ma_format fmt;
    switch (format) {
        case 0: fmt = ma_format_f32; break;
        case 1: fmt = ma_format_s16; break;
        case 2: fmt = ma_format_s24; break;
        case 3: fmt = ma_format_s32; break;
        case 4: fmt = ma_format_u8; break;
        default: fmt = ma_format_f32; break;
    }

    ma_lpf_config config = ma_lpf_config_init(fmt, channels, sampleRate, cutoffFrequency, order);
    return ma_lpf_reinit(&config, &g_lpf) == MA_SUCCESS ? 0 : -1;
}

/* 高通滤波器 (HPF) */
static ma_hpf g_hpf;
static void* g_hpf_heap = NULL;
static int g_hpf_initialized = 0;

/* 初始化 HPF */
DLLEXPORT int ma_dll_hpf_init(int format, int channels, int sampleRate, float cutoffFrequency, int order)
{
    if (g_hpf_initialized) {
        ma_hpf_uninit(&g_hpf, NULL);
        if (g_hpf_heap) {
            free(g_hpf_heap);
            g_hpf_heap = NULL;
        }
        g_hpf_initialized = 0;
    }

    ma_format fmt;
    switch (format) {
        case 0: fmt = ma_format_f32; break;
        case 1: fmt = ma_format_s16; break;
        case 2: fmt = ma_format_s24; break;
        case 3: fmt = ma_format_s32; break;
        case 4: fmt = ma_format_u8; break;
        default: fmt = ma_format_f32; break;
    }

    ma_hpf_config config = ma_hpf_config_init(fmt, channels, sampleRate, cutoffFrequency, order);

    size_t heapSize = 0;
    if (ma_hpf_get_heap_size(&config, &heapSize) != MA_SUCCESS) {
        return -1;
    }

    g_hpf_heap = malloc(heapSize);
    if (!g_hpf_heap) {
        return -2;
    }

    if (ma_hpf_init_preallocated(&config, g_hpf_heap, &g_hpf) != MA_SUCCESS) {
        free(g_hpf_heap);
        g_hpf_heap = NULL;
        return -3;
    }

    g_hpf_initialized = 1;
    return 0;
}

/* 反初始化 HPF */
DLLEXPORT void ma_dll_hpf_uninit(void)
{
    if (g_hpf_initialized) {
        ma_hpf_uninit(&g_hpf, NULL);
        if (g_hpf_heap) {
            free(g_hpf_heap);
            g_hpf_heap = NULL;
        }
        g_hpf_initialized = 0;
    }
}

/* 处理 PCM 帧 (高通滤波) */
DLLEXPORT unsigned long long ma_dll_hpf_process_pcm_frames(void* framesOut, const void* framesIn, unsigned long long frameCount)
{
    if (!g_hpf_initialized) return 0;
    ma_hpf_process_pcm_frames(&g_hpf, framesOut, framesIn, frameCount);
    return frameCount;
}

/* 重新初始化 HPF (修改截止频率) */
DLLEXPORT int ma_dll_hpf_reinit(int format, int channels, int sampleRate, float cutoffFrequency, int order)
{
    if (!g_hpf_initialized) return -1;

    ma_format fmt;
    switch (format) {
        case 0: fmt = ma_format_f32; break;
        case 1: fmt = ma_format_s16; break;
        case 2: fmt = ma_format_s24; break;
        case 3: fmt = ma_format_s32; break;
        case 4: fmt = ma_format_u8; break;
        default: fmt = ma_format_f32; break;
    }

    ma_hpf_config config = ma_hpf_config_init(fmt, channels, sampleRate, cutoffFrequency, order);
    return ma_hpf_reinit(&config, &g_hpf) == MA_SUCCESS ? 0 : -1;
}
