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

    // Game process probe: CPU per thread of the game and its helper processes (is it working,
    // waiting or spinning?), and the Vortek activity probe (is it calling Vulkan, stuck in a call?).
    private final Map<Integer, Long> lastThreadTicks = new HashMap<>();
    private final Map<Integer, Long> lastProcessTicks = new HashMap<>();
    private long lastProbeNanos = 0;
    private final List<String> probeSummary = new ArrayList<>();
    private final Map<Integer, long[]> lastIo = new HashMap<>();
    private final java.util.Set<Integer> lastIoLimitNoted = new java.util.HashSet<>();
    private final Map<Integer, java.util.Set<String>> lastOpenFiles = new HashMap<>();
    private final Map<Integer, java.util.Set<String>> openFilesSummary = new TreeMap<>();
    private static final long CLOCK_TICKS = clockTicks();

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
            trace.header.add("LURVETA Performance " + BuildConfig.VERSION_NAME + " (code " + BuildConfig.VERSION_CODE + ") | build " + (BuildConfig.DEBUG ? "debug" : "release")
                             + " | source " + BuildConfig.BUILD_ID + " | built " + BuildConfig.BUILD_TIME);
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
                    sampleActivity();
                    sampleGameThreads();
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

    private static final String[] WINE_SYSTEM_EXES = {"explorer.exe", "services.exe", "winedevice.exe", "plugplay.exe", "svchost.exe", "rpcss.exe",
        "conhost.exe", "start.exe", "winhandler.exe", "wfm.exe", "wineboot.exe", "winemenubuilder", "rundll32.exe", "cmd.exe", "tabtip.exe", "taskmgr.exe"};

    /** True for the game's process: the known executable, or (launched from the desktop) any non-system .exe. */
    private boolean isGameProcess(String text) {
        if (!gameExe.isEmpty()) {
            String shortName = gameExe.length() > 15 ? gameExe.substring(0, 15) : gameExe;   // Linux comm is 15 chars
            return text.startsWith(shortName) || text.contains(gameExe);
        }
        String comm = text.split(" ")[0];
        if (!comm.endsWith(".exe") && !(comm.length() == 15 && comm.contains(".ex"))) return false;
        for (String system : WINE_SYSTEM_EXES) if (comm.startsWith(system.length() > 15 ? system.substring(0, 15) : system)) return false;
        return true;
    }

    private void classify(String text) {
        if (text.contains("wineserver") || text.contains("wine")) stage(LaunchAnalysis.STAGE_WINE_RUNNING, null);
        if (text.contains("explorer")) stage(LaunchAnalysis.STAGE_EXPLORER, null);
        if (isGameProcess(text)) stage(LaunchAnalysis.STAGE_GAME_PROCESS, text.split(" ")[0]);
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

    private static long clockTicks() {
        try {
            long ticks = Os.sysconf(android.system.OsConstants._SC_CLK_TCK);
            return ticks > 0 ? ticks : 100;
        }
        catch (Throwable t) {
            return 100;
        }
    }

    /** files/logs/vortek-activity.txt, rewritten every 2 s by Vortek's watchdog thread. */
    private static final class ClientActivity {
        String body;
        String key;
        long lastNoteNanos;
        long fpsPresents = -1;
        long fpsNanos;
        String fps = "";
        double maxFps = 0;
    }
    private final Map<Integer, ClientActivity> clients = new TreeMap<>();

    /** files/logs/vortek-activity-<pid>.txt, one per Vulkan client, rewritten every 2 s by Vortek. */
    private void sampleActivity() {
        File[] files = new File(context.getFilesDir(), "logs").listFiles((d, n) -> n.startsWith("vortek-activity-") && n.endsWith(".txt"));
        if (files == null) return;
        long now = System.nanoTime();
        for (File file : files) {
            if (file.lastModified() / 1000 < startEpochSeconds - 1) continue;
            String line;
            try {
                line = readSmall(file.getPath()).trim();
            }
            catch (IOException e) {
                continue;
            }
            int space = line.indexOf(' ');
            if (space < 0) continue;
            String body = line.substring(space + 1);
            int pid = (int)parseMs(body, "pid=");
            ClientActivity client = clients.get(pid);
            if (client == null) clients.put(pid, client = new ClientActivity());
            client.body = body;

            // Frames per second from the present counter (over >= 4 s windows).
            long presents = parseMs(body, "presents=");
            if (client.fpsPresents < 0 || presents < client.fpsPresents) {
                client.fpsPresents = presents;
                client.fpsNanos = now;
            }
            else if (now - client.fpsNanos >= 4_000_000_000L) {
                double fps = (presents - client.fpsPresents) / ((now - client.fpsNanos) / 1e9);
                client.maxFps = Math.max(client.maxFps, fps);
                client.fps = String.format(Locale.ENGLISH, " | ~%.1f fps (presents/s, last %.0f s)", fps, (now - client.fpsNanos) / 1e9);
                client.fpsPresents = presents;
                client.fpsNanos = now;
            }

            // Note it when the state changes (ignoring moving counters) and every 15 s.
            String key = body.replaceAll("(idleMs|forMs|requests|submits|presents|images|allocMB)=-?\\d+", "");
            boolean stuck = body.contains("state=IN_CALL") && parseMs(body, "forMs=") >= 5000;
            if (!key.equals(client.key) || now - client.lastNoteNanos >= 15_000_000_000L || stuck && now - client.lastNoteNanos >= 5_000_000_000L) {
                note("vortek[" + clientName(pid) + "]: " + body + client.fps + (stuck ? "   <-- SERVER BLOCKED INSIDE THIS VULKAN CALL" : ""));
                client.key = key;
                client.lastNoteNanos = now;
            }
        }
    }

    private String clientName(int pid) {
        String name;
        synchronized (this) {
            name = everSeen.get(pid);
        }
        return pid + (name != null ? " " + name.split(" ")[0] : "");
    }

    private static long parseMs(String text, String key) {
        int i = text.indexOf(key);
        if (i < 0) return -1;
        int j = i + key.length();
        int k = j;
        while (k < text.length() && (Character.isDigit(text.charAt(k)) || text.charAt(k) == '-')) k++;
        try { return Long.parseLong(text.substring(j, k)); } catch (NumberFormatException e) { return -1; }
    }

    /** Every 5 s: CPU of each thread of the game / browser processes (Wine names its threads). */
    private void sampleGameThreads() {
        long now = System.nanoTime();
        if (now - lastProbeNanos < 5_000_000_000L) return;
        double seconds = lastProbeNanos > 0 ? (now - lastProbeNanos) / 1e9 : 0;
        lastProbeNanos = now;

        List<Integer> targets = new ArrayList<>();
        synchronized (this) {
            for (Map.Entry<Integer, String> e : lastProcesses.entrySet()) {
                String text = e.getValue().toLowerCase(Locale.ROOT);
                if (isGameProcess(text) || text.contains("browser")) targets.add(e.getKey());
            }
        }

        for (int pid : targets) {
            File[] tasks = new File("/proc/" + pid + "/task").listFiles();
            if (tasks == null) continue;
            long processDelta = 0;
            int running = 0, sleeping = 0, disk = 0, other = 0;
            List<long[]> busiest = new ArrayList<>();          // {delta, tid}
            Map<Integer, String> names = new HashMap<>();
            Map<String, Integer> waits = new TreeMap<>();      // what the sleeping threads wait in
            for (File task : tasks) {
                try {
                    int tid = Integer.parseInt(task.getName());
                    String stat = readSmall(task.getPath() + "/stat");
                    int open = stat.indexOf('('), close = stat.lastIndexOf(')');
                    if (open < 0 || close < 0) continue;
                    String[] f = stat.substring(close + 2).split(" ");
                    char state = f[0].charAt(0);
                    if (state == 'R') running++; else if (state == 'S') sleeping++; else if (state == 'D') disk++; else other++;
                    long ticks = Long.parseLong(f[11]) + Long.parseLong(f[12]);
                    Long previous = lastThreadTicks.put(tid, ticks);
                    long delta = previous != null ? Math.max(0, ticks - previous) : 0;
                    processDelta += delta;
                    names.put(tid, stat.substring(open + 1, close) + "/" + state);
                    if (state == 'S' || state == 'D') {
                        String wchan = readSmall(task.getPath() + "/wchan").trim();
                        if (wchan.isEmpty() || wchan.equals("0")) wchan = "?";
                        waits.merge(wchan, 1, Integer::sum);
                    }
                    busiest.add(new long[]{delta, tid});
                }
                catch (Throwable ignored) {}
            }
            busiest.sort((a, b) -> Long.compare(b[0], a[0]));
            StringBuilder top = new StringBuilder();
            for (int i = 0; i < Math.min(4, busiest.size()); i++) {
                long[] t = busiest.get(i);
                double pct = seconds > 0 ? 100.0 * t[0] / CLOCK_TICKS / seconds : 0;
                top.append(String.format(Locale.ENGLISH, " [%s %.0f%%]", names.get((int)t[1]), pct));
            }
            if (!busiest.isEmpty()) {
                // "running" = spinning in user code; a number = blocked in that syscall (aarch64 numbers)
                String sys = "";
                try { sys = readSmall("/proc/" + pid + "/task/" + busiest.get(0)[1] + "/syscall").trim(); } catch (Throwable ignored) {}
                top.append(" (busiest thread syscall: ").append(sys.isEmpty() ? "?" : sys.split(" ")[0]).append(")");
            }
            long rssMb = 0;
            try {
                String[] statm = readSmall("/proc/" + pid + "/statm").trim().split(" ");
                rssMb = Long.parseLong(statm[1]) * 4096 / (1024 * 1024);
            }
            catch (Throwable ignored) {}
            // File I/O progress: a loading screen that reads nothing is waiting, not loading.
            String io = "";
            try {
                long rchar = -1, syscr = -1;
                for (String l : readSmall("/proc/" + pid + "/io").split("\n")) {
                    if (l.startsWith("rchar:")) rchar = Long.parseLong(l.substring(6).trim());
                    else if (l.startsWith("syscr:")) syscr = Long.parseLong(l.substring(6).trim());
                }
                long[] prev = lastIo.put(pid, new long[]{rchar, syscr});
                if (rchar >= 0 && prev != null && seconds > 0) {
                    io = String.format(Locale.ENGLISH, " | read %.1f MB/s in %.0f calls/s", (rchar - prev[0]) / 1048576.0 / seconds, (syscr - prev[1]) / seconds);
                }
                else if (rchar >= 0) io = String.format(Locale.ENGLISH, " | read total %d MB", rchar >> 20);
            }
            catch (Throwable ignored) {}
            String[] fdNames = new File("/proc/" + pid + "/fd").list();
            int fdCount = fdNames != null ? fdNames.length : -1;
            if (!lastIoLimitNoted.contains(pid)) {
                lastIoLimitNoted.add(pid);
                try {
                    for (String l : readSmall("/proc/" + pid + "/limits").split("\n")) {
                        if (l.startsWith("Max open files")) note("limits: pid " + pid + " " + l.replaceAll("\\s+", " ").trim());
                    }
                }
                catch (Throwable ignored) {}
            }
            if (fdNames != null && isGameProcess(lastProcesses.containsKey(pid) ? lastProcesses.get(pid).toLowerCase(Locale.ROOT) : "")) {
                java.util.TreeSet<String> files = new java.util.TreeSet<>();
                for (String fd : fdNames) {
                    try {
                        String target = Os.readlink("/proc/" + pid + "/fd/" + fd);
                        if (target.startsWith("/") && !target.startsWith("/dev/") && !target.startsWith("/proc/")) files.add(target);
                    }
                    catch (Throwable ignored) {}
                }
                java.util.Set<String> previous = lastOpenFiles.put(pid, files);
                if (previous != null) {
                    int changes = 0, hidden = 0;
                    for (String f : files) if (!previous.contains(f)) { if (changes++ < 8) note("files: pid " + pid + " opened " + shortPath(f)); else hidden++; }
                    for (String f : previous) if (!files.contains(f)) { if (changes++ < 8) note("files: pid " + pid + " closed " + shortPath(f)); else hidden++; }
                    if (hidden > 0) note("files: pid " + pid + " " + hidden + " more files opened/closed");
                }
                synchronized (this) { openFilesSummary.put(pid, files); }
            }
            StringBuilder waitText = new StringBuilder();
            List<Map.Entry<String, Integer>> waitList = new ArrayList<>(waits.entrySet());
            waitList.sort((a, b) -> b.getValue() - a.getValue());
            for (int i = 0; i < Math.min(4, waitList.size()); i++) waitText.append(i == 0 ? "" : ", ").append(waitList.get(i).getKey()).append(" x").append(waitList.get(i).getValue());
            double cpu = seconds > 0 ? 100.0 * processDelta / CLOCK_TICKS / seconds : 0;
            String name = lastProcesses.containsKey(pid) ? lastProcesses.get(pid).split(" ")[0] : String.valueOf(pid);
            boolean hasBaseline = lastProcessTicks.containsKey(pid);
            lastProcessTicks.put(pid, processDelta);
            String verdict = seconds <= 0 || !hasBaseline ? "first sample (no CPU baseline yet)" : cpu < 2 ? "IDLE/WAITING (almost no CPU: blocked or waiting for something)" : cpu > 80 * Math.max(1, running) && running <= 1 ? "one thread busy" : "working";
            String line = String.format(Locale.ENGLISH, "probe %s pid %d: cpu %.0f%% | rss %d MB | threads %d (R%d S%d D%d other%d) | %s | busiest:%s%s | fds %d | sleeping in: %s",
                                        name, pid, cpu, rssMb, tasks.length, running, sleeping, disk, other, verdict, top, io, fdCount, waitText.length() > 0 ? waitText : "-");
            note(line);
            synchronized (this) {
                probeSummary.add(line);
                if (probeSummary.size() > 6) probeSummary.remove(0);
            }
        }
    }

    private static String shortPath(String path) {
        int i = path.indexOf("/drive_");
        if (i < 0) i = path.indexOf("/dosdevices/");
        return i >= 0 ? path.substring(i) : (path.length() > 160 ? "…" + path.substring(path.length() - 160) : path);
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

    private volatile String gamesAliveAtEnd = null;

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
            List<String> alive = new ArrayList<>();
            for (Map.Entry<Integer, String> e : lastProcesses.entrySet()) {
                if (isGameProcess(e.getValue().toLowerCase(Locale.ROOT))) alive.add(e.getValue().split(" ")[0] + " (pid " + e.getKey() + ")");
            }
            gamesAliveAtEnd = alive.isEmpty() ? "none" : String.join(", ", alive);
            if (!alive.isEmpty()) note("game process(es) STILL RUNNING when the Wine session ended: " + gamesAliveAtEnd + " -> the exit code is not the game's");
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
        synchronized (this) {
            List<String> games = new ArrayList<>();
            for (String p : everSeen.values()) {
                String name = p.split(" ")[0];
                if (isGameProcess(p.toLowerCase(Locale.ROOT)) && !games.contains(name)) games.add(name);
            }
            sb.append("game process(es) detected: ").append(games.isEmpty() ? "none" : String.join(", ", games)).append('\n');
            if (gamesAliveAtEnd != null) sb.append("game process(es) still running when the session ended: ").append(gamesAliveAtEnd).append('\n');
        }
        sb.append("last stage reached: ").append(last >= 0 ? last + " — " + LaunchAnalysis.stageName(last) : "none").append('\n');
        sb.append("stages: ");
        for (int i = 0; i < reached.length; i++) sb.append(i).append(reached[i] ? "=yes " : "=NO ");
        sb.append('\n');
        sb.append("verdict (heuristic): ").append(LaunchAnalysis.verdict(execError, reached, exited, exitStatus, elapsedMs)).append('\n');
        synchronized (this) {
            if (clients.isEmpty()) sb.append("vortek activity: no Vulkan client connected yet\n");
            for (Map.Entry<Integer, ClientActivity> e : clients.entrySet()) {
                sb.append("vortek client ").append(clientName(e.getKey())).append(" (last): ").append(e.getValue().body).append(e.getValue().fps)
                  .append(String.format(Locale.ENGLISH, " | best window ~%.1f fps", e.getValue().maxFps)).append('\n');
            }
        }
        synchronized (this) {
            if (!probeSummary.isEmpty()) {
                sb.append("game process probe (latest samples):\n");
                for (String line : probeSummary) sb.append("  ").append(line).append('\n');
            }
            for (Map.Entry<Integer, java.util.Set<String>> e : openFilesSummary.entrySet()) {
                sb.append("files open in game pid ").append(e.getKey()).append(" (last sample, ").append(e.getValue().size()).append("):\n");
                int shown = 0;
                for (String f : e.getValue()) { if (shown++ >= 60) { sb.append("  …\n"); break; } sb.append("  ").append(shortPath(f)).append('\n'); }
            }
        }

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

        // Vulkan call journal of every client (written by Vortek, one per client process).
        File[] journals = new File(context.getFilesDir(), "logs").listFiles((d, n) -> n.startsWith("vortek-journal-") && n.endsWith(".txt"));
        if (journals != null) {
            Arrays.sort(journals, (a, b) -> a.getName().compareTo(b.getName()));
            for (File journal : journals) {
                if (journal.lastModified() / 1000 < startEpochSeconds - 1) continue;
                String pidText = journal.getName().substring(15, journal.getName().length() - 4);
                int pid = -1;
                try { pid = Integer.parseInt(pidText); } catch (NumberFormatException ignored) {}
                List<String> all = readLines(journal, 1024 * 1024);
                // Synchronisation and frame calls of the whole session: where a device is lost they show what came just before.
                List<String> sync = new ArrayList<>();
                for (String line : all) for (String k : SYNC_CALLS) if (line.contains(" " + k + " ")) { sync.add(line); break; }
                if (sync.size() > SYNC_LINES) sync = sync.subList(sync.size() - SYNC_LINES, sync.size());
                List<String> lines = all.size() > JOURNAL_LINES ? all.subList(all.size() - JOURNAL_LINES, all.size()) : all;
                sb.append("\n--- VULKAN SYNC/FRAME CALLS of client ").append(clientName(pid)).append(" (").append(sync.size()).append(" entries, whole journal of ").append(all.size()).append(") ---\n");
                for (String line : sync) sb.append(line).append('\n');
                sb.append("\n--- VULKAN CALL JOURNAL of client ").append(clientName(pid)).append(" (last ").append(lines.size()).append(" entries; consecutive identical calls merged) ---\n");
                for (String line : lines) sb.append(line).append('\n');
            }
        }

        OutputScan scan = scanOutput();
        sb.append("\n--- BOX64 / WINE: IMPORTANT LINES (whole output, ").append(outputFile.length()).append(" bytes; repeated noise filtered) ---\n");
        for (String line : scan.important) sb.append(line).append('\n');
        if (scan.importantDropped > 0) sb.append("(").append(scan.importantDropped).append(" more important lines not shown)\n");
        sb.append("vortek client EINTR fix (vtio): ").append(scan.vtioActive ? "loaded" : "NOT seen in the output")
          .append(scan.vtioEintr > 0 ? " | waits interrupted by signals: yes (see 'LURVETA vtio' lines)" : " | waits interrupted by signals: none logged")
          .append(scan.vtioAnomalies > 0 ? " | client I/O anomalies: YES (see 'LURVETA vtio: ANOMALY' lines)" : " | client I/O anomalies: none").append('\n');
        if (scan.dxvkNeedsRobustness2) {
            sb.append("KNOWN CAUSE: this DXVK version requires VK_EXT_robustness2, which this GPU driver does not offer;\n")
              .append("  DXVK 2.x cannot create a D3D11 device here. Use DXVK 1.10.x in the container settings.\n");
        }
        sb.append("noise filtered: ").append(scan.noise).append(" lines (\"function ... not found in lib\", wined3d PolygonOffset)\n");
        sb.append("\n--- BOX64 / WINE OUTPUT (last ").append(scan.tail.size()).append(" lines without noise) ---\n");
        for (String line : scan.tail) sb.append(line).append('\n');
        sb.append("=== end of report ===\n");
        return sb.toString();
    }


    private static final int JOURNAL_LINES = 160;
    private static final int SYNC_LINES = 150;
    private static final String[] SYNC_CALLS = {"vkCreateDevice", "vkDestroyDevice", "vkQueueSubmit", "vkQueueSubmit2", "vkWaitForFences", "vkGetFenceStatus",
        "vkResetFences", "vkDeviceWaitIdle", "vkQueueWaitIdle", "vkWaitSemaphores", "vkCreateSwapchainKHR", "vkAcquireNextImageKHR", "vkQueuePresentKHR"};
    private static final String[] NOISE = {"not found in lib", "PolygonOffset scale factor detection failed"};
    private static final String[] IMPORTANT = {"err:", "Unhandled", "xception", "wine: ", "rror", "fatal", "Fatal", "crash", "Crash",
        "SIGSEGV", "SIGILL", "SIGBUS", "Assertion", "abort", "Abort", "page fault", "backtrace", "Backtrace", "killed", "Killed", "failed", "Failed",
        "not supported", "LURVETA vtio"};

    private static final class OutputScan {
        final List<String> important = new ArrayList<>();
        final java.util.ArrayDeque<String> tail = new java.util.ArrayDeque<>();
        int importantDropped = 0;
        int noise = 0;
        boolean dxvkNeedsRobustness2 = false;
        boolean vtioActive = false;
        int vtioEintr = 0;
        int vtioAnomalies = 0;
    }

    /** Reads the whole Box64/Wine output once: keeps every important line (up to 250) and the tail without noise. */
    private OutputScan scanOutput() {
        OutputScan scan = new OutputScan();
        if (!outputFile.isFile()) return scan;
        try (BufferedReader reader = new BufferedReader(new FileReader(outputFile))) {
            String line;
            String previous = null;
            int repeats = 0;
            while ((line = reader.readLine()) != null) {
                boolean noisy = false;
                for (String n : NOISE) if (line.contains(n)) { noisy = true; break; }
                if (noisy) { scan.noise++; continue; }
                if (line.length() > 400) line = line.substring(0, 400) + "…";

                if (line.equals(previous)) { repeats++; continue; }
                if (repeats > 0) { addTail(scan, "  (previous line repeated " + repeats + " more times)"); repeats = 0; }
                previous = line;

                addTail(scan, line);
                if (line.contains("Required Vulkan extension VK_EXT_robustness2 not supported")) scan.dxvkNeedsRobustness2 = true;
                if (line.startsWith("LURVETA vtio: active")) scan.vtioActive = true;
                if (line.startsWith("LURVETA vtio:") && line.contains("interrupted by a signal")) scan.vtioEintr++;
                if (line.startsWith("LURVETA vtio:") && line.contains("ANOMALY")) scan.vtioAnomalies++;
                for (String k : IMPORTANT) {
                    if (line.contains(k)) {
                        if (scan.important.size() < 250) scan.important.add(line);
                        else scan.importantDropped++;
                        break;
                    }
                }
            }
            if (repeats > 0) addTail(scan, "  (previous line repeated " + repeats + " more times)");
        }
        catch (IOException ignored) {}
        return scan;
    }

    private static void addTail(OutputScan scan, String line) {
        scan.tail.addLast(line);
        if (scan.tail.size() > OUTPUT_TAIL_LINES) scan.tail.removeFirst();
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
        if (publicDir.isDirectory() || publicDir.mkdirs()) {
            write(new File(publicDir, "launch-latest.txt"), report);
            // One file per launch too, so the report of one game is not overwritten by the next launch.
            String stamp = new java.text.SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new java.util.Date(startEpochSeconds * 1000));
            write(new File(publicDir, "launch-" + stamp + ".txt"), report);
        }
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

    /** Every line of a file (at most maxBytes are read, from the start). */
    private static List<String> readLines(File file, int maxBytes) {
        List<String> result = new ArrayList<>();
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            String line;
            long read = 0;
            while ((line = reader.readLine()) != null && read < maxBytes) {
                read += line.length() + 1;
                result.add(line);
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
