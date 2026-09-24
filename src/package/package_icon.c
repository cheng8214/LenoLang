/**
 * package_icon.c - 打包时把 VM 基底的**应用图标**换成应用指定的 .ico
 * （resource.toml 的 [pack] icon = "app.ico"）
 *
 * ----------------------------------------------------------------------------
 * 为什么改的是 VM 副本，而不是最终产物：
 *   最终 exe = [VM 二进制][资源段][lenb]，而图标位于 **PE 资源段** —— 也就是文件
 *   最开头那一段（prepend 进去的 VM 二进制里）。可 EndUpdateResource 会**按 PE
 *   结构重写整个文件**，对最终 exe 调用会把尾部附加的资源段 + lenb 一起丢掉 ✗。
 *   ⇒ 必须：把干净的 VM 映像单独写成临时文件 → 改它的图标 → 再 prepend。
 *   （这也是本文件只处理"一个完整 PE 映像"的原因：不解析、不重建 PE 结构。）
 *
 * 替换方式：系统 API BeginUpdateResource / UpdateResource / EndUpdateResource，
 *   不自己动 PE 节表 —— 重建 .rsrc 段要改节表、SizeOfImage、数据目录，改坏一点
 *   就是个跑不起来的 exe，交给系统 API 才稳。
 *
 * 写入布局（与 .rc 编出来的完全一致）：
 *   每个图像一条 RT_ICON（id = 1..N）+ 一条 RT_GROUP_ICON（id = 1）
 *   · Windows 取**最低 id** 的图标组当应用图标 ⇒ 组 id 固定 1；
 *   · 组里每条 GRPICONDIRENTRY 的 nID 必须等于对应 RT_ICON 的 id。
 *
 * ⚠ 语言 id 是个坑：新写的条目若与旧条目的语言不同，(type,id) 就会同时存在两条
 *   不同语言的记录，Windows 按自己的语言偏好挑 ⇒ 可能挑到**旧图标**。
 *   所以这里先把旧条目的 (id, 语言) 枚举出来，沿用它的语言，并删掉语言不一致 /
 *   id 超出范围的残留条目。
 * ----------------------------------------------------------------------------
 * 平台：只有 Windows 有意义（图标是 PE 资源）。非 Windows 返回 -2，调用方提示后忽略。
 */

#include "../include/leno_package.h"
#include "include/leno_types.h"     /* MAX_PATH_LEN */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef _WIN32

/* 非 Windows：图标不是 PE 资源（macOS 用 .icns、Linux 靠 .desktop），无对应物 */
int package_icon_replace(const unsigned char* exe_img, size_t exe_size,
                         const char* ico_path,
                         unsigned char** out, size_t* out_size,
                         char* err, size_t err_size) {
    (void)exe_img; (void)exe_size; (void)ico_path;
    (void)out; (void)out_size;
    if (err && err_size) snprintf(err, err_size, "本平台不支持替换 PE 图标");
    return -2;
}

#else  /* _WIN32 */

#include <windows.h>

/* 一个 .ico 里最多认多少张图（Windows 自己也就几十张，64 足够） */
#define ICON_MAX_IMAGES 64
/* 枚举旧图标资源时最多记多少条 (id, 语言) */
#define ICON_MAX_REFS   64

/* .ico 目录项（ICONDIRENTRY，16 字节）*/
typedef struct {
    uint8_t  width;        /* 0 表示 256 */
    uint8_t  height;
    uint8_t  color_count;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t bytes;        /* 该图像数据长度 */
    uint32_t offset;       /* 该图像数据在 .ico 文件里的偏移 */
} IcoImage;

/* 旧图标资源的一条：(类型, id, 语言) —— 语言必须沿用，否则 Windows 可能挑到旧的 */
typedef struct {
    WORD id;
    WORD lang;
} IconRef;

typedef struct {
    IconRef items[ICON_MAX_REFS];
    int count;
} IconRefList;

/* ---------------------------------------------------------------- 小工具 */

/* UTF-8 路径 → 宽字符（Windows 下所有文件 API 都走宽字符，中文路径才安全） */
static int icon_to_wide(const char* utf8, wchar_t* out, size_t n) {
    if (!utf8 || !out || n == 0) return 0;
    int r = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)n);
    return (r > 0 && (size_t)r <= n);
}

/* 生成一个唯一的临时文件路径（图标替换要先把 VM 映像落到磁盘） */
static int icon_temp_path(wchar_t* out, size_t n) {
    wchar_t dir[MAX_PATH_LEN];
    DWORD got = GetTempPathW(MAX_PATH_LEN, dir);
    if (got == 0 || got >= MAX_PATH_LEN) return 0;
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    unsigned long long tick = (unsigned long long)GetTickCount64();
    _snwprintf(out, n, L"%sleno_icon_%lu_%llu.tmp", dir, pid, tick);
    out[n - 1] = L'\0';
    return 1;
}

/* 读整个文件（宽字符路径） */
static int icon_read_file(const wchar_t* path, unsigned char** out, size_t* out_size) {
    *out = NULL;
    *out_size = 0;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buf;
    *out_size = (size_t)sz;
    return 0;
}

/* ---------------------------------------------------------------- .ico 解析 */

/* 解析 .ico 目录：6 字节头 + N × 16 字节目录项。
 * 只做**结构校验**，不碰图像数据本身 —— 数据是原样搬进 RT_ICON 的，
 * 不需要解码（BMP/DIB 与 PNG 两种编码都合法，Vista+ 都认）。 */
static int ico_parse(const unsigned char* data, size_t size,
                     IcoImage* out, int max, int* out_count) {
    *out_count = 0;
    if (!data || size < 6) return -1;
    uint16_t reserved = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t type     = (uint16_t)(data[2] | (data[3] << 8));
    uint16_t count    = (uint16_t)(data[4] | (data[5] << 8));
    if (reserved != 0 || type != 1) return -1;   /* type 1 = 图标（2 是光标） */
    if (count == 0 || count > (uint16_t)max) return -1;
    if (6 + (size_t)count * 16 > size) return -1;

    for (int i = 0; i < (int)count; i++) {
        const unsigned char* e = data + 6 + (size_t)i * 16;
        IcoImage* im = &out[i];
        im->width       = e[0];
        im->height      = e[1];
        im->color_count = e[2];
        im->planes      = (uint16_t)(e[4] | (e[5] << 8));
        im->bit_count   = (uint16_t)(e[6] | (e[7] << 8));
        im->bytes       = (uint32_t)(e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24));
        im->offset      = (uint32_t)(e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24));
        if (im->bytes == 0) return -1;
        if ((size_t)im->offset + (size_t)im->bytes > size) return -1;   /* 越界：文件坏了 */
    }
    *out_count = (int)count;
    return 0;
}

/* ------------------------------------------------- 枚举旧图标资源 */

static BOOL CALLBACK icon_enum_lang_cb(HMODULE mod, LPCWSTR type, LPCWSTR name,
                                       WORD lang, LONG_PTR lparam) {
    (void)mod; (void)type;
    IconRefList* list = (IconRefList*)lparam;
    if (!IS_INTRESOURCE(name)) return TRUE;      /* 只认整数 id（图标资源都是整数 id） */
    if (list->count >= ICON_MAX_REFS) return FALSE;   /* 收满了：停下 */
    list->items[list->count].id = (WORD)(ULONG_PTR)name;
    list->items[list->count].lang = lang;
    list->count++;
    return TRUE;
}

static BOOL CALLBACK icon_enum_name_cb(HMODULE mod, LPCWSTR type, LPWSTR name, LONG_PTR lparam) {
    /* 对每个 id 再枚举它的语言 */
    EnumResourceLanguagesW(mod, type, name, icon_enum_lang_cb, lparam);
    return TRUE;
}

/* 把 exe 里某个类型（RT_ICON / RT_GROUP_ICON）的 (id, 语言) 全收进 list */
static void icon_collect_refs(HMODULE mod, LPCWSTR type, IconRefList* list) {
    list->count = 0;
    EnumResourceNamesW(mod, type, icon_enum_name_cb, (LONG_PTR)list);
}

/* ---------------------------------------------------------------- 主入口 */

int package_icon_replace(const unsigned char* exe_img, size_t exe_size,
                         const char* ico_path,
                         unsigned char** out, size_t* out_size,
                         char* err, size_t err_size) {
    *out = NULL;
    *out_size = 0;
    if (err && err_size) err[0] = '\0';
    if (!exe_img || exe_size == 0 || !ico_path || !ico_path[0]) {
        if (err && err_size) snprintf(err, err_size, "参数错误");
        return -1;
    }

    /* ① 读并解析 .ico */
    wchar_t wico[MAX_PATH_LEN];
    if (!icon_to_wide(ico_path, wico, MAX_PATH_LEN)) {
        if (err && err_size) snprintf(err, err_size, "图标路径无效: %s", ico_path);
        return -1;
    }
    unsigned char* ico = NULL;
    size_t ico_size = 0;
    if (icon_read_file(wico, &ico, &ico_size) != 0) {
        if (err && err_size) snprintf(err, err_size, "读不到图标文件: %s", ico_path);
        return -1;
    }
    IcoImage imgs[ICON_MAX_IMAGES];
    int img_count = 0;
    if (ico_parse(ico, ico_size, imgs, ICON_MAX_IMAGES, &img_count) != 0) {
        if (err && err_size) {
            snprintf(err, err_size, "不是有效的 .ico（或条目数超过 %d）: %s",
                     ICON_MAX_IMAGES, ico_path);
        }
        free(ico);
        return -1;
    }

    /* ② VM 映像写临时文件（EndUpdateResource 会按 PE 结构重写整个文件，
     *    所以只能拿"干净的 VM 映像"来改，不能改最终 exe —— 那会丢掉尾部附加数据） */
    wchar_t wtmp[MAX_PATH_LEN];
    if (!icon_temp_path(wtmp, MAX_PATH_LEN)) {
        if (err && err_size) snprintf(err, err_size, "取临时目录失败");
        free(ico);
        return -1;
    }
    FILE* f = _wfopen(wtmp, L"wb");
    if (!f || fwrite(exe_img, 1, exe_size, f) != exe_size) {
        if (f) fclose(f);
        _wremove(wtmp);
        if (err && err_size) snprintf(err, err_size, "写临时文件失败: %ls", wtmp);
        free(ico);
        return -1;
    }
    fclose(f);

    int rc = -1;
    char why[256] = {0};

    /* ③ 枚举 VM 里已有的图标资源（拿到 id 与**语言**） */
    IconRefList ico_refs, grp_refs;
    HMODULE mod = LoadLibraryExW(wtmp, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!mod) {
        snprintf(why, sizeof(why), "LoadLibraryEx 失败 (err=%lu)", (unsigned long)GetLastError());
    } else {
        icon_collect_refs(mod, (LPCWSTR)RT_ICON, &ico_refs);
        icon_collect_refs(mod, (LPCWSTR)RT_GROUP_ICON, &grp_refs);
        FreeLibrary(mod);

        /* 新条目沿用旧条目的语言（组优先）—— 语言不一致会让 Windows 挑到旧图标 */
        WORD lang = 0;
        if (grp_refs.count > 0) lang = grp_refs.items[0].lang;
        else if (ico_refs.count > 0) lang = ico_refs.items[0].lang;

        /* ④ 开始更新（FALSE = 保留清单等其它资源） */
        HANDLE hu = BeginUpdateResourceW(wtmp, FALSE);
        if (!hu) {
            snprintf(why, sizeof(why), "BeginUpdateResource 失败 (err=%lu)", (unsigned long)GetLastError());
        } else {
            int ok = 1;

            /* 旧条目里，凡是要被覆盖的（id 在 1..N 且语言相同）不用删，直接覆盖；
             * 其余（id 超出范围、语言不同）必须删掉 —— 否则留下多余图像，
             * 或因语言不同让 Windows 挑到旧图标。 */
            for (int i = 0; i < ico_refs.count && ok; i++) {
                IconRef r = ico_refs.items[i];
                if (r.id >= 1 && r.id <= (WORD)img_count && r.lang == lang) continue;
                if (!UpdateResourceW(hu, (LPCWSTR)RT_ICON, MAKEINTRESOURCEW(r.id),
                                     r.lang, NULL, 0)) ok = 0;
            }
            for (int i = 0; i < grp_refs.count && ok; i++) {
                IconRef r = grp_refs.items[i];
                if (r.id == 1 && r.lang == lang) continue;
                if (!UpdateResourceW(hu, (LPCWSTR)RT_GROUP_ICON, MAKEINTRESOURCEW(r.id),
                                     r.lang, NULL, 0)) ok = 0;
            }

            /* ⑤ 写新图像：id = 1..N */
            for (int i = 0; i < img_count && ok; i++) {
                if (!UpdateResourceW(hu, (LPCWSTR)RT_ICON, MAKEINTRESOURCEW((WORD)(i + 1)), lang,
                                     (LPVOID)(ico + imgs[i].offset), (DWORD)imgs[i].bytes)) {
                    ok = 0;
                }
            }

            /* ⑥ 写图标组（GRPICONDIR = 头 6 字节 + N × 14 字节；
             *    与 ICONDIRENTRY 的差别：最后 4 字节的 imageOffset 换成 2 字节的 nID） */
            if (ok) {
                size_t grp_size = 6 + 14 * (size_t)img_count;
                unsigned char* grp = (unsigned char*)calloc(1, grp_size);
                if (!grp) {
                    ok = 0;
                    snprintf(why, sizeof(why), "内存不足");
                } else {
                    grp[0] = 0; grp[1] = 0;                    /* reserved */
                    grp[2] = 1; grp[3] = 0;                    /* type = 1（图标） */
                    grp[4] = (unsigned char)(img_count & 0xFF);/* count */
                    grp[5] = (unsigned char)((img_count >> 8) & 0xFF);
                    for (int i = 0; i < img_count; i++) {
                        unsigned char* e = grp + 6 + (size_t)i * 14;
                        e[0] = imgs[i].width;
                        e[1] = imgs[i].height;
                        e[2] = imgs[i].color_count;
                        e[3] = 0;                              /* reserved */
                        e[4] = (unsigned char)(imgs[i].planes & 0xFF);
                        e[5] = (unsigned char)(imgs[i].planes >> 8);
                        e[6] = (unsigned char)(imgs[i].bit_count & 0xFF);
                        e[7] = (unsigned char)(imgs[i].bit_count >> 8);
                        e[8]  = (unsigned char)(imgs[i].bytes & 0xFF);
                        e[9]  = (unsigned char)((imgs[i].bytes >> 8) & 0xFF);
                        e[10] = (unsigned char)((imgs[i].bytes >> 16) & 0xFF);
                        e[11] = (unsigned char)((imgs[i].bytes >> 24) & 0xFF);
                        e[12] = (unsigned char)((i + 1) & 0xFF);   /* nID = 对应 RT_ICON 的 id */
                        e[13] = 0;
                    }
                    if (!UpdateResourceW(hu, (LPCWSTR)RT_GROUP_ICON, MAKEINTRESOURCEW(1), lang,
                                         grp, (DWORD)grp_size)) {
                        ok = 0;
                    }
                    free(grp);
                }
            }

            /* ⑦ 提交（fDiscard = FALSE：真写盘） */
            if (!EndUpdateResourceW(hu, !ok)) {
                if (ok) snprintf(why, sizeof(why), "EndUpdateResource 失败 (err=%lu)",
                                 (unsigned long)GetLastError());
                ok = 0;
            }

            /* ⑧ 读回改好的 PE 映像 */
            if (ok) {
                if (icon_read_file(wtmp, out, out_size) != 0) {
                    snprintf(why, sizeof(why), "回读临时文件失败");
                } else {
                    rc = 0;
                }
            }
        }
    }

    _wremove(wtmp);
    free(ico);

    if (rc != 0) {
        if (out && *out) { free(*out); *out = NULL; }
        if (out_size) *out_size = 0;
        if (err && err_size) {
            snprintf(err, err_size, "替换图标失败: %s", why[0] ? why : "未知原因");
        }
    }
    return rc;
}

#endif /* _WIN32 */
