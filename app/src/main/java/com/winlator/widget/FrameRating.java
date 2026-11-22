package com.winlator.widget;

import android.app.ActivityManager;
import android.content.Context;
import android.os.SystemClock;
import android.util.AttributeSet;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.winlator.R;
import com.winlator.box64.Box64Utils;
import com.winlator.core.CPUStatus;
import com.winlator.core.FrameStats;
import com.winlator.core.HardwareInfo;
import com.winlator.core.StringUtils;
import com.winlator.core.ThermalMonitor;

import java.util.Locale;

/**
 * Performance HUD. LURVETA additions: frametime + 1%/0.1% lows (10 s window), temperature and
 * thermal status, Vulkan GPU name, benchmark REC indicator. The per-frame cost on the game path is
 * a single FrameStats.onFrame() call; everything else runs at 2 Hz on the UI thread.
 */
public class FrameRating extends FrameLayout implements Runnable {
    public enum Mode {DISABLED, SIMPLE, FULL}
    private static final long LOWS_WINDOW_NANOS = 10_000_000_000L;
    private long lastTime = 0;
    private short frameCount = 0;
    private float lastFPS = 0;
    private final LinearLayout fpsPanel;
    private final LinearLayout lowsPanel;
    private final LinearLayout gpuPanel;
    private final LinearLayout ramPanel;
    private final LinearLayout cpuPanel;
    private final LinearLayout tempPanel;
    private final TextView fpsLabel;
    private Mode mode = Mode.SIMPLE;
    private ActivityManager activityManager;
    private ActivityManager.MemoryInfo memoryInfo;
    private ThermalMonitor thermalMonitor;
    private String cpuInfo = null;
    private byte tick = 0;
    private final FrameStats frameStats = new FrameStats();
    private volatile boolean recording = false;

    public FrameRating(Context context) {
        this(context, null);
    }

    public FrameRating(Context context, AttributeSet attrs) {
        this(context, attrs, 0);
    }

    public FrameRating(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);

        View view = LayoutInflater.from(context).inflate(R.layout.frame_rating, this, false);
        fpsPanel = view.findViewById(R.id.LLFPSPanel);
        lowsPanel = view.findViewById(R.id.LLLowsPanel);
        gpuPanel = view.findViewById(R.id.LLGPUPanel);
        ramPanel = view.findViewById(R.id.LLRAMPanel);
        cpuPanel = view.findViewById(R.id.LLCPUPanel);
        tempPanel = view.findViewById(R.id.LLTempPanel);
        fpsLabel = (TextView)fpsPanel.getChildAt(0);
        addView(view);
        setupPanels();
    }

    private void setupPanels() {
        boolean full = mode == Mode.FULL;
        fpsPanel.setVisibility(mode != Mode.DISABLED ? VISIBLE : GONE);
        lowsPanel.setVisibility(full ? VISIBLE : GONE);
        gpuPanel.setVisibility(full ? VISIBLE : GONE);
        ramPanel.setVisibility(full ? VISIBLE : GONE);
        cpuPanel.setVisibility(full ? VISIBLE : GONE);
        tempPanel.setVisibility(full ? VISIBLE : GONE);

        if (full) {
            Context context = getContext();
            activityManager = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
            memoryInfo = new ActivityManager.MemoryInfo();
            thermalMonitor = new ThermalMonitor(context);
        }
        else {
            activityManager = null;
            memoryInfo = null;
            thermalMonitor = null;
        }
    }

    public Mode getMode() {
        return mode;
    }

    public void setMode(Mode mode) {
        this.mode = mode;
        setupPanels();
    }

    public FrameStats getFrameStats() {
        return frameStats;
    }

    public void setRecording(boolean recording) {
        this.recording = recording;
        post(() -> {
            fpsLabel.setText(recording ? "● REC FPS:" : "FPS:");
            fpsLabel.setTextColor(recording ? 0xffff5252 : 0xff00c8f5);
            if (mode == Mode.DISABLED) setVisibility(recording ? VISIBLE : GONE);
        });
    }

    public void setGPUInfo(String gpuInfo) {
        if (gpuInfo == null || gpuInfo.isEmpty() || gpuInfo.equals("N/A")) {
            HardwareInfo.Gpu gpu = HardwareInfo.getGpu(getContext());
            if (!gpu.deviceName.isEmpty()) gpuInfo = gpu.deviceName;
        }
        final String text = gpuInfo;
        post(() -> ((TextView)gpuPanel.getChildAt(1)).setText(text));
    }

    public void reset() {
        frameCount = 0;
        lastTime = SystemClock.elapsedRealtime();
        lastFPS = 0;
        tick = 2;
    }

    /** Called for every presented frame (game thread): keep it trivial. */
    public void update() {
        frameStats.onFrame();
        if (mode == Mode.DISABLED && !recording) return;

        long time = SystemClock.elapsedRealtime();
        if (time >= lastTime + 500) {
            lastFPS = ((float)(frameCount * 1000) / (time - lastTime));
            post(this);
            lastTime = time;
            frameCount = 0;
        }

        frameCount++;
    }

    @Override
    public void run() {
        if (mode == Mode.DISABLED && !recording) return;
        if (getVisibility() == GONE) setVisibility(View.VISIBLE);
        ((TextView)fpsPanel.getChildAt(1)).setText(String.format(Locale.ENGLISH, "%.1f", lastFPS));
        if (mode != Mode.FULL) return;

        FrameStats.Summary summary = frameStats.summarize(LOWS_WINDOW_NANOS);
        if (summary != null) {
            ((TextView)lowsPanel.getChildAt(1)).setText(String.format(Locale.ENGLISH, "%.1f | 0.1%%: %.1f | %.1f ms", summary.low1Fps, summary.low01Fps, summary.averageFrametimeMs));
        }

        if (++tick >= 2) {
            tick = 0;
            activityManager.getMemoryInfo(memoryInfo);
            long usedMem = memoryInfo.totalMem - memoryInfo.availMem;
            String ramText = StringUtils.formatBytes(usedMem, false)+"/"+StringUtils.formatBytes(memoryInfo.totalMem);
            ((TextView)ramPanel.getChildAt(1)).setText(ramText);

            if (cpuInfo == null) {
                cpuInfo = "Box64 v"+ Box64Utils.extractBinVersion(cpuPanel.getContext());
            }

            short[] clockSpeeds = CPUStatus.getCurrentClockSpeeds();
            int maxClockSpeed = 0;
            for (short clockSpeed : clockSpeeds) maxClockSpeed = Math.max(maxClockSpeed, clockSpeed);
            ((TextView)cpuPanel.getChildAt(1)).setText(CPUStatus.formatClockSpeed(maxClockSpeed)+" | "+cpuInfo);

            float cpuTemp = thermalMonitor.getCpuTemperature();
            float batteryTemp = thermalMonitor.getBatteryTemperature();
            StringBuilder temp = new StringBuilder();
            if (!Float.isNaN(cpuTemp)) temp.append(String.format(Locale.ENGLISH, "CPU %.0f°C | ", cpuTemp));
            if (!Float.isNaN(batteryTemp)) temp.append(String.format(Locale.ENGLISH, "BAT %.0f°C | ", batteryTemp));
            temp.append(ThermalMonitor.formatStatus(thermalMonitor.getThermalStatus()));
            ((TextView)tempPanel.getChildAt(1)).setText(temp.toString());
        }
    }
}
