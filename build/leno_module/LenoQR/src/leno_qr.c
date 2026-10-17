/* ============================================================================
 * leno_qr.c —— quirc 的 Leno 友好包装层（LenoQR 模块的唯一 C 入口）
 *
 * 为什么要有这一层（而不是让 Leno 直接调 quirc_*）：
 *   ① quirc 的公开 API 要传 `struct quirc_code` / `struct quirc_data`（各含
 *      ~9KB 内嵌数组）。让 Leno 侧用 cstruct 严格对齐这两个结构 = 把 quirc 的
 *      ABI 细节抄进 .leno ⇒ 换 quirc 版本就静默错位。这里把它们挡在 C 里，
 *      对外只暴露「像素缓冲 → UTF-8 文本」这一件事。
 *   ② quirc 只吃**灰度紧凑缓冲**（w*h，无 stride），而调用方给的是带 pitch 的
 *      ARGB8888（SDL3 的 surf 就是）⇒ 转换/逐行拷贝都在 C 里做一次。
 *
 * 约定（与 lib/qr_native.leno 的注释严格一致，改一处必须改另一处）：
 *   返回值：
 *     >= 0  = 成功解码，值为**写进 out 的字节数**（payload 是 UTF-8/ASCII 字节）
 *        0  = 图里没有找到能解开的二维码（正常情形，不是错误）
 *       -2  = 参数非法（空指针 / w,h <= 0 / stride < w / cap <= 0）
 *       -3  = quirc 初始化或内存分配失败
 *   out 不会自动补 '\0'（Leno 侧用 ffi.read_bytes(out, 0, n) 取，长度说了算）。
 *
 * 注意：本文件只依赖 quirc 的公开头 → 想升级 quirc 只需替换 src/ 下那 6 个文件。
 * ========================================================================== */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "quirc.h"

#ifdef _WIN32
#  define LQ_EXPORT __declspec(dllexport)
#else
#  define LQ_EXPORT __attribute__((visibility("default")))
#endif

/* ----------------------------------------------------------------------------
 * ABI 版本：Leno 侧启动时先调它做一次"握手"
 *   为什么需要：dlopen 成功不代表函数签名一致（旧 DLL 配新 .leno 时，
 *   参数个数/顺序对不上的调用在 x64 上**不会崩**，只会读出垃圾 ⇒ 静默错值）。
 *   .leno 侧把它和模块里的常量比对，不等就直接报"请重跑 src/build_dll.*"。
 * -------------------------------------------------------------------------- */
#define LENO_QR_ABI 2   /* 2：新增多码枚举（begin/count/get/meta/end） */

LQ_EXPORT int32_t leno_qr_abi(void) { return LENO_QR_ABI; }

/* quirc 自身的版本串（静态内存，调用方不要 free） */
LQ_EXPORT const char *leno_qr_version(void) { return quirc_version(); }

/* 能装下的最大 payload（QR 版本 40 + 字节模式的理论上限；与 quirc 的缓冲一致） */
LQ_EXPORT int32_t leno_qr_max_payload(void) { return QUIRC_MAX_PAYLOAD; }

/* ----------------------------------------------------------------------------
 * 灰度缓冲 → 文本
 *   gray  : 每像素 1 字节，0 = 黑，255 = 白
 *   stride: 每行字节数（>= w；调用方常常给 w 本身）
 * -------------------------------------------------------------------------- */
LQ_EXPORT int32_t leno_qr_scan_gray(const uint8_t *gray, int32_t w, int32_t h,
                                    int32_t stride, uint8_t *out, int32_t cap)
{
    struct quirc *q;
    uint8_t *buf;
    int bw = 0, bh = 0, n, i;
    int32_t rc = 0;

    if (!gray || !out || w <= 0 || h <= 0 || cap <= 0 || stride < w) return -2;

    q = quirc_new();
    if (!q) return -3;
    if (quirc_resize(q, (int)w, (int)h) < 0) { quirc_destroy(q); return -3; }

    buf = quirc_begin(q, &bw, &bh);
    if (!buf) { quirc_destroy(q); return -3; }

    /* quirc 的输入是**紧凑**的 w*h（它的 resize 已经按图片宽高算好），
     * 所以 stride 与 w 不同时必须逐行拷 —— 直接 memcpy 整段会把行尾的
     * 填充字节当成下一行像素（越往后越歪，二维码定位图案直接认不出来）。*/
    for (i = 0; i < h; i++) {
        memcpy(buf + (size_t)i * (size_t)w, gray + (size_t)i * (size_t)stride, (size_t)w);
    }

    quirc_end(q);
    n = quirc_count(q);

    /* 一张图里可能有多枚码：先返回**第一个能解开的**（多码枚举等有真实需求再加）*/
    for (i = 0; i < n; i++) {
        struct quirc_code code;
        struct quirc_data data;
        int len;

        quirc_extract(q, i, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;

        len = data.payload_len;
        if (len > (int)cap) len = (int)cap;   /* 按 cap 截断，不越界写 */
        memcpy(out, data.payload, (size_t)len);
        rc = (int32_t)len;
        break;
    }

    quirc_destroy(q);
    return rc;
}

/* ----------------------------------------------------------------------------
 * ARGB8888 / BGR 缓冲 → 文本（内部转灰度）
 *   bpp = 4：内存序 B,G,R,A（SDL 的 PIXELFORMAT_ARGB8888 在小端机上就是这个）
 *   bpp = 3：内存序 B,G,R
 *   pitch  : 每行字节数（SDL surf 的 pitch，通常 = w*bpp）
 *   灰度口径：ITU-R BT.601 luma（0.299R + 0.587G + 0.114B），整数运算 + 四舍五入
 * -------------------------------------------------------------------------- */
LQ_EXPORT int32_t leno_qr_scan_argb(const uint8_t *px, int32_t w, int32_t h,
                                    int32_t pitch, int32_t bpp,
                                    uint8_t *out, int32_t cap)
{
    uint8_t *gray;
    int32_t stride = w;
    int32_t y, x;
    int32_t rc;

    if (!px || !out || w <= 0 || h <= 0 || cap <= 0) return -2;
    if (bpp != 3 && bpp != 4) return -2;
    if (pitch < w * bpp) return -2;

    gray = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!gray) return -3;

    for (y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * (size_t)pitch;
        uint8_t *dst = gray + (size_t)y * (size_t)stride;
        for (x = 0; x < w; x++) {
            int b = row[x * bpp + 0];
            int g = row[x * bpp + 1];
            int r = row[x * bpp + 2];
            dst[x] = (uint8_t)((r * 299 + g * 587 + b * 114 + 500) / 1000);
        }
    }

    rc = leno_qr_scan_gray(gray, w, h, stride, out, cap);
    free(gray);
    return rc;
}

/* ============================================================================
 * 多码枚举（ABI 2 新增）
 * ----------------------------------------------------------------------------
 * 为什么用"上下文句柄"而不是"一次调用返回全部"：
 *   quirc 的流程是 begin(填像素) → end(检测) → count/extract/decode。
 *   若每个码都从头再来一遍，一张图要**检测 N 次**（检测远贵于解码）；
 *   这里把检测结果挂在句柄上，调用方按 index 取 payload。
 *
 * 生命周期：begin → count/get/meta 任意次 → end（**必须调**，内部持有 malloc 的 quirc 对象）
 *
 * 返回码：
 *   begin : 句柄（NULL = 参数错或内存不足）
 *   count : >=0 检测到的码数；-2 句柄为空
 *   get   : >=0 payload 字节数；-1 该码**检出但解不开**（纠错失败/不是 QR 等）；-2 参数错
 *   meta  : 0 成功，-2 参数错。out 四个 int32 = [version, ecc_level, mask, data_type]
 *           ⚠ ecc_level 是 quirc 的口径（M=0, L=1, H=2, Q=3），**不是** L/M/Q/H 下标
 * ========================================================================== */

static struct quirc *lq_detect_gray(const uint8_t *gray, int32_t w, int32_t h, int32_t stride)
{
    struct quirc *q;
    uint8_t *buf;
    int bw = 0, bh = 0, i;

    if (!gray || w <= 0 || h <= 0 || stride < w) return NULL;

    q = quirc_new();
    if (!q) return NULL;
    if (quirc_resize(q, (int)w, (int)h) < 0) { quirc_destroy(q); return NULL; }
    buf = quirc_begin(q, &bw, &bh);
    if (!buf) { quirc_destroy(q); return NULL; }
    for (i = 0; i < h; i++) {
        memcpy(buf + (size_t)i * (size_t)w, gray + (size_t)i * (size_t)stride, (size_t)w);
    }
    quirc_end(q);
    return q;
}

LQ_EXPORT void *leno_qr_begin(const uint8_t *gray, int32_t w, int32_t h, int32_t stride)
{
    return (void *)lq_detect_gray(gray, w, h, stride);
}

LQ_EXPORT void *leno_qr_begin_argb(const uint8_t *px, int32_t w, int32_t h,
                                   int32_t pitch, int32_t bpp)
{
    uint8_t *gray;
    struct quirc *q;
    int32_t y, x;

    if (!px || w <= 0 || h <= 0) return NULL;
    if (bpp != 3 && bpp != 4) return NULL;
    if (pitch < w * bpp) return NULL;

    gray = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!gray) return NULL;
    for (y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * (size_t)pitch;
        uint8_t *dst = gray + (size_t)y * (size_t)w;
        for (x = 0; x < w; x++) {
            int b = row[x * bpp + 0];
            int g = row[x * bpp + 1];
            int r = row[x * bpp + 2];
            dst[x] = (uint8_t)((r * 299 + g * 587 + b * 114 + 500) / 1000);
        }
    }
    q = lq_detect_gray(gray, w, h, w);
    free(gray);
    return (void *)q;
}

LQ_EXPORT int32_t leno_qr_count(void *handle)
{
    if (!handle) return -2;
    return (int32_t)quirc_count((const struct quirc *)handle);
}

LQ_EXPORT int32_t leno_qr_get(void *handle, int32_t index, uint8_t *out, int32_t cap)
{
    struct quirc_code code;
    struct quirc_data data;
    int n, len;

    if (!handle || !out || cap <= 0 || index < 0) return -2;
    n = quirc_count((const struct quirc *)handle);
    if (index >= n) return -2;

    quirc_extract((const struct quirc *)handle, (int)index, &code);
    if (quirc_decode(&code, &data) != QUIRC_SUCCESS) return -1;

    len = data.payload_len;
    if (len > (int)cap) len = (int)cap;
    memcpy(out, data.payload, (size_t)len);
    return (int32_t)len;
}

LQ_EXPORT int32_t leno_qr_meta(void *handle, int32_t index, int32_t *out4)
{
    struct quirc_code code;
    struct quirc_data data;
    int n;

    if (!handle || !out4 || index < 0) return -2;
    n = quirc_count((const struct quirc *)handle);
    if (index >= n) return -2;

    quirc_extract((const struct quirc *)handle, (int)index, &code);
    if (quirc_decode(&code, &data) != QUIRC_SUCCESS) return -1;

    out4[0] = data.version;
    out4[1] = data.ecc_level;
    out4[2] = data.mask;
    out4[3] = data.data_type;
    return 0;
}

LQ_EXPORT void leno_qr_end(void *handle)
{
    if (handle) quirc_destroy((struct quirc *)handle);
}
