package com.winlator.core;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;

import java.io.File;
import java.util.Locale;

/**
 * LURVETA Performance — thermal monitoring (read-only; never tries to bypass protections).
 *
 * Sources, in order of reliability on non-rooted Android:
 *  - PowerManager thermal status (API 29+) and thermal headroom (API 30+): always allowed;
 *  - battery temperature from the sticky ACTION_BATTERY_CHANGED intent: always allowed;
 *  - /sys/class/thermal zones whose type looks like CPU/SoC or GPU: often blocked by SELinux on
 *    recent devices, in which case they are reported as unavailable (NaN).
 */
public final class ThermalMonitor {
    private final Context context;
    private final PowerManager powerManager;
    private File cpuZone;
    private File gpuZone;
    private boolean zonesScanned = false;

    public ThermalMonitor(Context context) {
        this.context = context.getApplicationContext();
        this.powerManager = (PowerManager)context.getSystemService(Context.POWER_SERVICE);
    }

    /** PowerManager.THERMAL_STATUS_* (0 none ... 6 shutdown) or -1 if unavailable. */
    public int getThermalStatus() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && powerManager != null) return powerManager.getCurrentThermalStatus();
        return -1;
    }

    /** Forecast headroom (1.0 = throttling threshold) or NaN if unavailable. */
    public float getThermalHeadroom() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && powerManager != null) {
            try {
                return powerManager.getThermalHeadroom(10);
            }
            catch (Throwable ignored) {}
        }
        return Float.NaN;
    }

    public float getBatteryTemperature() {
        try {
            Intent intent = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (intent != null) {
                int tenths = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
                if (tenths != Integer.MIN_VALUE) return tenths / 10f;
            }
        }
        catch (Throwable ignored) {}
        return Float.NaN;
    }

    public float getCpuTemperature() {
        scanZones();
        return readZone(cpuZone);
    }

    public float getGpuTemperature() {
        scanZones();
        return readZone(gpuZone);
    }

    public static String formatStatus(int status) {
        switch (status) {
            case 0: return "OK";
            case 1: return "LIGHT";
            case 2: return "MODERATE";
            case 3: return "SEVERE";
            case 4: return "CRITICAL";
            case 5: return "EMERGENCY";
            case 6: return "SHUTDOWN";
            default: return "N/A";
        }
    }

    private synchronized void scanZones() {
        if (zonesScanned) return;
        zonesScanned = true;
        File[] zones = new File("/sys/class/thermal").listFiles((dir, name) -> name.startsWith("thermal_zone"));
        if (zones == null) return;

        for (File zone : zones) {
            String type = readFirstLine(new File(zone, "type")).toLowerCase(Locale.ENGLISH);
            if (type.isEmpty() || Float.isNaN(readZone(zone))) continue;
            if (gpuZone == null && type.contains("gpu")) gpuZone = zone;
            else if (cpuZone == null && (type.contains("soc_max") || type.contains("tscpu") || type.startsWith("cpu") || type.contains("cpu-"))) cpuZone = zone;
        }
    }

    private static float readZone(File zone) {
        if (zone == null) return Float.NaN;
        String value = readFirstLine(new File(zone, "temp"));
        if (value.isEmpty()) return Float.NaN;
        try {
            float raw = Float.parseFloat(value);
            float celsius = raw > 1000 ? raw / 1000f : (raw > 200 ? raw / 10f : raw);
            return celsius > -40 && celsius < 150 ? celsius : Float.NaN;
        }
        catch (NumberFormatException e) {
            return Float.NaN;
        }
    }

    private static String readFirstLine(File file) {
        try {
            if (!file.canRead()) return "";
            java.util.List<String> lines = FileUtils.readLines(file);
            return lines.isEmpty() ? "" : lines.get(0).trim();
        }
        catch (Throwable e) {
            return "";
        }
    }
}
