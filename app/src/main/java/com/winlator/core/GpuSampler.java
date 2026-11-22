package com.winlator.core;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * LURVETA Performance — GPU load and frequency, when the device lets an app read them.
 *
 * Android usually blocks these kernel nodes for apps (SELinux), so the sampler probes the known
 * locations once (MediaTek GED, ARM Mali kbase, generic kgsl/devfreq) and reports "not readable"
 * instead of inventing a value. Nothing here is required by the rest of the recorder.
 */
public final class GpuSampler {
    private static final String[] LOAD_PATHS = {
        "/sys/kernel/ged/hal/gpu_utilization",   // MediaTek GED: "loading block idle"
        "/sys/class/misc/mali0/device/utilization",
        "/sys/kernel/gpu/gpu_busy",              // "NN %"
        "/proc/mali/utilization",
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage"
    };
    private static final String[] FREQ_PATHS = {
        "/sys/kernel/ged/hal/current_freqency",  // (sic) MediaTek GED
        "/sys/class/misc/mali0/device/cur_freq",
        "/sys/kernel/gpu/gpu_clock",
        "/sys/class/kgsl/kgsl-3d0/gpuclk"
    };
    private static final Pattern NUMBER = Pattern.compile("\\d+");

    private final String loadPath;
    private final String freqPath;

    public GpuSampler() {
        loadPath = firstReadable(LOAD_PATHS, findDevfreq("load"));
        freqPath = firstReadable(FREQ_PATHS, findDevfreq("cur_freq"));
    }

    public String describeSources() {
        return "load " + (loadPath != null ? loadPath : "not readable") + " | freq " + (freqPath != null ? freqPath : "not readable");
    }

    /** GPU busy percentage, or NaN when not readable. */
    public float getLoadPercent() {
        if (loadPath == null) return Float.NaN;
        long value = firstNumber(readLine(loadPath));
        return value >= 0 && value <= 100 ? value : Float.NaN;
    }

    /** Current GPU frequency in MHz, or NaN when not readable. */
    public float getFrequencyMhz() {
        if (freqPath == null) return Float.NaN;
        long value = largestNumber(readLine(freqPath));
        if (value <= 0) return Float.NaN;
        if (value > 10_000_000L) return value / 1_000_000f; // Hz
        if (value > 10_000L) return value / 1000f;           // kHz
        return value;                                       // MHz
    }

    private static String findDevfreq(String node) {
        File[] devices = new File("/sys/class/devfreq").listFiles();
        if (devices == null) return null;
        for (File device : devices) {
            String name = device.getName().toLowerCase();
            if (name.contains("mali") || name.contains("gpu") || name.contains("kgsl")) {
                File file = new File(device, node);
                if (file.canRead()) return file.getPath();
            }
        }
        return null;
    }

    private static String firstReadable(String[] paths, String fallback) {
        for (String path : paths) if (readLine(path) != null) return path;
        return fallback != null && readLine(fallback) != null ? fallback : null;
    }

    private static String readLine(String path) {
        try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
            String line = reader.readLine();
            return line != null && !line.trim().isEmpty() ? line : null;
        }
        catch (Exception e) {
            return null;
        }
    }

    private static long firstNumber(String text) {
        if (text == null) return -1;
        Matcher matcher = NUMBER.matcher(text);
        return matcher.find() ? Long.parseLong(matcher.group()) : -1;
    }

    private static long largestNumber(String text) {
        if (text == null) return -1;
        long max = -1;
        Matcher matcher = NUMBER.matcher(text);
        while (matcher.find()) {
            try {
                max = Math.max(max, Long.parseLong(matcher.group()));
            }
            catch (NumberFormatException ignored) {}
        }
        return max;
    }
}
