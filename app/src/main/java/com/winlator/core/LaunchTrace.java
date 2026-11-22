package com.winlator.core;

import android.content.Context;
import android.os.Build;
import android.os.Environment;
import android.os.Process;
import android.system.Os;
import android.util.Log;

import com.winlator.BuildConfig;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * LURVETA Performance — launch trace: records HOW FAR a game gets and WHY it stopped.
 *
 * Until now a game that failed at startup left no trace: Wine/Box64 output went to /dev/null, the
 * exit code was ignored, and the app silently returned to the menu. With the trace enabled
 * (preference "lurveta_launch_trace", on by default) every launch produces ONE report:
 *   - device, app and container description, the exact command and environment, file checks;
 *   - a timeline of the guest processes (box64, wine, wineserver, explorer, the game);
 *   - the Vulkan/Vortek stages (connection, instance, device, first frame) and failed Vulkan calls;
 *   - the exit code / signal, time to exit, last stage reached and a heuristic verdict;
 *   - the tail of the Box64/Wine output, DXVK logs, Vortek errors.
 *
 * Files: files/logs/launch/launch-latest.txt (+ guest-output.log, previous runs rotated) and a copy in
 * Download/LURVETA/logs/ so it can be collected without root. Overhead: one /proc scan per second
 * on a background thread during the first minutes, then every 10 s; Wine/Box64 output is redirected
 * to a file by the OS (no extra thread). The verdict is a heuristic; the raw sections are the evidence.
 */
public final class LaunchTrace {
    private static final String TAG = "LaunchTrace";
    public static final String PREF_ENABLED = "lurveta_launch_trace";
    private static final int KEEP_RUNS = 5;
    private static final long MAX_OUTPUT_BYTES = 24L << 20;
    private static final int OUTPUT_TAIL_LINES = 150;
    private static final String VORTEK_MARK = " STAGE ";

    private static volatile LaunchTrace current;

    private final Context context;
    private final File dir;
    private final File outputFile;
    private final String title;
    private final String gameExe;                 // lower case, may be empty
    private final List<String> header = new ArrayList<>();
    private final List<String> timeline = new ArrayList<>();
    private final boolean[] reached = new boolean[LaunchAnalysis.stageCount()];
    private final long startNanos = System.nanoTime();
    private final long startEpochSeconds = System.currentTimeMillis() / 1000;
    private final int uid = Process.myUid();
    private final Map<Integer, String> lastProcesses = new HashMap<>();
    private final Map<Integer, String> everSeen = new TreeMap<>();

    private volatile boolean finished = false;
    private volatile boolean exited = false;
    private volatile int exitStatus = 0;
    private volatile long exitElapsedMs = 0;
    private volatile String execError = null;
    private volatile int guestPid = -1;
    private Thread sampler;
    private long lastFlushNanos = 0;

    private LaunchTrace(Context context, String title, String gameExe) {
        this.context = context.getApplicationContext();
        this.title = title;
        this.gameExe = gameExe == null ? "" : gameExe.toLowerCase(Locale.ROOT);
        this.dir = new File(this.context.getFilesDir(), "logs/launch");
        this.outputFile = new File(dir, "guest-output.log");
    }

    public static boolean isEnabled(android.content.SharedPreferences preferences) {
        return preferences.getBoolean(PREF_ENABLED, true);
    }

    /** Starts a trace (replaces any previous one). Never throws. */
    public static LaunchTrace begin(Context context, String title, String gameExe, List<String> description) {
        try {
            LaunchTrace trace = new LaunchTrace(context, title, gameExe);
            trace.rotate();
            trace.header.add("LURVETA Performance " + BuildConfig.VERSION_NAME + " (code " + BuildConfig.VERSION_CODE + ") | build " + (BuildConfig.DEBUG ? "debug" : "release"));
            trace.header.add("device: " + Build.MANUFACTURER + " " + Build.MODEL + " | Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ") | SoC " + HardwareInfo.getSocName());
            trace.header.add("cpu: " + HardwareInfo.getCpuSummary());
            try {
                HardwareInfo.Gpu gpu = HardwareInfo.getGpu(trace.context);
                trace.header.add("gpu: " + gpu.deviceName + " | driver " + gpu.driverName + " " + gpu.driverInfo + " | Vulkan " + gpu.getApiVersionString());
            }
            catch (Throwable t) {
                trace.header.add("gpu: not available (" + t + ")");
            }
            for (String line : description) trace.header.add(line);
            trace.header.add("game executable (matched against process names): " + (trace.gameExe.isEmpty() ? "(unknown)" : trace.gameExe));
            current = trace;
            trace.stage(LaunchAnalysis.STAGE_ENV_READY, null);
            trace.startSampler();
            trace.flush();
            return trace;
        }
        catch (Throwable t) {
            Log.w(TAG, "launch trace unavailable", t);
            current = null;
            return null;
        }
    }

    public static LaunchTrace current() {
        return current;
    }

    /** File that receives the stdout+stderr of box64/wine, or null when no trace is active. */
    public static File currentOutputFile() {
        LaunchTrace trace = current;
        return trace != null && !trace.finished ? trace.outputFile : null;
    }

    /** Called right before box64 is started: exact command, environment and file checks. */
    public static void onPreExec(String command, EnvVars envVars, File rootDir) {
        LaunchTrace trace = current;
        if (trace == null) return;
        try {
            trace.header.add("command: " + command);
            File box64 = new File(rootDir, "usr/local/bin/box64");
            trace.header.add("box64: " + box64 + " exists=" + box64.isFile() + " executable=" + box64.canExecute() + " size=" + box64.length());
            File wine = new File(rootDir, "opt");
            trace.header.add("rootfs: " + rootDir + " exists=" + rootDir.isDirectory() + " | opt dir exists=" + wine.isDirectory());
            List<String> names = new ArrayList<>();
            for (String name : envVars) names.add(name);
            java.util.Collections.sort(names);
            trace.header.add("environment (" + names.size() + " variables):");
            for (String name : names) trace.header.add("  " + name + "=" + envVars.get(name));
            trace.flush();
        }
        catch (Throwable t) {
            Log.w(TAG, "onPreExec", t);
        }
    }

    public static void onExecResult(int pid, String error) {
        LaunchTrace trace = current;
        if (trace == null) return;
        trace.guestPid = pid;
        if (pid == -1) trace.execError = error != null ? error : "unknown error starting the process";
        else trace.stage(LaunchAnalysis.STAGE_BOX64_STARTED, "pid " + pid);
        trace.flush();
    }

    /** The guest process (box64 wine ...) ended. Writes the final report. Returns true if the user should be notified. */
    public static boolean end(int status) {
        LaunchTrace trace = current;
        if (trace == null || trace.finished) return false;
        return trace.finish(status);
    }

    /** The session is closed from the app (exit button, back to menu) without the guest having failed. */
    public static void endByUser() {
        LaunchTrace trace = current;
        if (trace == null || trace.finished) return;
        trace.exited = true;
        trace.exitStatus = 0;
        trace.exitElapsedMs = (System.nanoTime() - trace.startNanos) / 1_000_000L;
        synchronized (trace) {
            trace.note("session closed from the app (exit)");
        }
        trace.finished = true;
        if (trace.sampler != null) trace.sampler.interrupt();
        trace.flush();
    }

    // ---------------------------------------------------------------------------------------------

    public synchronized void stage(int stage, String detail) {
        if (stage < 0 || stage >= reached.length || reached[stage]) return;
        reached[stage] = true;
        timeline.add(String.format(Locale.ENGLISH, "T+%6.1fs  STAGE %d: %s%s", elapsedSeconds(), stage, LaunchAnalysis.stageName(stage), detail != null ? " (" + detail + ")" : ""));
    }

    private double elapsedSeconds() {
        return (System.nanoTime() - startNanos) / 1e9;
    }

    private synchronized void note(String text) {
        timeline.add(String.format(Locale.ENGLISH, "T+%6.1fs  %s", elapsedSeconds(), text));
    }

    private void startSampler() {
        sampler = new Thread(() -> {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
            while (!finished) {
                try {
                    sampleProcesses();
                    sampleVortekStages();
                    limitOutputSize();
                    long now = System.nanoTime();
                    double seconds = (now - startNanos) / 1e9;
                    long flushEvery = seconds < 300 ? 10_000_000_000L : 30_000_000_000L;
                    if (now - lastFlushNanos >= flushEvery) flush();
                    Thread.sleep(seconds < 180 ? 1000 : 10000);
                }
                catch (InterruptedException e) {
                    return;
                }
                catch (Throwable t) {
                    Log.w(TAG, "sampler", t);
                }
            }
        }, "LURVETA-LaunchTrace");
        sampler.setDaemon(true);
        sampler.start();
    }

    private void sampleProcesses() {
        File[] entries = new File("/proc").listFiles();
        if (entries == null) return;
        Map<Integer, String> now = new HashMap<>();
        for (File entry : entries) {
            String name = entry.getName();
            if (name.isEmpty() || !Character.isDigit(name.charAt(0))) continue;
            try {
                if (Os.stat(entry.getPath()).st_uid != uid) continue;
                int pid = Integer.parseInt(name);
                if (pid == Process.myPid()) continue;
                String stat = readSmall(entry.getPath() + "/stat");
                int open = stat.indexOf('('), close = stat.lastIndexOf(')');
                if (open < 0 || close < 0) continue;
                String comm = stat.substring(open + 1, close);
                String state = close + 2 < stat.length() ? stat.substring(close + 2, close + 3) : "?";
                String cmdline = readSmall(entry.getPath() + "/cmdline").replace('\0', ' ').trim();
                if (cmdline.length() > 110) cmdline = cmdline.substring(0, 110) + "…";
                now.put(pid, comm + " [" + state + "] " + cmdline);
            }
            catch (Throwable ignored) {
                // the process ended while being read
            }
        }

        synchronized (this) {
            for (Map.Entry<Integer, String> e : now.entrySet()) {
                if (!lastProcesses.containsKey(e.getKey())) {
                    note("+ process " + e.getKey() + " " + e.getValue());
                    everSeen.put(e.getKey(), e.getValue());
                    classify(e.getValue().toLowerCase(Locale.ROOT));
                }
            }
            for (Map.Entry<Integer, String> e : lastProcesses.entrySet()) {
                if (!now.containsKey(e.getKey())) note("- process " + e.getKey() + " ended: " + e.getValue());
            }
            lastProcesses.clear();
            lastProcesses.putAll(now);
        }
    }

    private void classify(String text) {
        if (text.contains("wineserver") || text.contains("wine")) stage(LaunchAnalysis.STAGE_WINE_RUNNING, null);
        if (text.contains("explorer")) stage(LaunchAnalysis.STAGE_EXPLORER, null);
        if (!gameExe.isEmpty()) {
            String shortName = gameExe.length() > 15 ? gameExe.substring(0, 15) : gameExe;   // Linux comm is 15 chars
            if (text.startsWith(shortName) || text.contains(gameExe)) stage(LaunchAnalysis.STAGE_GAME_PROCESS, null);
        }
    }

    /** Vortek writes "STAGE ..." lines into files/logs/vortek-diagnostics.log (see VkContext_logInfo). */
    private void sampleVortekStages() {
        for (String line : readLogLines("logs/vortek-diagnostics.log", VORTEK_MARK)) {
            if (line.contains("STAGE client-connected")) stage(LaunchAnalysis.STAGE_VULKAN_CLIENT, null);
            else if (line.contains("STAGE instance-created")) stage(LaunchAnalysis.STAGE_VULKAN_INSTANCE, afterMarker(line));
            else if (line.contains("STAGE device-created")) stage(LaunchAnalysis.STAGE_VULKAN_DEVICE, null);
            else if (line.contains("STAGE first-present")) stage(LaunchAnalysis.STAGE_FIRST_PRESENT, null);
        }
    }

    private static String afterMarker(String line) {
        int i = line.indexOf("STAGE ");
        String s = i >= 0 ? line.substring(i + 6) : line;
        return s.length() > 160 ? s.substring(0, 160) + "…" : s;
    }

    private void limitOutputSize() {
        try {
            if (outputFile.length() > MAX_OUTPUT_BYTES) {
                try (RandomAccessFile file = new RandomAccessFile(outputFile, "rw")) {
                    file.setLength(0);
                }
                note("guest-output.log exceeded " + (MAX_OUTPUT_BYTES >> 20) + " MB and was truncated");
            }
        }
        catch (IOException ignored) {}
    }

    private boolean finish(int status) {
        exited = true;
        exitStatus = status;
        exitElapsedMs = (System.nanoTime() - startNanos) / 1_000_000L;
        try {
            sampleProcesses();
            sampleVortekStages();
        }
        catch (Throwable ignored) {}
        synchronized (this) {
            note("guest process ended: " + LaunchAnalysis.describeExit(status));
        }
        finished = true;
        if (sampler != null) sampler.interrupt();
        flush();
        boolean abnormal = LaunchAnalysis.isAbnormalEnd(execError, reached, status, exitElapsedMs);
        if (abnormal) writeNotice();
        return abnormal;
    }

    // ---------------------------------------------------------------------------------------------

    private String buildReport() {
        StringBuilder sb = new StringBuilder(16384);
        sb.append("=== LURVETA launch report: ").append(title).append(" ===\n");
        sb.append("started (epoch s): ").append(startEpochSeconds).append(" | elapsed: ").append(String.format(Locale.ENGLISH, "%.1f s", exited ? exitElapsedMs / 1000.0 : elapsedSeconds())).append('\n');
        for (String line : header) sb.append(line).append('\n');

        long elapsedMs = exited ? exitElapsedMs : (System.nanoTime() - startNanos) / 1_000_000L;
        sb.append("\n--- RESULT ---\n");
        sb.append("state: ").append(exited ? "ended, " + LaunchAnalysis.describeExit(exitStatus) : "running").append('\n');
        int last = LaunchAnalysis.lastStage(reached);
        sb.append("last stage reached: ").append(last >= 0 ? last + " — " + LaunchAnalysis.stageName(last) : "none").append('\n');
        sb.append("stages: ");
        for (int i = 0; i < reached.length; i++) sb.append(i).append(reached[i] ? "=yes " : "=NO ");
        sb.append('\n');
        sb.append("verdict (heuristic): ").append(LaunchAnalysis.verdict(execError, reached, exited, exitStatus, elapsedMs)).append('\n');

        sb.append("\n--- TIMELINE ---\n");
        synchronized (this) {
            for (String line : timeline) sb.append(line).append('\n');
        }

        List<String> failures = readLogLines("logs/vortek-diagnostics.log", " FAIL ");
        List<String> infos = readLogLines("logs/vortek-diagnostics.log", " INFO ");
        List<String> fatals = readLogLines("logs/vortek-errors.log", "");
        sb.append("\n--- VORTEK / VULKAN ---\n");
        sb.append("failed Vulkan calls: ").append(failures.size()).append(" | fatal errors: ").append(fatals.size()).append('\n');
        for (String line : infos) sb.append(line).append('\n');
        for (String line : failures) sb.append(line).append('\n');
        for (String line : fatals) sb.append(line).append('\n');

        File[] dxvk = dxvkLogs();
        sb.append("\n--- DXVK LOGS (").append(dxvk.length).append(" file(s) in C:\\lurveta-logs) ---\n");
        for (File file : dxvk) {
            sb.append("# ").append(file.getName()).append(" (").append(file.length()).append(" bytes)\n");
            for (String line : tail(file, 120)) sb.append(line).append('\n');
        }

        sb.append("\n--- BOX64 / WINE OUTPUT (last ").append(OUTPUT_TAIL_LINES).append(" lines of ").append(outputFile.length()).append(" bytes) ---\n");
        for (String line : tail(outputFile, OUTPUT_TAIL_LINES)) sb.append(line).append('\n');
        sb.append("=== end of report ===\n");
        return sb.toString();
    }

    private File[] dxvkLogs() {
        File[] files = dxvkLogDir() != null ? dxvkLogDir().listFiles((d, n) -> n.endsWith(".log")) : null;
        if (files == null) return new File[0];
        List<File> result = new ArrayList<>();
        for (File f : files) if (f.lastModified() / 1000 >= startEpochSeconds - 2) result.add(f);
        File[] array = result.toArray(new File[0]);
        Arrays.sort(array, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return array.length > 4 ? Arrays.copyOf(array, 4) : array;
    }

    private File dxvkLogDirCache;

    public void setDxvkLogDir(File directory) {
        dxvkLogDirCache = directory;
    }

    private File dxvkLogDir() {
        return dxvkLogDirCache;
    }

    public synchronized void flush() {
        lastFlushNanos = System.nanoTime();
        String report = buildReport();
        write(new File(dir, "launch-latest.txt"), report);
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        File publicDir = new File(downloads, "LURVETA/logs");
        if (publicDir.isDirectory() || publicDir.mkdirs()) write(new File(publicDir, "launch-latest.txt"), report);
    }

    private static void write(File file, String content) {
        try {
            File parent = file.getParentFile();
            if (parent != null && !parent.isDirectory()) parent.mkdirs();
            try (FileWriter writer = new FileWriter(file, false)) {
                writer.write(content);
            }
        }
        catch (IOException e) {
            Log.w(TAG, "unable to write " + file, e);
        }
    }

    private void writeNotice() {
        String verdict = LaunchAnalysis.verdict(execError, reached, true, exitStatus, exitElapsedMs);
        write(new File(dir, "notice.txt"), title + "\n" + verdict + "\n\nDownload/LURVETA/logs/launch-latest.txt");
    }

    /** Text of the pending "the last launch ended unexpectedly" notice, or null. Deletes it. */
    public static String takePendingNotice(Context context) {
        File file = new File(context.getFilesDir(), "logs/launch/notice.txt");
        if (!file.isFile()) return null;
        String text = readAll(file);
        file.delete();
        return text;
    }

    private void rotate() {
        if (!dir.isDirectory()) dir.mkdirs();
        File latest = new File(dir, "launch-latest.txt");
        File out = outputFile;
        if (latest.isFile() || out.isFile()) {
            long stamp = Math.max(latest.lastModified(), out.lastModified()) / 1000;
            if (latest.isFile()) latest.renameTo(new File(dir, "launch-" + stamp + ".txt"));
            if (out.isFile() && out.length() > 0) out.renameTo(new File(dir, "guest-output-" + stamp + ".log"));
        }
        // the new guest output file is created empty so the redirect appends to a fresh file
        try { new FileWriter(outputFile, false).close(); } catch (IOException ignored) {}
        File[] olds = dir.listFiles((d, n) -> n.startsWith("launch-") && n.endsWith(".txt") && !n.equals("launch-latest.txt"));
        if (olds != null && olds.length > KEEP_RUNS) {
            Arrays.sort(olds, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
            for (int i = KEEP_RUNS; i < olds.length; i++) {
                String stamp = olds[i].getName().substring(7, olds[i].getName().length() - 4);
                olds[i].delete();
                new File(dir, "guest-output-" + stamp + ".log").delete();
            }
        }
    }

    // ---- small file helpers ---------------------------------------------------------------------

    private List<String> readLogLines(String relativePath, String contains) {
        List<String> result = new ArrayList<>();
        File log = new File(context.getFilesDir(), relativePath);
        if (!log.isFile()) return result;
        try (BufferedReader reader = new BufferedReader(new FileReader(log))) {
            String line;
            while ((line = reader.readLine()) != null) {
                int space = line.indexOf(' ');
                long epoch = -1;
                try { epoch = Long.parseLong(space > 0 ? line.substring(0, space) : line); } catch (NumberFormatException ignored) {}
                if (epoch >= startEpochSeconds - 1 && (contains.isEmpty() || line.contains(contains))) result.add(line.trim());
            }
        }
        catch (IOException ignored) {}
        return result;
    }

    private static List<String> tail(File file, int lines) {
        List<String> result = new ArrayList<>();
        if (!file.isFile() || file.length() == 0) return result;
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            long length = raf.length();
            long start = Math.max(0, length - 96 * 1024);
            raf.seek(start);
            byte[] data = new byte[(int)(length - start)];
            raf.readFully(data);
            String[] all = new String(data, StandardCharsets.UTF_8).split("\n");
            for (int i = Math.max(start > 0 ? 1 : 0, all.length - lines); i < all.length; i++) {
                String line = all[i];
                result.add(line.length() > 400 ? line.substring(0, 400) + "…" : line);
            }
        }
        catch (IOException ignored) {}
        return result;
    }

    private static String readSmall(String path) throws IOException {
        try (FileInputStream in = new FileInputStream(path)) {
            byte[] buffer = new byte[1024];
            int length = in.read(buffer);
            return length > 0 ? new String(buffer, 0, length, StandardCharsets.UTF_8) : "";
        }
    }

    private static String readAll(File file) {
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) sb.append(line).append('\n');
            return sb.toString().trim();
        }
        catch (IOException e) {
            return null;
        }
    }
}
