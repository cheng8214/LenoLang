package com.leno.app;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.View;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * 最小可运行宿主：把 assets 里的 .lenb 解到 filesDir，交给 libleno.so 里的 VM 执行，
 * 把 stdout 实时刷到屏幕上。
 *
 * 这个类刻意只做"宿主"该做的事（生命周期 / 资源解包 / 线程 / UI），
 * 不掺任何业务逻辑 —— 业务全部在 .leno 里，这正是 Lua/Python 在安卓上
 * 唯一被验证成功的分工方式。
 */
public class MainActivity extends Activity {

    static {
        System.loadLibrary("leno");
    }

    /** 在后台线程里同步跑字节码，返回 VM 退出码。 */
    public native int runLenb(String absPath);

    /** arm64-v8a / 指针宽度 / 字节码版本，用于确认 ABI 与产物版本对得上。 */
    public native String buildInfo();

    /**
     * 把本 app 的私有可写目录交给 VM（`_tmpdir()` / 脚本目录都靠它）。
     * 没有这一步，脚本里的 `_tmpdir()` 会退到 "/tmp" —— 沙箱里不可写，落盘全失败。
     */
    public native void setDataDir(String absDir);

    private TextView out;
    private final StringBuilder log = new StringBuilder();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.parseColor("#0B1020"));

        out = new TextView(this);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        out.setTextColor(Color.parseColor("#3DDC84"));
        out.setBackgroundColor(Color.parseColor("#0B1020"));
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        out.setPadding(pad, pad, pad, pad);
        out.setTextIsSelectable(true);

        scroll.addView(out);
        setContentView(scroll);
        getWindow().setStatusBarColor(Color.parseColor("#0B1020"));

        // 必须在任何脚本执行之前注入：脚本里第一次 `_tmpdir()` 就依赖它
        setDataDir(getFilesDir().getAbsolutePath());

        final String asset = getIntent().getStringExtra("asset");
        // 调试用：直接跑设备上已有的 .lenb（adb push 到 /data/local/tmp 后免重装 APK 迭代）
        final String absPath = getIntent().getStringExtra("path");
        new Thread(() -> {
            if (absPath != null) {
                run(new File(absPath), absPath);
            } else {
                run(new File(getFilesDir(), "app.lenb"),
                        asset == null ? "hello.lenb" : asset);
            }
        }, "leno-vm").start();
    }

    private void run(File target, String assetName) {
        appendLine("Leno on Android");
        appendLine(buildInfo());
        appendLine("");
        try {
            if (assetName.startsWith("/")) {
                appendLine("[host] 直接运行 " + target + " (" + target.length() + " bytes)");
            } else {
                copyAsset(assetName, target);
                appendLine("[host] " + assetName + " -> " + target.getName()
                        + " (" + target.length() + " bytes)");
            }
            appendLine("[host] 进入 VM ...");
            appendLine("");

            int rc = runLenb(target.getAbsolutePath());

            appendLine("");
            appendLine("[host] VM 退出码 = " + rc);
        } catch (Throwable t) {
            appendLine("[host] 失败: " + t);
            for (StackTraceElement e : t.getStackTrace()) {
                appendLine("    at " + e);
            }
        }
    }

    /** 由 native reader 线程调用（每次一整行合法 UTF-8）。 */
    void onOutput(byte[] data) {
        final String text = new String(data, StandardCharsets.UTF_8);
        runOnUiThread(() -> {
            log.append(text);
            out.setText(log);
            // 跟随滚动到底部
            if (out.getParent() instanceof ScrollView) {
                final ScrollView sv = (ScrollView) out.getParent();
                sv.post(() -> sv.fullScroll(View.FOCUS_DOWN));
            }
        });
    }

    private void appendLine(String s) {
        final String text = s + "\n";
        runOnUiThread(() -> {
            log.append(text);
            out.setText(log);
        });
    }

    private void copyAsset(String name, File dst) throws IOException {
        try (InputStream in = getAssets().open(name);
             OutputStream os = new FileOutputStream(dst)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        }
    }
}
