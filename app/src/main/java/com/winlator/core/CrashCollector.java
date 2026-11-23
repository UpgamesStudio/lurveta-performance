package com.winlator.core;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Environment;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * LURVETA: when the app process itself dies during a game (native crash, out of memory kill...),
 * nothing in the launch report can say why, because the report is written by that process.
 * On the next start this collects what is left: the Vortek crash handler's native-crash.txt and
 * Android's own record of the exit (ApplicationExitInfo, with the tombstone on Android 12+), and
 * appends it to the last launch report in Download/LURVETA/logs.
 */
public final class CrashCollector {
    private static final String TAG = "CrashCollector";
    private static final String PREFS = "lurveta_crash_collector";
    private static final String KEY_LAST_EXIT = "last_exit_timestamp";

    private CrashCollector() {}

    /** Returns a short notice for the user when the previous run ended abnormally, else null. */
    public static String collect(Context context) {
        try {
            return collectInternal(context);
        }
        catch (Throwable e) {
            Log.w(TAG, "unable to collect the previous exit", e);
            return null;
        }
    }

    private static String collectInternal(Context context) {
        StringBuilder sb = new StringBuilder();
        String notice = null;
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File publicDir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "LURVETA/logs");

        File nativeCrash = new File(context.getFilesDir(), "logs/native-crash.txt");
        if (nativeCrash.isFile() && nativeCrash.length() > 0) {
            sb.append("\n--- NATIVE CRASH OF THE APP PROCESS (recorded by the LURVETA crash handler) ---\n");
            sb.append(readText(nativeCrash, 64 * 1024)).append('\n');
            nativeCrash.delete();
            notice = "La app se cerró por un fallo interno (no fue el juego).";
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
            long lastSeen = prefs.getLong(KEY_LAST_EXIT, 0);
            ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            List<ApplicationExitInfo> exits = am != null ? am.getHistoricalProcessExitReasons(null, 0, 5) : new ArrayList<>();
            long newest = lastSeen;
            for (ApplicationExitInfo exit : exits) {
                if (exit.getTimestamp() <= lastSeen) continue;
                newest = Math.max(newest, exit.getTimestamp());
                int reason = exit.getReason();
                if (reason == ApplicationExitInfo.REASON_USER_REQUESTED || reason == ApplicationExitInfo.REASON_EXIT_SELF
                    || reason == ApplicationExitInfo.REASON_USER_STOPPED || reason == ApplicationExitInfo.REASON_PACKAGE_UPDATED) continue;
                // reason UNKNOWN without a description: Android recorded no cause (usually the session closed normally)
                if (reason == ApplicationExitInfo.REASON_UNKNOWN && (exit.getDescription() == null || exit.getDescription().isEmpty())) continue;
                sb.append("\n--- PREVIOUS APP EXIT (recorded by Android) ---\n");
                sb.append("when: ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(exit.getTimestamp())))
                  .append(" | process ").append(exit.getProcessName()).append(" pid ").append(exit.getPid()).append('\n');
                sb.append("reason: ").append(reasonName(reason)).append(" | status ").append(exit.getStatus())
                  .append(" | importance ").append(exit.getImportance()).append('\n');
                sb.append("memory at exit: pss ").append(exit.getPss() / 1024).append(" MB, rss ").append(exit.getRss() / 1024).append(" MB\n");
                if (exit.getDescription() != null) sb.append("description: ").append(exit.getDescription()).append('\n');
                if (notice == null) notice = "Android cerró la app: " + reasonName(reason) + ".";

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                    && (reason == ApplicationExitInfo.REASON_CRASH_NATIVE || reason == ApplicationExitInfo.REASON_ANR)) {
                    byte[] trace = readTrace(exit);
                    if (trace != null && trace.length > 0) {
                        File raw = new File(publicDir, (reason == ApplicationExitInfo.REASON_ANR ? "anr-" : "tombstone-") + stamp + (reason == ApplicationExitInfo.REASON_ANR ? ".txt" : ".pb"));
                        writeBytes(raw, trace);
                        sb.append("full trace saved: Download/LURVETA/logs/").append(raw.getName()).append('\n');
                        sb.append("trace excerpt (readable strings):\n");
                        for (String line : readableStrings(trace, reason == ApplicationExitInfo.REASON_ANR ? 200 : 160)) sb.append("  ").append(line).append('\n');
                    }
                }
            }
            prefs.edit().putLong(KEY_LAST_EXIT, newest).apply();
        }

        if (sb.length() == 0) return null;

        String text = sb.toString();
        appendToNewestReport(publicDir, text);
        appendTo(new File(context.getFilesDir(), "logs/launch/launch-latest.txt"), text);
        writeBytes(new File(publicDir, "crash-" + stamp + ".txt"), text.getBytes(StandardCharsets.UTF_8));
        return (notice != null ? notice : "La sesión anterior terminó de forma anormal.")
            + "\n\nDetalles añadidos al informe: Download/LURVETA/logs/launch-latest.txt\ny en Download/LURVETA/logs/crash-" + stamp + ".txt";
    }

    private static String reasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE (fallo nativo)";
            case ApplicationExitInfo.REASON_CRASH: return "CRASH (excepción Java)";
            case ApplicationExitInfo.REASON_ANR: return "ANR (la app dejó de responder)";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY (memoria insuficiente)";
            case ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED (matada por una señal)";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE_USAGE";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "INITIALIZATION_FAILURE";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "PERMISSION_CHANGE";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "DEPENDENCY_DIED";
            case ApplicationExitInfo.REASON_OTHER: return "OTHER";
            default: return "reason " + reason;
        }
    }

    private static byte[] readTrace(ApplicationExitInfo exit) {
        try (InputStream in = exit.getTraceInputStream()) {
            if (in == null) return null;
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            int n;
            while ((n = in.read(buffer)) > 0 && out.size() < 4 * 1024 * 1024) out.write(buffer, 0, n);
            return out.toByteArray();
        }
        catch (IOException e) {
            return null;
        }
    }

    /** Printable runs of the (protobuf) tombstone: abort message, thread names, frames' library and function names. */
    static List<String> readableStrings(byte[] data, int maxLines) {
        List<String> lines = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        for (int i = 0; i <= data.length && lines.size() < maxLines; i++) {
            int c = i < data.length ? data[i] & 0xff : 0;
            if (c >= 0x20 && c < 0x7f) current.append((char) c);
            else {
                if (current.length() >= 6) lines.add(current.toString());
                current.setLength(0);
                if (c == '\n' && i < data.length) continue;
            }
        }
        return lines;
    }

    private static void appendToNewestReport(File publicDir, String text) {
        appendTo(new File(publicDir, "launch-latest.txt"), text);
        File[] reports = publicDir.listFiles((d, n) -> n.startsWith("launch-2") && n.endsWith(".txt"));
        if (reports == null || reports.length == 0) return;
        File newest = reports[0];
        for (File report : reports) if (report.lastModified() > newest.lastModified()) newest = report;
        appendTo(newest, text);
    }

    private static void appendTo(File file, String text) {
        if (!file.isFile()) return;
        try (FileWriter writer = new FileWriter(file, true)) {
            writer.write(text);
        }
        catch (IOException e) {
            Log.w(TAG, "unable to append to " + file, e);
        }
    }

    private static void writeBytes(File file, byte[] data) {
        try {
            File parent = file.getParentFile();
            if (parent != null && !parent.isDirectory()) parent.mkdirs();
            try (FileOutputStream out = new FileOutputStream(file)) {
                out.write(data);
            }
        }
        catch (IOException e) {
            Log.w(TAG, "unable to write " + file, e);
        }
    }

    private static String readText(File file, int maxBytes) {
        try {
            byte[] data = Files.readAllBytes(file.toPath());
            int start = Math.max(0, data.length - maxBytes);
            return new String(data, start, data.length - start, StandardCharsets.UTF_8).trim();
        }
        catch (IOException e) {
            return "(unreadable: " + e + ")";
        }
    }
}
