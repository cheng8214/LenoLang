# miniaudio DLL API 文档

本 DLL 封装了 miniaudio 音频库的核心功能，供 Leno 语言通过 FFI 调用。

## 编译

```bash
cd src
build_dll.bat
```

输出文件：`build/miniaudio.dll`

---

## 常量定义

### 音频格式

| 常量 | 值 | 说明 |
|------|-----|------|
| `MA_FORMAT_F32` | 0 | 32位浮点格式 |
| `MA_FORMAT_S16` | 1 | 16位有符号整数 |
| `MA_FORMAT_S24` | 2 | 24位有符号整数 |
| `MA_FORMAT_S32` | 3 | 32位有符号整数 |
| `MA_FORMAT_U8`  | 4 | 8位无符号整数 |

---

## 设备管理 API

### ma_dll_get_version

```c
unsigned int ma_dll_get_version(void)
```

获取 miniaudio 版本号。

**返回值：** 版本号（格式：主版本 << 16 | 次版本 << 8 | 修订版本）

---

### ma_dll_init_device

```c
int ma_dll_init_device(int format, int channels, unsigned int sampleRate)
```

初始化音频播放设备。

**参数：**
- `format` - 音频格式（见常量定义）
- `channels` - 通道数（1=单声道, 2=立体声）
- `sampleRate` - 采样率（如 44100, 48000）

**返回值：**
- `0` - 成功
- `-1` - 设备已初始化
- `-2` - 初始化失败

---

### ma_dll_start

```c
int ma_dll_start(void)
```

开始音频播放。

**返回值：**
- `0` - 成功
- `-1` - 设备未初始化或启动失败

---

### ma_dll_stop

```c
int ma_dll_stop(void)
```

停止音频播放。

**返回值：**
- `0` - 成功
- `-1` - 设备未初始化

---

### ma_dll_uninit

```c
void ma_dll_uninit(void)
```

反初始化音频设备，释放资源。

---

### ma_dll_get_sample_rate

```c
unsigned int ma_dll_get_sample_rate(void)
```

获取当前设备采样率。

**返回值：** 采样率（Hz），未初始化时返回 0

---

### ma_dll_get_channels

```c
int ma_dll_get_channels(void)
```

获取当前设备通道数。

**返回值：** 通道数，未初始化时返回 0

---

## 音频生成 API

### ma_dll_generate_sine_f32

```c
void ma_dll_generate_sine_f32(
    float* buffer,
    unsigned int frameCount,
    int channels,
    float frequency,
    float amplitude,
    double* phase
)
```

生成正弦波到缓冲区。

**参数：**
- `buffer` - 输出缓冲区（f32格式）
- `frameCount` - 帧数
- `channels` - 通道数
- `frequency` - 频率（Hz）
- `amplitude` - 振幅（0.0 ~ 1.0）
- `phase` - 相位指针（用于保持连续性）

---

### ma_dll_generate_square_f32

```c
void ma_dll_generate_square_f32(
    float* buffer,
    unsigned int frameCount,
    int channels,
    float frequency,
    float amplitude,
    double* phase
)
```

生成方波到缓冲区。

**参数：** 同 `ma_dll_generate_sine_f32`

---

### ma_dll_silence_f32

```c
void ma_dll_silence_f32(float* buffer, unsigned int frameCount, int channels)
```

将缓冲区清零（静音）。

**参数：**
- `buffer` - 缓冲区
- `frameCount` - 帧数
- `channels` - 通道数

---

## 音频解码 API

### ma_dll_decoder_open

```c
ma_decoder* ma_dll_decoder_open(const char* filepath)
```

打开音频文件解码器。

**参数：**
- `filepath` - 音频文件路径

**返回值：** 解码器句柄，失败返回 NULL

**支持格式：** MP3, WAV, FLAC, OGG 等

---

### ma_dll_decoder_close

```c
void ma_dll_decoder_close(ma_decoder* decoder)
```

关闭解码器，释放资源。

**参数：**
- `decoder` - 解码器句柄

---

### ma_dll_decoder_read_pcm_frames

```c
unsigned long long ma_dll_decoder_read_pcm_frames(
    ma_decoder* decoder,
    void* framesOut,
    unsigned long long frameCount
)
```

读取 PCM 帧数据。

**参数：**
- `decoder` - 解码器句柄
- `framesOut` - 输出缓冲区
- `frameCount` - 要读取的帧数

**返回值：** 实际读取的帧数

---

### ma_dll_decoder_get_sample_rate

```c
unsigned int ma_dll_decoder_get_sample_rate(ma_decoder* decoder)
```

获取解码器采样率。

---

### ma_dll_decoder_get_channels

```c
int ma_dll_decoder_get_channels(ma_decoder* decoder)
```

获取解码器通道数。

---

### ma_dll_decoder_get_format

```c
int ma_dll_decoder_get_format(ma_decoder* decoder)
```

获取解码器音频格式。

**返回值：** 格式常量（0-4）

---

### ma_dll_decoder_seek_to_pcm_frame

```c
int ma_dll_decoder_seek_to_pcm_frame(
    ma_decoder* decoder,
    unsigned long long frameIndex
)
```

定位到指定帧。

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_decoder_get_length_in_pcm_frames

```c
unsigned long long ma_dll_decoder_get_length_in_pcm_frames(ma_decoder* decoder)
```

获取音频总帧数（时长）。

**返回值：** 总帧数，未知时返回 0

---

## 音频引擎 API（高级）

### ma_dll_engine_init

```c
int ma_dll_engine_init(void)
```

初始化音频引擎（高级API）。

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_engine_uninit

```c
void ma_dll_engine_uninit(void)
```

反初始化音频引擎。

---

### ma_dll_engine_play_sound

```c
int ma_dll_engine_play_sound(const char* filepath)
```

播放音频文件（简单方式，不支持暂停）。

**参数：**
- `filepath` - 音频文件路径

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_engine_get_sample_rate

```c
unsigned int ma_dll_engine_get_sample_rate(void)
```

获取引擎采样率。

---

### ma_dll_engine_get_channels

```c
int ma_dll_engine_get_channels(void)
```

获取引擎通道数。

---

## Sound 对象 API（支持暂停/恢复/音量控制）

### ma_dll_sound_init_from_file

```c
int ma_dll_sound_init_from_file(const char* filepath)
```

从文件创建 Sound 对象。

**参数：**
- `filepath` - 音频文件路径

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_sound_uninit

```c
void ma_dll_sound_uninit(void)
```

反初始化 Sound 对象。

---

### ma_dll_sound_start

```c
int ma_dll_sound_start(void)
```

开始播放 Sound。

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_sound_stop

```c
int ma_dll_sound_stop(void)
```

停止播放 Sound。

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_sound_pause

```c
int ma_dll_sound_pause(void)
```

暂停播放。

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_sound_resume

```c
int ma_dll_sound_resume(void)
```

恢复播放。

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_sound_is_playing

```c
int ma_dll_sound_is_playing(void)
```

检查是否正在播放。

**返回值：**
- `1` - 正在播放
- `0` - 未播放或未初始化

---

### ma_dll_sound_set_volume

```c
void ma_dll_sound_set_volume(float volume)
```

设置音量。

**参数：**
- `volume` - 音量值（0.0 ~ 1.0）

---

### ma_dll_sound_get_volume

```c
float ma_dll_sound_get_volume(void)
```

获取当前音量。

**返回值：** 音量值（0.0 ~ 1.0）

---

### ma_dll_sound_seek_to_second

```c
int ma_dll_sound_seek_to_second(float second)
```

定位到指定时间（秒）。

**参数：**
- `second` - 时间（秒）

**返回值：**
- `0` - 成功
- `-1` - 失败

---

### ma_dll_sound_get_position

```c
float ma_dll_sound_get_position(void)
```

获取当前播放位置（秒）。

---

### ma_dll_sound_get_duration

```c
float ma_dll_sound_get_duration(void)
```

获取音频总时长（秒）。

---

## 数学工具 API

### ma_dll_sin

```c
double ma_dll_sin(double x)
```

正弦函数。

---

### ma_dll_cos

```c
double ma_dll_cos(double x)
```

余弦函数。

---

### ma_dll_sqrt

```c
double ma_dll_sqrt(double x)
```

平方根函数。

---

### ma_dll_pow

```c
double ma_dll_pow(double base, double exp)
```

幂函数。

---

## Leno 调用示例

### 基础设备操作

```leno
import ffi

main() {
    // 加载DLL
    var ma = ffi.load("../build/miniaudio.dll")
    
    // 初始化设备（F32格式，立体声，48kHz）
    ffi.call_int(ma, "ma_dll_init_device", 0, 2, 48000)
    
    // 开始播放
    ffi.call_int(ma, "ma_dll_start")
    
    // 播放5秒...
    sleep(5000)
    
    // 停止并清理
    ffi.call_void(ma, "ma_dll_stop")
    ffi.call_void(ma, "ma_dll_uninit")
    ffi.free(ma)
}
```

### 播放音频文件（简单方式）

```leno
import ffi

main() {
    var ma = ffi.load("../build/miniaudio.dll")
    
    // 初始化引擎
    ffi.call_int(ma, "ma_dll_engine_init")
    
    // 播放文件
    ffi.call_int(ma, "ma_dll_engine_play_sound", "music.mp3")
    
    // 等待播放完成
    sleep(10000)
    
    ffi.call_void(ma, "ma_dll_engine_uninit")
    ffi.free(ma)
}
```

### 使用 Sound 对象（支持暂停/恢复）

```leno
import ffi

main() {
    var ma = ffi.load("../build/miniaudio.dll")
    
    // 初始化引擎
    ffi.call_int(ma, "ma_dll_engine_init")
    
    // 创建 Sound 对象
    ffi.call_int(ma, "ma_dll_sound_init_from_file", "music.mp3")
    
    // 开始播放
    ffi.call_int(ma, "ma_dll_sound_start")
    
    // 播放3秒
    sleep(3000)
    
    // 暂停
    ffi.call_int(ma, "ma_dll_sound_pause")
    print("已暂停")
    sleep(2000)
    
    // 恢复
    ffi.call_int(ma, "ma_dll_sound_resume")
    print("已恢复")
    sleep(3000)
    
    // 停止
    ffi.call_int(ma, "ma_dll_sound_stop")
    
    // 清理
    ffi.call_void(ma, "ma_dll_sound_uninit")
    ffi.call_void(ma, "ma_dll_engine_uninit")
    ffi.free(ma)
}
```

### 音量控制

```leno
import ffi

main() {
    var ma = ffi.load("../build/miniaudio.dll")
    
    ffi.call_int(ma, "ma_dll_engine_init")
    ffi.call_int(ma, "ma_dll_sound_init_from_file", "music.mp3")
    
    // 设置音量为 50%
    ffi.call_void(ma, "ma_dll_sound_set_volume", 0.5)
    
    // 播放
    ffi.call_int(ma, "ma_dll_sound_start")
    sleep(5000)
    
    // 获取当前音量
    float vol = ffi.call_double(ma, "ma_dll_sound_get_volume")
    print("当前音量: " + vol)
    
    ffi.call_void(ma, "ma_dll_sound_uninit")
    ffi.call_void(ma, "ma_dll_engine_uninit")
    ffi.free(ma)
}
```

### 解码音频文件

```leno
import ffi

main() {
    var ma = ffi.load("../build/miniaudio.dll")
    
    // 打开解码器
    Ptr decoder = ffi.call_ptr(ma, "ma_dll_decoder_open", "music.mp3")
    if decoder == null {
        print("打开文件失败")
        return
    }
    
    // 获取信息
    int sampleRate = ffi.call_int(ma, "ma_dll_decoder_get_sample_rate", decoder)
    int channels = ffi.call_int(ma, "ma_dll_decoder_get_channels", decoder)
    print("采样率: " + sampleRate + ", 通道: " + channels)
    
    // 读取数据
    Ptr buffer = ffi.malloc(4096 * 4 * 2)  // 4096帧 * 4字节 * 2通道
    int framesRead = ffi.call_int(ma, "ma_dll_decoder_read_pcm_frames", decoder, buffer, 4096)
    
    ffi.free(buffer)
    ffi.call_void(ma, "ma_dll_decoder_close", decoder)
    ffi.free(ma)
}
```

---

## 注意事项

1. **回调函数**：`ma_dll_set_callback` 用于设置音频数据回调，需要配合 FFI 回调机制使用
2. **线程安全**：设备启动/停止不能在回调中调用
3. **资源释放**：使用完毕后必须调用对应的 uninit/close 函数释放资源
4. **DLL路径**：Leno加载DLL时路径可以是相对路径或绝对路径
5. **Sound 对象**：需要先调用 `ma_dll_engine_init` 初始化引擎，再创建 Sound 对象
