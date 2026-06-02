/* Leno GUI - 图片加载（参考 SDL3 SDL_stb.c）
 * 使用 stb_image 实现 PNG/JPEG/BMP 等格式的加载
 * 通过 #include 方式引入到各平台文件中，共享代码
 */

/* ===== stb_image 配置（参考 SDL3） ===== */
#define STB_IMAGE_STATIC
#define STBI_NO_THREAD_LOCALS
#define STBI_FAILURE_USERMSG
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)

/* 重定向内存分配函数（参考 SDL3） */
#include <stdlib.h>
#define STBI_MALLOC(sz)           malloc(sz)
#define STBI_REALLOC(p,newsz)     realloc(p, newsz)
#define STBI_FREE(p)              free(p)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ===== 平台共享的图片加载函数 ===== */

/* 从文件加载图片并创建图像（参考 SDL3 SDL_LoadSurface + CreateTextureFromSurface）
 *
 * SDL3 的做法：
 *   1. 用 stbi_load() 加载图片文件得到 RGBA 像素数据
 *   2. 创建 SDL_Surface 包装像素数据
 *   3. 通过 SDL_CreateTextureFromSurface() 创建 GPU 纹理
 *
 * 本项目的做法（软件渲染，无需 GPU 纹理）：
 *   1. 用 stbi_load() 加载图片文件得到 RGBA 像素数据
 *   2. 直接创建 LenoGUIPlatformImage 包装像素数据
 *   3. 返回图像对象供 render_image 使用
 */
static LenoGUIPlatformImage* sw_load_image(const char* filepath) {
    int w = 0, h = 0, channels = 0;

    /* 加载图片为 RGBA 格式（stbi_load 自动转换为 4 通道） */
    unsigned char* img_data = stbi_load(filepath, &w, &h, &channels, 4);
    if (!img_data) {
        /* stbi_failure_reason() 返回失败原因 */
        return NULL;
    }

    /* 分配图像对象 */
    LenoGUIPlatformImage* tex = (LenoGUIPlatformImage*)malloc(sizeof(LenoGUIPlatformImage));
    if (!tex) {
        stbi_image_free(img_data);
        return NULL;
    }

    tex->width = w;
    tex->height = h;
    tex->pitch = w * 4;
    tex->access = LENO_GUI_IMAGEACCESS_STATIC;

    /* 将 stb_image 的 RGBA 数据转换为平台的像素格式（BGRA）
     * stb_image 返回顺序: R, G, B, A
     * Leno GUI 内存布局（小端序）: B, G, R, A
     */
    tex->pixels = (uint32_t*)malloc(w * h * sizeof(uint32_t));
    if (!tex->pixels) {
        free(tex);
        stbi_image_free(img_data);
        return NULL;
    }

    unsigned char* src = img_data;
    uint32_t* dst = tex->pixels;
    for (int i = 0; i < w * h; i++) {
        unsigned char r = *src++;
        unsigned char g = *src++;
        unsigned char b = *src++;
        unsigned char a = *src++;
        /* LENO_GUI_PIXEL: (A<<24)|(R<<16)|(G<<8)|B */
        *dst++ = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    stbi_image_free(img_data);

    /* 平台特定字段置零（render target 相关） */
    tex->target_dc = NULL;
    tex->target_bitmap = NULL;
    tex->old_bitmap = NULL;

    return tex;
}

/* 从内存数据加载图片并创建图像 */
static LenoGUIPlatformImage* sw_load_image_from_memory(const unsigned char* data, int data_len) {
    int w = 0, h = 0, channels = 0;

    unsigned char* img_data = stbi_load_from_memory(data, data_len, &w, &h, &channels, 4);
    if (!img_data) {
        return NULL;
    }

    LenoGUIPlatformImage* tex = (LenoGUIPlatformImage*)malloc(sizeof(LenoGUIPlatformImage));
    if (!tex) {
        stbi_image_free(img_data);
        return NULL;
    }

    tex->width = w;
    tex->height = h;
    tex->pitch = w * 4;
    tex->access = LENO_GUI_IMAGEACCESS_STATIC;

    tex->pixels = (uint32_t*)malloc(w * h * sizeof(uint32_t));
    if (!tex->pixels) {
        free(tex);
        stbi_image_free(img_data);
        return NULL;
    }

    unsigned char* src = img_data;
    uint32_t* dst = tex->pixels;
    for (int i = 0; i < w * h; i++) {
        unsigned char r = *src++;
        unsigned char g = *src++;
        unsigned char b = *src++;
        unsigned char a = *src++;
        *dst++ = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    stbi_image_free(img_data);

    tex->target_dc = NULL;
    tex->target_bitmap = NULL;
    tex->old_bitmap = NULL;

    return tex;
}

/* 获取 stb_image 最后一次失败的原因 */
static const char* sw_get_image_error(void) {
    return stbi_failure_reason();
}

/* ===== 平台公共 API 实现 ===== */

LenoGUIPlatformImage* leno_gui_platform_load_image(const char* filepath) {
    return sw_load_image(filepath);
}

LenoGUIPlatformImage* leno_gui_platform_load_image_mem(const unsigned char* data, int len) {
    return sw_load_image_from_memory(data, len);
}

const char* leno_gui_platform_get_image_error(void) {
    return sw_get_image_error();
}
