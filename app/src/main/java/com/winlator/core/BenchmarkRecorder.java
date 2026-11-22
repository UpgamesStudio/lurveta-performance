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
 *                mem_used_mb,mem_avail_mb,cpu<N>_mhz...  (one row per second)
 *   summary.txt  statistics of the whole session + device/GPU/profile information
 *
 * Everything runs on a background-priority thread; the game thread only stores timestamps.
 */
public final class BenchmarkRecorder {
    private static final String TAG = "BenchmarkRecorder";
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

        long nextFrame = frameStats.getFrameCount();
        long startNanos = System.nanoTime();
        long lastTimestamp = -1;
        long lastSensorNanos = 0;
        long framesAtLastSensor = nextFrame;

        try (BufferedWriter frames = new BufferedWriter(new FileWriter(new File(outputDir, "frames.csv")));
             BufferedWriter sensors = new BufferedWriter(new FileWriter(new File(outputDir, "sensors.csv")))) {
            frames.write("t_ms,frametime_ms\n");
            StringBuilder header = new StringBuilder("t_ms,fps,cpu_temp_c,gpu_temp_c,battery_temp_c,thermal_status,thermal_headroom,mem_used_mb,mem_avail_mb");
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
                    }
                    lastTimestamp = ts;
                }
                nextFrame += count;

                long now = System.nanoTime();
                if (now - lastSensorNanos >= 1_000_000_000L) {
                    long frameDelta = frameStats.getFrameCount() - framesAtLastSensor;
                    double fps = lastSensorNanos > 0 ? frameDelta * 1e9 / (now - lastSensorNanos) : 0;
                    if (lastSensorNanos > 0) perSecondFps.add(fps);
                    framesAtLastSensor = frameStats.getFrameCount();
                    lastSensorNanos = now;

                    activityManager.getMemoryInfo(memoryInfo);
                    StringBuilder row = new StringBuilder();
                    row.append(String.format(Locale.ENGLISH, "%.0f,%.2f,%s,%s,%s,%d,%s,%d,%d",
                        (now - startNanos) / 1e6, fps,
                        fmt(thermalMonitor.getCpuTemperature()), fmt(thermalMonitor.getGpuTemperature()), fmt(thermalMonitor.getBatteryTemperature()),
                        thermalMonitor.getThermalStatus(), fmt(thermalMonitor.getThermalHeadroom()),
                        (memoryInfo.totalMem - memoryInfo.availMem) >> 20, memoryInfo.availMem >> 20));
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

        writeSummary(allFrametimes, perSecondFps, (System.nanoTime() - startNanos) / 1e9);
    }

    private void writeSummary(List<Double> frametimes, List<Double> perSecondFps, double seconds) {
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
