// ============================================================================
// Leno Android JNI 胶水
// ----------------------------------------------------------------------------
// 职责只有三件（刻意保持薄，理由见 docs 的讨论：桥比 VM 值钱）：
//   1. JNI_OnLoad 存下 JavaVM ⇒ 后续任何 native 线程都能 Attach
//   2. 把 stdout 接到匿名管道，后台线程收字节流回传给 Java
//      （Leno 的 print 走 fwrite(stdout) + fflush，见 src/module/io/io.c）
//   3. 调 `lenolang_run_lenb()` 跑字节码
//
// 为什么用 jbyteArray 而不是 NewStringUTF：NewStringUTF 吃的是 **modified UTF-8**，
//   对 4 字节序列（emoji / 生僻字）解码是错的；而且 pipe 读到的块可能在多字节字符
//   中间截断。改传原始字节 + Java 侧 StandardCharsets.UTF_8 解码，两个问题一起没了。
//
// 线程模型：runLenb 在 Java 的后台线程里同步跑（VM 是单线程执行模型）；
//   输出回传走独立 reader 线程 ⇒ 必须 AttachCurrentThread（JNI 不 attach 会崩）。
// ============================================================================

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "leno_jni", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "leno_jni", __VA_ARGS__)

#include "leno_vm_runtime.h"
#include "leno_error.h"
#include "leno_serialize.h"   // LENO_BIN_VERSION（buildInfo 里报出来，排查"字节码版本不符"用）

static JavaVM* g_vm = NULL;
static jobject g_sink = NULL;          // MainActivity 的全局引用
static jmethodID g_on_output = NULL;   // void onOutput(byte[])
static int g_ready = 0;                // stdout 是否已接管（只接管一次）
static int g_pipe_w = -1;              // 管道写端（fd 1 被 dup2 到它）
static int g_saved_stdout = -1;        // 原来的 fd 1，收尾时还回去
static int g_saved_stderr = -1;        // 原来的 fd 2

// ---------------------------------------------------------------------------
// 行缓冲：只有遇到 '\n' 才回调 Java。
// 目的：pipe 的一次 read 很可能切在多字节 UTF-8 字符中间，整块直接解码会花屏。
//   攒到换行为止再送 ⇒ 一次回调永远是一整行合法 UTF-8（末尾残留由 flush 兜底）。
// ---------------------------------------------------------------------------
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} OutBuf;

static OutBuf g_out = { NULL, 0, 0 };

static int outbuf_push(OutBuf* b, const char* p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 4096);
        while (cap < b->len + n + 1) cap *= 2;
        char* nd = (char*)realloc(b->data, cap);
        if (!nd) return 0;
        b->data = nd;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

// 逐行镜像到 logcat。
// 为什么需要：自动化的能力探针一次输出几十行，靠截图 OCR 认字既慢又会错；
//   镜像之后 `adb logcat -s leno_jni` 就能直接 grep 结果。
// 为什么逐行而不是整块：logcat 单条上限约 4KB，且含 '\n' 的条目在 `-v brief` 下会被拆得难看。
static void mirror_to_logcat(const char* p, size_t cut) {
    size_t start = 0;
    while (start < cut) {
        size_t end = start;
        while (end < cut && p[end] != '\n') end++;
        size_t n = end - start;
        if (n > 0) {
            // 单行过长时截断：避免被 logcat 的上限悄悄截掉尾部，看不出是"截断"还是"就这么多"
            if (n > 900) { LOGI("%.*s …(truncated)", 900, p + start); }
            else         { LOGI("%.*s", (int)n, p + start); }
        }
        start = (end < cut) ? end + 1 : cut;
    }
}

// 把 buf 里到最后一个 '\n' 为止的内容交给 Java，剩余部分留在缓冲里
static void outbuf_emit_lines(JNIEnv* env, OutBuf* b, int force) {
    if (!b->len) return;
    size_t cut = b->len;
    if (!force) {
        size_t i = b->len;
        while (i > 0 && b->data[i - 1] != '\n') i--;
        if (i == 0) return;          // 还没有完整行 ⇒ 等
        cut = i;
    }
    mirror_to_logcat(b->data, cut);

    // SDL 形态的宿主没有 TextView（界面是 SDLSurface）⇒ g_sink 为空，
    // 输出只走 logcat。这里必须容忍空 sink，否则整条 print 链路会段错误。
    if (g_sink && g_on_output) {
        jbyteArray arr = (*env)->NewByteArray(env, (jsize)cut);
        if (!arr) return;
        (*env)->SetByteArrayRegion(env, arr, 0, (jsize)cut, (const jbyte*)b->data);
        (*env)->CallVoidMethod(env, g_sink, g_on_output, arr);
        (*env)->DeleteLocalRef(env, arr);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    // 去掉已发出的部分；缓冲区缩到 0 但保留已分配的堆（下一行直接复用）
    memmove(b->data, b->data + cut, b->len - cut);
    b->len -= cut;
    b->data[b->len] = '\0';
}

static void* reader_main(void* arg) {
    int fd = (int)(intptr_t)arg;
    char chunk[2048];
    JNIEnv* env = NULL;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;

    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;                     // 写端全关 ⇒ EOF
        if (!outbuf_push(&g_out, chunk, (size_t)n)) break;
        outbuf_emit_lines(env, &g_out, 0);
    }
    outbuf_emit_lines(env, &g_out, 1);          // 收尾：没有换行的尾巴也要送出去
    close(fd);
    (*g_vm)->DetachCurrentThread(g_vm);
    return NULL;
}

// 接管 fd 1。必须在第一次跑字节码**之前**调用，且只调一次。
// sink 允许为 NULL（SDL 宿主没有 TextView）⇒ 只镜像 logcat。
static int stdout_hook_install(JNIEnv* env, jobject sink) {
    if (g_ready) return 1;

    if (sink && env) {
        g_sink = (*env)->NewGlobalRef(env, sink);
        if (!g_sink) return 0;

        jclass cls = (*env)->GetObjectClass(env, sink);
        g_on_output = (*env)->GetMethodID(env, cls, "onOutput", "([B)V");
        (*env)->DeleteLocalRef(env, cls);
        if (!g_on_output) return 0;
    } else {
        g_sink = NULL;
        g_on_output = NULL;
    }

    int fds[2];
    if (pipe(fds) != 0) { LOGE("pipe 失败 errno=%d", errno); return 0; }

    g_saved_stdout = dup(1);
    g_saved_stderr = dup(2);
    g_pipe_w = fds[1];

    // Leno 的 print 自己会 fflush；这里再把 stdout 设成无缓冲，保证顺序与实时性
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    fflush(stdout);
    fflush(stderr);

    // ⚠ 必须把**写端**（fds[1]）dup 到 fd 1：这里一开始写成 fds[0]，
    //   结果是 fd 1 变成读端 ⇒ printf 写自己 ⇒ EBADF 静默失败，
    //   reader 永远读到 0 字节（屏上只有 host 那几行，VM 的 print 全丢）。
    // fd 2 也要接：Leno 的运行期错误全走 stderr（`fprintf(stderr, "[错误]…")`），
    //   而 Android 的 app 进程**不会**把 native stderr 送到 logcat ⇒ 不接就等于吞掉所有报错。
    if (dup2(fds[1], 1) < 0 || dup2(fds[1], 2) < 0) {
        LOGE("dup2 失败 errno=%d", errno);
        return 0;
    }

    pthread_t th;
    if (pthread_create(&th, NULL, reader_main, (void*)(intptr_t)fds[0]) != 0) {
        LOGE("pthread_create 失败");
        return 0;
    }
    pthread_detach(th);

    g_ready = 1;
    return 1;
}

static void stdout_hook_restore(void) {
    if (!g_ready) return;
    fflush(stdout);
    fflush(stderr);
    if (g_saved_stdout >= 0) {
        dup2(g_saved_stdout, 1);
        close(g_saved_stdout);
        g_saved_stdout = -1;
    }
    if (g_saved_stderr >= 0) {
        dup2(g_saved_stderr, 2);
        close(g_saved_stderr);
        g_saved_stderr = -1;
    }
    if (g_pipe_w >= 0) {
        close(g_pipe_w);          // 写端关闭 ⇒ reader 收到 EOF 后自行退出
        g_pipe_w = -1;
    }
    g_ready = 0;
}

// ---------------------------------------------------------------------------
// 【历史坑，留档免得下次又踩】SDL3 的 JavaVM 是靠它自己的 JNI_OnLoad 缓存的，
// 而 dlopen 不会触发 JNI_OnLoad ⇒ SDL 内部报 "Failed, there is no JavaVM"，
// Android 上 SDL_Init 直接失败。
//
// 试过在这里 dlopen("libSDL3.so") 后手动调它的 JNI_OnLoad(vm, NULL)：
// **会让进程直接 abort** —— SDL 的 JNI_OnLoad 里会 FindClass("org/libsdl/app/SDLActivity")
// 并 RegisterNatives，宿主里没有这个类 ⇒ 抛 pending exception，ART 于是
// `art::Thread::AssertNoPendingException` 直接杀进程（实测 tombstone 栈：
//   ClassLinker::FindClass → Thread::Abort）。
//
// ★ 2026-11-19 已解决：改成 SDL 官方形态 —— 见 com.leno.app.SdlActivity
//   （继承 org.libsdl.app.SDLActivity，SDL3 由 System.loadLibrary 加载 ⇒
//    JNI_OnLoad 正常跑，JavaVM 缓存到位），入口就是本文件末尾的 leno_sdl_main()。
// ---------------------------------------------------------------------------

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

// ---------------------------------------------------------------------------
// void setDataDir(String dir)
//   把 app 的可写目录（getFilesDir()）交给 VM。
//   为什么必须显式给：Android 沙箱里 `_tmpdir()` 取不到 TMPDIR，原来会退到 "/tmp"
//   —— 那个路径既不存在也不可写，任何落盘都会失败，而报错点离原因很远。
// ---------------------------------------------------------------------------
JNIEXPORT void JNICALL
Java_com_leno_app_MainActivity_setDataDir(JNIEnv* env, jobject thiz, jstring jdir) {
    (void)thiz;
    if (!jdir) return;
    const char* dir = (*env)->GetStringUTFChars(env, jdir, NULL);
    if (dir) {
        leno_set_data_dir(dir);
        LOGI("dataDir = %s", dir);
        (*env)->ReleaseStringUTFChars(env, jdir, dir);
    }
}

// ---------------------------------------------------------------------------
// int runLenb(String absPath)
//   返回 VM 的退出码：0 = 正常；非 0 = 脚本/VM 报错（与桌面 `leno_vm app.lenb` 同口径）
// ---------------------------------------------------------------------------
JNIEXPORT jint JNICALL
Java_com_leno_app_MainActivity_runLenb(JNIEnv* env, jobject thiz, jstring jpath) {
    if (!g_vm) (*env)->GetJavaVM(env, &g_vm);
    if (!stdout_hook_install(env, thiz)) {
        return -900;   // 挂钩失败（pipe/线程创建失败）—— 明确区别于 VM 的退出码
    }

    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return -901;

    LOGI("runLenb 开始: %s", path);
    leno_error_init();                       // 幂等；Android 上 fd1/fd2 都有效 ⇒ 不会抢重定向
    int rc = lenolang_run_lenb(path);        // ← 真正的入口（src/vm_main.c 导出）
    LOGI("VM 返回 %d", rc);

    (*env)->ReleaseStringUTFChars(env, jpath, path);
    stdout_hook_restore();
    return (jint)rc;
}

// 供 Java 侧确认 .so 真的加载成功、以及跑的是哪套 ABI
JNIEXPORT jstring JNICALL
Java_com_leno_app_MainActivity_buildInfo(JNIEnv* env, jobject thiz) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "leno vm / android / %s / ptr=%d / LENO_BIN_VERSION=%d",
#if defined(__aarch64__)
             "arm64-v8a",
#elif defined(__arm__)
             "armeabi-v7a",
#elif defined(__x86_64__)
             "x86_64",
#else
             "unknown",
#endif
             (int)(sizeof(void*) * 8),
             LENO_BIN_VERSION);
    return (*env)->NewStringUTF(env, buf);
}

// ---------------------------------------------------------------------------
// SDL 形态宿主（com.leno.app.SdlActivity）用的同一份 setDataDir
// ---------------------------------------------------------------------------
JNIEXPORT void JNICALL
Java_com_leno_app_SdlActivity_setDataDir(JNIEnv* env, jclass cls, jstring jdir) {
    (void)cls;
    if (!jdir) return;
    const char* dir = (*env)->GetStringUTFChars(env, jdir, NULL);
    if (dir) {
        leno_set_data_dir(dir);
        LOGI("dataDir = %s", dir);
        (*env)->ReleaseStringUTFChars(env, jdir, dir);
    }
}

// ---------------------------------------------------------------------------
// int leno_sdl_main(int argc, char** argv)
//
//   SDL 官方宿主约定的入口：由 SDLActivity 的 SDLThread →
//   nativeRunMain("libleno.so", "leno_sdl_main", args) → SDL_RunApp(...) 调进来。
//   **运行在 SDL 认定的主线程上**，这是 SDL_Init(VIDEO) 能成功的前提。
//
//   argv[0] 是 SDL 固定塞的 "app_process"，argv[1] 是 Java 侧 getArguments() 给的
//   .lenb 绝对路径。
// ---------------------------------------------------------------------------
__attribute__((visibility("default")))
int leno_sdl_main(int argc, char** argv) {
    if (!g_vm) {
        // 正常路径下 libleno.so 是被 System.loadLibrary("leno") 加载的，
        // JNI_OnLoad 已经存过 vm；这里兜底一次，避免被别的加载方式拉起来时崩。
        LOGE("leno_sdl_main: 没有 JavaVM（libleno.so 不是被 System.loadLibrary 加载的？）");
    }

    const char* path = (argc > 1) ? argv[1] : NULL;
    if (!path || path[0] == '\0') {
        LOGE("leno_sdl_main: 缺少 .lenb 路径（argv 里只有一个元素）");
        return -901;
    }

    // SDL 宿主没有 TextView ⇒ sink 传 NULL，输出只走 logcat
    stdout_hook_install(NULL, NULL);

    LOGI("leno_sdl_main: %s（主线程）", path);
    leno_error_init();
    int rc = lenolang_run_lenb(path);
    LOGI("VM 返回 %d", rc);

    stdout_hook_restore();
    return rc;
}
