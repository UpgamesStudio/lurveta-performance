package com.winlator.core;

import android.app.ActivityManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

import androidx.preference.PreferenceManager;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * LURVETA Performance — hardware detection.
 *
 * GPU: read from the Vulkan driver (vendorID / driverID / deviceName), not guessed from the SoC
 * vendor: a MediaTek Dimensity 8350 reports an ARM Mali-G615 and gets the Mali profile.
 * CPU: clusters grouped by cpuinfo_max_freq, core types from /proc/cpuinfo "CPU part".
 */
public final class HardwareInfo {
    public enum GpuFamily {
        ARM_MALI("ARM Mali"), ARM_IMMORTALIS("ARM Immortalis"), QUALCOMM_ADRENO("Qualcomm Adreno"),
        IMAGINATION_POWERVR("Imagination PowerVR"), SAMSUNG_XCLIPSE("Samsung Xclipse"), OTHER("Other"), UNKNOWN("Unknown");

        public final String label;
        GpuFamily(String label) { this.label = label; }
    }

    public static final int VENDOR_ARM = 0x13B5;
    public static final int VENDOR_QUALCOMM = 0x5143;
    public static final int VENDOR_IMAGINATION = 0x1010;
    public static final int VENDOR_SAMSUNG = 0x144D;
    // VkDriverId values (Vulkan 1.2).
    public static final int DRIVER_ID_IMAGINATION_PROPRIETARY = 7;
    public static final int DRIVER_ID_QUALCOMM_PROPRIETARY = 8;
    public static final int DRIVER_ID_ARM_PROPRIETARY = 9;
    public static final int DRIVER_ID_SAMSUNG_PROPRIETARY = 21;

    public static final class Gpu {
        public int vendorId;
        public int deviceId;
        public String deviceName = "";
        public int driverId;
        public String driverName = "";
        public String driverInfo = "";
        public int apiVersion;
        public int driverVersion;
        public boolean textureCompressionBC;
        public boolean textureCompressionETC2;
        public boolean textureCompressionASTC;
        public GpuFamily family = GpuFamily.UNKNOWN;

        public boolean isMali() {
            return family == GpuFamily.ARM_MALI || family == GpuFamily.ARM_IMMORTALIS;
        }

        public String getApiVersionString() {
            return GPUHelper.vkVersionMajor(apiVersion)+"."+GPUHelper.vkVersionMinor(apiVersion)+"."+GPUHelper.vkVersionPatch(apiVersion);
        }
    }

    public static final class CpuCluster {
        public int maxFreqKHz;
        public final List<Integer> cpus = new ArrayList<>();
        public String coreName = "";

        @Override
        public String toString() {
            return cpus.size()+"× "+(coreName.isEmpty() ? "" : coreName+" ")+String.format(Locale.ENGLISH, "%.2f GHz", maxFreqKHz / 1e6f);
        }
    }

    private static Gpu cachedGpu;

    private HardwareInfo() {}

    /** Vulkan GPU identity, cached per OS build (a driver update changes Build.FINGERPRINT). */
    public static synchronized Gpu getGpu(Context context) {
        if (cachedGpu != null) return cachedGpu;
        SharedPreferences preferences = PreferenceManager.getDefaultSharedPreferences(context);
        String key = "lurveta_gpu_info:"+Build.FINGERPRINT;
        String stored = preferences.getString("lurveta_gpu_info", "");
        String[] lines;
        if (stored.startsWith(key+"\n")) {
            lines = stored.substring(key.length() + 1).split("\n");
        }
        else {
            lines = GPUHelper.vkGetDeviceInfo();
            if (lines.length > 0) preferences.edit().putString("lurveta_gpu_info", key+"\n"+String.join("\n", lines)).apply();
        }
        cachedGpu = parseGpu(lines);
        return cachedGpu;
    }

    static Gpu parseGpu(String[] lines) {
        Map<String, String> values = new LinkedHashMap<>();
        for (String line : lines) {
            int index = line.indexOf('=');
            if (index > 0) values.put(line.substring(0, index), line.substring(index + 1));
        }

        Gpu gpu = new Gpu();
        gpu.vendorId = parseInt(values.get("vendorID"));
        gpu.deviceId = parseInt(values.get("deviceID"));
        gpu.deviceName = valueOrEmpty(values.get("deviceName"));
        gpu.driverId = parseInt(values.get("driverID"));
        gpu.driverName = valueOrEmpty(values.get("driverName"));
        gpu.driverInfo = valueOrEmpty(values.get("driverInfo"));
        gpu.apiVersion = parseInt(values.get("apiVersion"));
        gpu.driverVersion = parseInt(values.get("driverVersion"));
        gpu.textureCompressionBC = "1".equals(values.get("textureCompressionBC"));
        gpu.textureCompressionETC2 = "1".equals(values.get("textureCompressionETC2"));
        gpu.textureCompressionASTC = "1".equals(values.get("textureCompressionASTC_LDR"));
        gpu.family = classify(gpu.vendorId, gpu.driverId, gpu.deviceName, !values.isEmpty());
        return gpu;
    }

    static GpuFamily classify(int vendorId, int driverId, String deviceName, boolean known) {
        if (!known) return GpuFamily.UNKNOWN;
        String name = deviceName.toLowerCase(Locale.ENGLISH);
        if (vendorId == VENDOR_ARM || driverId == DRIVER_ID_ARM_PROPRIETARY) {
            return name.contains("immortalis") ? GpuFamily.ARM_IMMORTALIS : GpuFamily.ARM_MALI;
        }
        if (vendorId == VENDOR_QUALCOMM || driverId == DRIVER_ID_QUALCOMM_PROPRIETARY || name.contains("adreno")) return GpuFamily.QUALCOMM_ADRENO;
        if (vendorId == VENDOR_IMAGINATION || driverId == DRIVER_ID_IMAGINATION_PROPRIETARY || name.contains("powervr")) return GpuFamily.IMAGINATION_POWERVR;
        if (vendorId == VENDOR_SAMSUNG || driverId == DRIVER_ID_SAMSUNG_PROPRIETARY || name.contains("xclipse")) return GpuFamily.SAMSUNG_XCLIPSE;
        return GpuFamily.OTHER;
    }

    /** CPU clusters, fastest first. */
    public static List<CpuCluster> getCpuClusters() {
        int count = Runtime.getRuntime().availableProcessors();
        String[] partNames = readCpuPartNames(count);
        List<CpuCluster> clusters = new ArrayList<>();
        for (int i = 0; i < count; i++) {
            int maxFreq = FileUtils.readInt("/sys/devices/system/cpu/cpu"+i+"/cpufreq/cpuinfo_max_freq");
            CpuCluster cluster = null;
            for (CpuCluster c : clusters) {
                if (c.maxFreqKHz == maxFreq) {
                    cluster = c;
                    break;
                }
            }
            if (cluster == null) {
                cluster = new CpuCluster();
                cluster.maxFreqKHz = maxFreq;
                cluster.coreName = partNames[i];
                clusters.add(cluster);
            }
            cluster.cpus.add(i);
        }
        clusters.sort((a, b) -> Integer.compare(b.maxFreqKHz, a.maxFreqKHz));
        return clusters;
    }

    public static String getCpuSummary() {
        List<String> parts = new ArrayList<>();
        for (CpuCluster cluster : getCpuClusters()) parts.add(cluster.toString());
        return String.join(" + ", parts);
    }

    public static String getSocName() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            String soc = (Build.SOC_MANUFACTURER+" "+Build.SOC_MODEL).trim();
            if (!soc.isEmpty() && !soc.equalsIgnoreCase("unknown unknown")) return soc;
        }
        return Build.HARDWARE;
    }

    public static long getTotalMemory(Context context) {
        ActivityManager activityManager = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo memoryInfo = new ActivityManager.MemoryInfo();
        activityManager.getMemoryInfo(memoryInfo);
        return memoryInfo.totalMem;
    }

    /** Maps ARM "CPU part" ids from /proc/cpuinfo to core names (empty when unknown). */
    private static String[] readCpuPartNames(int count) {
        String[] names = new String[count];
        for (int i = 0; i < count; i++) names[i] = "";
        File file = new File("/proc/cpuinfo");
        if (!file.canRead()) return names;

        int current = -1;
        for (String line : FileUtils.readLines(file)) {
            if (line.startsWith("processor")) {
                current = parseInt(line.substring(line.indexOf(':') + 1).trim());
            }
            else if (line.startsWith("CPU part") && current >= 0 && current < count) {
                names[current] = armCoreName(line.substring(line.indexOf(':') + 1).trim());
            }
        }
        return names;
    }

    static String armCoreName(String part) {
        switch (part.toLowerCase(Locale.ENGLISH)) {
            case "0xd05": return "Cortex-A55";
            case "0xd0b": return "Cortex-A76";
            case "0xd0d": return "Cortex-A77";
            case "0xd41": return "Cortex-A78";
            case "0xd44": return "Cortex-X1";
            case "0xd46": return "Cortex-A510";
            case "0xd47": return "Cortex-A710";
            case "0xd48": return "Cortex-X2";
            case "0xd4b": return "Cortex-A78C";
            case "0xd4d": return "Cortex-A715";
            case "0xd4e": return "Cortex-X3";
            case "0xd80": return "Cortex-A520";
            case "0xd81": return "Cortex-A720";
            case "0xd82": return "Cortex-X4";
            case "0xd85": return "Cortex-X925";
            case "0xd87": return "Cortex-A725";
            default: return "";
        }
    }

    private static int parseInt(String value) {
        if (value == null) return 0;
        try {
            return value.startsWith("0x") ? Integer.parseInt(value.substring(2), 16) : (int)Long.parseLong(value.trim());
        }
        catch (NumberFormatException e) {
            return 0;
        }
    }

    private static String valueOrEmpty(String value) {
        return value != null ? value : "";
    }
}
