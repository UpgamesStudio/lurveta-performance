package com.winlator.core;

import android.content.Context;
import android.os.Build;
import android.os.Environment;
import android.util.Log;

import androidx.preference.PreferenceManager;

import com.winlator.BuildConfig;

import java.io.File;
import java.io.FileOutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * LURVETA Performance — writes uncaught Java exceptions to a log file.
 *
 * Files: <app files>/logs/crash-*.txt and, when writable, Download/LURVETA/logs/crash-*.txt.
 * Costs nothing until a crash happens; chains to the previous default handler so Android still
 * shows its usual crash behavior. Native crashes (signals) are recorded by Android as tombstones
 * and are not captured here.
 */
public final class CrashLogger implements Thread.UncaughtExceptionHandler {
    private static final String TAG = "CrashLogger";
    private static volatile boolean installed = false;
    private final Context appContext;
    private final Thread.UncaughtExceptionHandler previous;

    private CrashLogger(Context context, Thread.UncaughtExceptionHandler previous) {
        this.appContext = context.getApplicationContext();
        this.previous = previous;
    }

    public static synchronized void install(Context context) {
        if (installed) return;
        Thread.setDefaultUncaughtExceptionHandler(new CrashLogger(context, Thread.getDefaultUncaughtExceptionHandler()));
        installed = true;
    }

    public static File getInternalLogDir(Context context) {
        return new File(context.getFilesDir(), "logs");
    }

    @Override
    public void uncaughtException(Thread thread, Throwable throwable) {
        try {
            String report = buildReport(thread, throwable);
            String name = "crash-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".txt";
            write(new File(getInternalLogDir(appContext), name), report);
            File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
            write(new File(downloads, "LURVETA/logs/" + name), report);
        }
        catch (Throwable t) {
            Log.e(TAG, "Unable to write crash report", t);
        }

        if (previous != null) previous.uncaughtException(thread, throwable);
    }

    private String buildReport(Thread thread, Throwable throwable) {
        StringWriter stack = new StringWriter();
        throwable.printStackTrace(new PrintWriter(stack));

        String gpu = "";
        try {
            gpu = PreferenceManager.getDefaultSharedPreferences(appContext).getString("gpu_renderer", "");
        }
        catch (Throwable ignored) {}

        StringBuilder sb = new StringBuilder();
        sb.append(BuildConfig.PRODUCT_NAME).append(' ').append(BuildConfig.VERSION_NAME)
          .append(" (").append(BuildConfig.VERSION_CODE).append(") — ").append(BuildConfig.UPSTREAM_BASE).append('\n');
        sb.append("Time: ").append(new Date()).append('\n');
        sb.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        sb.append("Android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            sb.append("SoC: ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        }
        sb.append("Hardware: ").append(Build.HARDWARE).append('\n');
        sb.append("GPU: ").append(gpu).append('\n');
        sb.append("Thread: ").append(thread.getName()).append("\n\n");
        sb.append(stack);
        return sb.toString();
    }

    private static void write(File file, String content) {
        try {
            File parent = file.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) return;
            try (FileOutputStream out = new FileOutputStream(file)) {
                out.write(content.getBytes(StandardCharsets.UTF_8));
            }
        }
        catch (Throwable ignored) {}
    }
}
