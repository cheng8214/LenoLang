package com.leno.app;

import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * SDL3 形态的宿主 Activity：继承 SDL 官方的 {@code org.libsdl.app.SDLActivity}。
 *
 * <p>为什么必须是继承，而不是自己写一个普通 Activity：
 * SDL3 的 Android 视频/音频/输入驱动不是纯 native 的 ——
 * libSDL3.so 在自己的 {@code JNI_OnLoad} 里
 * {@code FindClass("org/libsdl/app/SDLActivity")} 并 {@code RegisterNatives}，
 * 之后由 Java 侧的 {@code SDLSurface} 提供 Surface、把触摸与生命周期事件转发给 native。
 *
 * <p>之前（TextView 宿主）的失败原因也在这里：libSDL3.so 是由 Leno 的
 * {@code ffi.load} 用 {@code dlopen} 拉起来的，而 <b>dlopen 不触发 JNI_OnLoad</b>
 * ⇒ SDL 内部的 {@code mJavaVM} 永远为空 ⇒ {@code SDL_Init(VIDEO)} 当场返回
 * "Failed, there is no JavaVM"（见 src/core/android/SDL_android.c:491）。
 * 手动补调它的 JNI_OnLoad 更糟：找不到 SDLActivity 类会抛 pending exception，
 * ART 直接 abort 整个进程（有 tombstone）。
 *
 * <p>现在 SDL3 由 {@code System.loadLibrary("SDL3")} 加载（JNI_OnLoad 正常执行、
 * JavaVM 缓存到位），Leno 的 VM 再 {@code ffi.load} 同一个 .so 时拿到的就是
 * 同一个已初始化的实例 —— 因为 dlopen 同一路径只会增加引用计数，不会重新初始化。
 *
 * <p>进程模型：
 * <pre>
 *   SDLActivity.onCreate  → loadLibraries()：SDL3 / SDL3_image / SDL3_ttf / leno
 *   SDLActivity.onStart   → 起 SDLThread，调我们的 main()
 *   main()                → setDataDir()，再 nativeRunMain("libleno.so", "leno_sdl_main")
 *   leno_sdl_main()       → 在这个 SDL 主线程上跑 .lenb（VM 单线程执行）
 * </pre>
 * 关键点：VM 必须跑在 SDL 的"主线程"上（SDL_RunApp 认定的那个），
 * 否则 SDL_Init(VIDEO) 会因为不是主线程而失败。
 */
public class SdlActivity extends org.libsdl.app.SDLActivity {
    private static final String TAG = "leno_sdl";

    /** 由 main() 传给 leno_sdl_main 的 argv[1]。 */
    private static String sLenbPath = null;

    /** 把可写目录交给 VM（否则 Android 上 _tmpdir() 会退到不存在的 /tmp）。 */
    static native void setDataDir(String dir);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // 先把 .lenb 落到应用私有目录：assets 里的条目 native 侧没有 fd 可读，
        // 而 VM 的入口只接受文件路径。
        String asset = getIntent().getStringExtra("asset");
        if (asset == null) asset = "app_sdl.lenb";

        // 调试后门：--es path /data/local/tmp/x.lenb（adb push 后免重装 APK 迭代）
        String abs = getIntent().getStringExtra("path");
        if (abs != null) {
            sLenbPath = abs;
            Log.i(TAG, "使用设备上的字节码: " + abs);
        } else {
            File dst = new File(getFilesDir(), "sdl_app.lenb");
            sLenbPath = dst.getAbsolutePath();
            try {
                copyAsset(asset, dst);
                Log.i(TAG, "已展开 " + asset + " → " + sLenbPath + " (" + dst.length() + " 字节)");
            } catch (IOException e) {
                Log.e(TAG, "展开 asset 失败: " + asset, e);
            }
        }

        super.onCreate(savedInstanceState);
    }

    /**
     * 加载顺序有讲究：SDL3 先（image/ttf 依赖它），leno 最后
     * —— 因为 getMainSharedObject() 取的是数组里<b>最后一个</b>库来 dlsym 入口函数。
     */
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "SDL3_image", "SDL3_ttf", "leno" };
    }

    @Override
    protected String getMainFunction() {
        return "leno_sdl_main";
    }

    @Override
    protected String[] getArguments() {
        return new String[] { sLenbPath == null ? "" : sLenbPath };
    }

    @Override
    protected void main() {
        // 在 SDLThread 上、nativeRunMain 之前调用：此时 libleno.so 已由
        // loadLibraries() 加载，JNI 符号可解析，且 VM 还没开始跑字节码。
        setDataDir(getFilesDir().getAbsolutePath());
        super.main();
    }

    private void copyAsset(String name, File dst) throws IOException {
        try (InputStream in = getAssets().open(name);
             FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[16 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }
}
