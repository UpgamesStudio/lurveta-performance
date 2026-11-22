package com.winlator.core;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Environment;
import android.os.Process;
import android.util.Log;

import com.winlator.BuildConfig;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * LURVETA Performance — benchmark session recorder.
 *
 * Output folder: Download/LURVETA/benchmarks/<date>_<label>/ (falls back to the app's external
 * files dir when Download is not writable):
 *   frames.csv   t_ms,frametime_ms                  (one row per presented frame)
 *   sensors.csv  t_ms,fps,cpu_temp_c,gpu_temp_c,battery_temp_c,thermal_status,thermal_headroom,
 *                mem_used_mb,mem_avail_mb,proc_cpu_pct,proc_rss_mb,procs,top_proc,gpu_load_pct,
 *                gpu_freq_mhz,cpu<N>_mhz...  (one row per second)
 *   events.csv   t_ms,event,duration_ms,detail      (stalls, freezes, Vortek fatal errors)
 *   summary.txt  statistics of the whole session + device/GPU/profile information
 *
 * LURVETA: proc_* = all processes of the app's uid (app + Wine + Box64), 100% = one core.
 * gpu_* are empty when Android does not let the app read them (common): never estimated.
 *
 * Everything runs on a background-priority thread; the game thread only stores timestamps.
 */
public final class BenchmarkRecorder {
    private static final String TAG = "BenchmarkRecorder";
    private static final double HITCH_MS = 100;                  // frame slower than 100 ms
    private static final double STALL_MS = 1000;                 // frame slower than 1 s
    private static final long FREEZE_NANOS = 5_000_000_000L;     // no frame for 5 s
    private static final long FAILURE_NANOS = 10_000_000_000L;   // no frame for 10 s = failure
    private final Context context;
    private final FrameStats frameStats;
    private final ThermalMonitor thermalMonitor;
    private final String label;
    private final String profileName;
    private volatile boolean running = false;
    private Thread thread;
    private File outputDir;

    public BenchmarkRecorder(Context context, FrameStats frameStats, String label, String profileName) {
        this.context = context.getApplicationContext();
        this.frameStats = frameStats;
        this.thermalMonitor = new ThermalMonitor(context);
        this.label = label.replaceAll("[^A-Za-z0-9._-]+", "_");
        this.profileName = profileName;
    }

    public boolean isRunning() {
        return running;
    }

    public File getOutputDir() {
        return outputDir;
    }

    public synchronized boolean start() {
        if (running) return true;
        String name = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date())+"_"+label;
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        outputDir = new File(downloads, "LURVETA/benchmarks/"+name);
        if (!outputDir.isDirectory() && !outputDir.mkdirs()) {
            File external = context.getExternalFilesDir("benchmarks");
            outputDir = new File(external != null ? external : context.getFilesDir(), name);
            if (!outputDir.isDirectory() && !outputDir.mkdirs()) return false;
        }

        running = true;
        thread = new Thread(this::loop, "LURVETA-Benchmark");
        thread.start();
        return true;
    }

    public synchronized void stop() {
        if (!running) return;
        running = false;
        try {
            thread.join(3000);
        }
        catch (InterruptedException ignored) {}
        thread = null;
    }

    private void loop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
        ActivityManager activityManager = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo memoryInfo = new ActivityManager.MemoryInfo();
        int cpuCount = Runtime.getRuntime().availableProcessors();
        long[] buffer = new long[4096];
        List<Double> allFrametimes = new ArrayList<>();
        List<Double> perSecondFps = new ArrayList<>();

        ProcessSampler processSampler = new ProcessSampler();
        GpuSampler gpuSampler = new GpuSampler();
        long startEpochSeconds = System.currentTimeMillis() / 1000;
        int hitches = 0, stalls = 0, freezes = 0;
        double longestGapMs = 0, procCpuSum = 0, failureSeconds = -1;
        String failureReason = null;
        long peakRssMb = 0, freezeStartNanos = 0;
        int procSamples = 0, procRows = 0;
        float peakCpuTemp = Float.NaN, peakGpuTemp = Float.NaN, peakBatteryTemp = Float.NaN;
        boolean frozen = false;

        long nextFrame = frameStats.getFrameCount();
        long startNanos = System.nanoTime();
        long lastTimestamp = -1;
        long lastSensorNanos = 0;
        long framesAtLastSensor = nextFrame;

        try (BufferedWriter frames = new BufferedWriter(new FileWriter(new File(outputDir, "frames.csv")));
             BufferedWriter sensors = new BufferedWriter(new FileWriter(new File(outputDir, "sensors.csv")));
             BufferedWriter events = new BufferedWriter(new FileWriter(new File(outputDir, "events.csv")))) {
            frames.write("t_ms,frametime_ms\n");
            events.write("t_ms,event,duration_ms,detail\n");
            StringBuilder header = new StringBuilder("t_ms,fps,cpu_temp_c,gpu_temp_c,battery_temp_c,thermal_status,thermal_headroom,mem_used_mb,mem_avail_mb,proc_cpu_pct,proc_rss_mb,procs,top_proc,gpu_load_pct,gpu_freq_mhz");
            for (int i = 0; i < cpuCount; i++) header.append(",cpu").append(i).append("_mhz");
            sensors.write(header.append('\n').toString());

            while (running) {
                // Drain new frame timestamps.
                int count = frameStats.copyTimestamps(nextFrame, buffer);
                for (int i = 0; i < count; i++) {
                    long ts = buffer[i];
                    if (lastTimestamp >= 0 && ts > lastTimestamp) {
                        double frametime = (ts - lastTimestamp) / 1e6;
                        allFrametimes.add(frametime);
                        frames.write(String.format(Locale.ENGLISH, "%.3f,%.3f\n", (ts - startNanos) / 1e6, frametime));
                        longestGapMs = Math.max(longestGapMs, frametime);
                        if (frametime > HITCH_MS) hitches++;
                        if (frametime > STALL_MS) {
                            stalls++;
                            events.write(String.format(Locale.ENGLISH, "%.0f,stall,%.0f,\n", (lastTimestamp - startNanos) / 1e6, frametime));
                        }
                    }
                    if (frozen) {
                        frozen = false;
                        events.write(String.format(Locale.ENGLISH, "%.0f,freeze_end,%.0f,frames resumed\n", (ts - startNanos) / 1e6, (ts - freezeStartNanos) / 1e6));
                    }
                    lastTimestamp = ts;
                }
                nextFrame += count;

                long now = System.nanoTime();
                long reference = lastTimestamp >= 0 ? lastTimestamp : startNanos;
                if (!frozen && now - reference >= FREEZE_NANOS) {
                    frozen = true;
                    freezes++;
                    freezeStartNanos = reference;
                    events.write(String.format(Locale.ENGLISH, "%.0f,freeze_start,,no frame for %d s\n", (reference - startNanos) / 1e6, FREEZE_NANOS / 1_000_000_000L));
                    events.flush();
                }
                if (frozen && failureSeconds < 0 && now - freezeStartNanos >= FAILURE_NANOS) {
                    failureSeconds = (freezeStartNanos - startNanos) / 1e9;
                    failureReason = "no frame presented for " + (FAILURE_NANOS / 1_000_000_000L) + " s";
                }

                if (now - lastSensorNanos >= 1_000_000_000L) {
                    long frameDelta = frameStats.getFrameCount() - framesAtLastSensor;
                    double fps = lastSensorNanos > 0 ? frameDelta * 1e9 / (now - lastSensorNanos) : 0;
                    if (lastSensorNanos > 0) perSecondFps.add(fps);
                    framesAtLastSensor = frameStats.getFrameCount();
                    lastSensorNanos = now;

                    activityManager.getMemoryInfo(memoryInfo);
                    ProcessSampler.Sample proc = processSampler.sample();
                    peakRssMb = Math.max(peakRssMb, proc.rssMb);
                    if (procRows++ > 0) { // the first sample has no CPU-time baseline
                        procCpuSum += proc.cpuPercent;
                        procSamples++;
                    }
                    float cpuTemp = thermalMonitor.getCpuTemperature(), gpuTemp = thermalMonitor.getGpuTemperature(), batteryTemp = thermalMonitor.getBatteryTemperature();
                    peakCpuTemp = maxOf(peakCpuTemp, cpuTemp);
                    peakGpuTemp = maxOf(peakGpuTemp, gpuTemp);
                    peakBatteryTemp = maxOf(peakBatteryTemp, batteryTemp);
                    StringBuilder row = new StringBuilder();
                    row.append(String.format(Locale.ENGLISH, "%.0f,%.2f,%s,%s,%s,%d,%s,%d,%d,%.1f,%d,%d,%s,%s,%s",
                        (now - startNanos) / 1e6, fps,
                        fmt(cpuTemp), fmt(gpuTemp), fmt(batteryTemp),
                        thermalMonitor.getThermalStatus(), fmt(thermalMonitor.getThermalHeadroom()),
                        (memoryInfo.totalMem - memoryInfo.availMem) >> 20, memoryInfo.availMem >> 20,
                        proc.cpuPercent, proc.rssMb, proc.processes, proc.topProcess.replace(',', '_'),
                        fmt(gpuSampler.getLoadPercent()), fmt(gpuSampler.getFrequencyMhz())));
                    short[] clocks = CPUStatus.getCurrentClockSpeeds();
                    for (short clock : clocks) row.append(',').append(clock);
                    sensors.write(row.append('\n').toString());
                    sensors.flush();
                    frames.flush();
                }

                try {
                    Thread.sleep(250);
                }
                catch (InterruptedException e) {
                    break;
                }
            }
        }
        catch (IOException e) {
            Log.e(TAG, "benchmark recording failed", e);
        }

        List<String> vortekErrors = readVortekErrors(startEpochSeconds);
        if (failureSeconds < 0 && !vortekErrors.isEmpty()) {
            failureSeconds = Math.max(0, parseEpoch(vortekErrors.get(0)) - startEpochSeconds);
            failureReason = "Vortek fatal error: " + vortekErrors.get(0);
        }
        appendEvents(vortekErrors, startEpochSeconds);

        StringBuilder stability = new StringBuilder();
        stability.append(String.format(Locale.ENGLISH, "hitches (>%.0f ms): %d | stalls (>%.0f ms): %d | freezes (no frame >= %d s): %d | longest frame gap: %.0f ms\n",
            HITCH_MS, hitches, STALL_MS, stalls, FREEZE_NANOS / 1_000_000_000L, freezes, longestGapMs));
        stability.append(failureSeconds >= 0
            ? String.format(Locale.ENGLISH, "time_to_failure_s: %.1f (%s)\n", failureSeconds, failureReason)
            : "time_to_failure_s: none (no freeze >= " + (FAILURE_NANOS / 1_000_000_000L) + " s and no Vortek fatal error during the recording)\n");
        if (!vortekErrors.isEmpty()) stability.append("vortek_fatal_errors: ").append(vortekErrors.size()).append(" (see events.csv)\n");
        stability.append(String.format(Locale.ENGLISH, "process cpu avg: %.0f %% of one core | peak rss: %d MB (app + Wine + Box64 processes)\n",
            procSamples > 0 ? procCpuSum / procSamples : 0, peakRssMb));
        stability.append("peak temperatures: cpu ").append(fmt(peakCpuTemp)).append(" | gpu ").append(fmt(peakGpuTemp)).append(" | battery ").append(fmt(peakBatteryTemp)).append(" (C)\n");
        stability.append("gpu counters: ").append(gpuSampler.describeSources()).append('\n');
        stability.append("note: a freeze is also recorded if the game is paused or in background while recording\n");

        writeSummary(allFrametimes, perSecondFps, (System.nanoTime() - startNanos) / 1e9, stability.toString());
    }

    private static float maxOf(float current, float value) {
        if (Float.isNaN(value)) return current;
        return Float.isNaN(current) ? value : Math.max(current, value);
    }

    /** Lines of files/logs/vortek-errors.log written during this session ("<epoch> <error> ..."). */
    private List<String> readVortekErrors(long startEpochSeconds) {
        List<String> result = new ArrayList<>();
        File log = new File(context.getFilesDir(), "logs/vortek-errors.log");
        if (!log.isFile()) return result;
        try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(log))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (parseEpoch(line) >= startEpochSeconds) result.add(line.trim());
            }
        }
        catch (IOException e) {
            Log.w(TAG, "unable to read " + log, e);
        }
        return result;
    }

    private static long parseEpoch(String line) {
        try {
            int space = line.indexOf(' ');
            return Long.parseLong(space > 0 ? line.substring(0, space) : line);
        }
        catch (NumberFormatException e) {
            return -1;
        }
    }

    private void appendEvents(List<String> vortekErrors, long startEpochSeconds) {
        if (vortekErrors.isEmpty()) return;
        try (BufferedWriter events = new BufferedWriter(new FileWriter(new File(outputDir, "events.csv"), true))) {
            for (String line : vortekErrors) {
                events.write(String.format(Locale.ENGLISH, "%d,vortek_fatal,,%s\n", (parseEpoch(line) - startEpochSeconds) * 1000, line.replace(',', ';')));
            }
        }
        catch (IOException e) {
            Log.w(TAG, "unable to append events", e);
        }
    }

    private void writeSummary(List<Double> frametimes, List<Double> perSecondFps, double seconds, String stability) {
        double[] values = new double[frametimes.size()];
        for (int i = 0; i < values.length; i++) values[i] = frametimes.get(i);
        FrameStats.Summary s = FrameStats.summarize(values);
        HardwareInfo.Gpu gpu = HardwareInfo.getGpu(context);

        double minSec = Double.MAX_VALUE, maxSec = 0;
        for (double fps : perSecondFps) {
            minSec = Math.min(minSec, fps);
            maxSec = Math.max(maxSec, fps);
        }

        StringBuilder sb = new StringBuilder();
        sb.append(BuildConfig.PRODUCT_NAME).append(' ').append(BuildConfig.VERSION_NAME).append(" — benchmark summary\n");
        sb.append("label: ").append(label).append('\n');
        sb.append("profile: ").append(profileName).append('\n');
        sb.append("device: ").append(android.os.Build.MANUFACTURER).append(' ').append(android.os.Build.MODEL).append(" | SoC ").append(HardwareInfo.getSocName()).append('\n');
        sb.append("cpu: ").append(HardwareInfo.getCpuSummary()).append('\n');
        sb.append("gpu: ").append(gpu.deviceName).append(" (").append(gpu.family.label).append(") | driver ").append(gpu.driverName).append(' ').append(gpu.driverInfo).append(" | Vulkan ").append(gpu.getApiVersionString()).append('\n');
        sb.append(String.format(Locale.ENGLISH, "duration_s: %.1f\n", seconds));
        if (s != null) {
            sb.append(String.format(Locale.ENGLISH, "frames: %d\n", s.frames));
            sb.append(String.format(Locale.ENGLISH, "avg_fps: %.2f\n1pct_low_fps: %.2f\n0.1pct_low_fps: %.2f\n", s.averageFps, s.low1Fps, s.low01Fps));
            sb.append(String.format(Locale.ENGLISH, "frametime_ms avg %.2f | p50 %.2f | p95 %.2f | p99 %.2f | max %.2f | stddev %.2f\n", s.averageFrametimeMs, s.p50Ms, s.p95Ms, s.p99Ms, s.maxFrametimeMs, s.frametimeStdDevMs));
        }
        else sb.append("frames: 0 (no frames were presented during the recording)\n");
        if (!perSecondFps.isEmpty()) sb.append(String.format(Locale.ENGLISH, "per_second_fps min %.1f | max %.1f\n", minSec, maxSec));
        sb.append(stability);
        sb.append("definitions: 1%/0.1% low = 1000 / mean of the slowest 1%/0.1% frametimes\n");

        try (BufferedWriter writer = new BufferedWriter(new FileWriter(new File(outputDir, "summary.txt")))) {
            writer.write(sb.toString());
        }
        catch (IOException e) {
            Log.e(TAG, "unable to write summary", e);
        }
    }

    private static String fmt(float value) {
        return Float.isNaN(value) ? "" : String.format(Locale.ENGLISH, "%.1f", value);
    }
}
