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

    /**
     * 把真实屏幕密度（densityDpi/160）交给框架。
     *
     * <p>为什么非得由 Java 给：SDL 3.4.20 的 Android 后端既没实现
     * {@code SDL_GetWindowDisplayScale}（恒 1.0），也没给
     * {@code SDL_DisplayMode.pixel_density} 赋值（恒 1.0）。实测 AVD
     * （720x1280 / 420dpi）两者都是 1.0，而窗口高度 1217 又确实是物理像素
     * ⇒ 框架拿 1.0 就等于没做任何适配（13px 字只有 5dp，手指点不中）。
     * {@code getDisplayMetrics().density} 才是那个 2.625。
     */
    static native void setAndroidDensity(float density);

    /**
     * 往 SDL 事件队列投一条 <b>AC_BACK</b> 键事件（scancode 282）。
     *
     * <p>为什么要自己投：{@code SDL_HINT_ANDROID_TRAP_BACK_BUTTON} 只是让
     * {@code SDLActivity.onBackPressed()} 不调 {@code super}（不 finish Activity），
     * 它<b>不会</b>产生任何 SDL 事件 —— 只设 hint 的话按返回键毫无反应。
     * 所以由这里补一条真事件，交给框架做"分层返回"（关弹层 → 关窗口 → 退出）。
     */
    static native void injectBackKey();

    /**
     * 返回键：<b>不</b>退出 Activity，改为交给 VM 处理。
     *
     * <p>原来的行为（不重写）：系统直接 {@code finish()} ⇒ "设置弹窗开着按返回，
     * 整个程序没了、状态全丢"。现在 VM 里 {@code run()} 会分层处理：
     * 先让下拉框/对话框消费，再关最上层的虚拟弹窗，最后才关窗口。
     * 用户想真正退出时，VM 会把主窗口关掉 ⇒ {@code SDL_main} 返回 ⇒ Activity 自然结束。
     */
    @Override
    public void onBackPressed() {
        injectBackKey();
        // 刻意不调 super.onBackPressed()：那一句就是 finish()。
        // （trapBack hint 已在 core.init 里设为 1，这里再显式拦一层，双保险 ✓）
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // 先把 .lenb 落到应用私有目录：assets 里的条目 native 侧没有 fd 可读，
        // 而 VM 的入口只接受文件路径。
        String dir = getIntent().getStringExtra("assetsdir");
        if (dir != null && dir.isEmpty()) dir = null;
        String asset = getIntent().getStringExtra("asset");
        if (asset == null) asset = "app_sdl.lenb";
        // assetsdir 给定时，asset 按"该目录下的相对路径"解释（lmusic + musicdl_gui.lenb）
        if (dir != null && !asset.startsWith(dir + "/")) asset = dir + "/" + asset;

        // 整棵资源树先落地：assets/<dir>/** → filesDir/<dir>/**
        // 为什么需要：VM 的 dirs.res_dir() 在未打包态 = 字节码所在目录，应用里
        // dirs.join(res_dir(), "images/x.png") 就靠这条把图片放到 .lenb 旁边。
        // （此前 copyAsset 一次只拷一个名字 —— 带子目录/z 张图的界面没法用）
        if (dir != null) {
            File root = new File(getFilesDir(), dir);
            try {
                copyAssetTree(dir, root);
                Log.i(TAG, "已展开资源树 " + dir + " → " + root.getAbsolutePath());
            } catch (IOException e) {
                Log.e(TAG, "展开资源树失败: " + dir, e);
            }
        }

        // 调试后门：--es path /data/local/tmp/x.lenb（adb push 后免重装 APK 迭代）
        String abs = getIntent().getStringExtra("path");
        if (abs != null) {
            sLenbPath = abs;
            Log.i(TAG, "使用设备上的字节码: " + abs);
        } else {
            File dst = new File(getFilesDir(), asset);
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
        // 同一时机把密度推进去 —— 框架的第一个 Window 就要用（再晚就来不及了）
        setAndroidDensity(getResources().getDisplayMetrics().density);
        super.main();
    }

    /**
     * 递归把 assets 里的一个目录整棵拷到磁盘。
     *
     * <p>{@code AssetManager.list(path)} 对目录返回子项名字、对文件返回空数组，
     * 所以拿子项数就能区分"目录"和"文件"，不必事先知道清单。
     */
    private void copyAssetTree(String assetDir, File dstDir) throws IOException {
        String[] kids = getAssets().list(assetDir);
        if (kids == null) return;
        if (!dstDir.exists() && !dstDir.mkdirs()) {
            throw new IOException("mkdirs 失败: " + dstDir);
        }
        for (String k : kids) {
            String child = assetDir + "/" + k;
            String[] sub = getAssets().list(child);
            if (sub != null && sub.length > 0) {
                copyAssetTree(child, new File(dstDir, k));
            } else {
                try {
                    copyAsset(child, new File(dstDir, k));
                } catch (IOException e) {
                    Log.e(TAG, "跳过 " + child, e);
                }
            }
        }
    }

    private void copyAsset(String name, File dst) throws IOException {
        File p = dst.getParentFile();
        if (p != null && !p.exists() && !p.mkdirs()) {
            throw new IOException("mkdirs 失败: " + p);
        }
        try (InputStream in = getAssets().open(name);
             FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[16 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }
}
