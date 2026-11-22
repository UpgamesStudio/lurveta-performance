package com.winlator.core;

import android.os.Process;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * LURVETA Performance — CPU and memory of every process of this app's uid: the Android app itself
 * plus the Wine, Box64 and wineserver processes it starts (they all run under the same uid, so
 * their /proc entries are readable without root).
 *
 * cpuPercent: CPU time used since the previous sample / wall time (100 = one core fully busy).
 * rssMb: resident memory of all those processes. Called once per second from a background thread.
 */
public final class ProcessSampler {
    public static final class Sample {
        public float cpuPercent;
        public long rssMb;
        public int processes;
        public String topProcess = "";
        public float topProcessCpuPercent;
    }

    private final int uid = Process.myUid();
    private final long clockTicks;
    private final long pageSize;
    private final Map<Integer, Long> lastTicks = new HashMap<>();
    private final byte[] buffer = new byte[1024];
    private long lastSampleNanos = 0;

    public ProcessSampler() {
        long ticks = 100, page = 4096;
        try {
            ticks = Os.sysconf(OsConstants._SC_CLK_TCK);
            page = Os.sysconf(OsConstants._SC_PAGESIZE);
        }
        catch (Throwable ignored) {}
        clockTicks = ticks > 0 ? ticks : 100;
        pageSize = page > 0 ? page : 4096;
    }

    public Sample sample() {
        Sample sample = new Sample();
        long now = System.nanoTime();
        double elapsedSeconds = lastSampleNanos > 0 ? (now - lastSampleNanos) / 1e9 : 0;
        lastSampleNanos = now;

        Map<Integer, Long> current = new HashMap<>();
        long totalDeltaTicks = 0, rssPages = 0, topDelta = -1;
        File[] entries = new File("/proc").listFiles();
        if (entries == null) return sample;

        for (File entry : entries) {
            String name = entry.getName();
            if (name.isEmpty() || !Character.isDigit(name.charAt(0))) continue;
            try {
                if (Os.stat(entry.getPath()).st_uid != uid) continue;
                int pid = Integer.parseInt(name);

                String stat = read(entry.getPath() + "/stat");
                int open = stat.indexOf('('), close = stat.lastIndexOf(')');
                if (open < 0 || close < 0 || close + 2 >= stat.length()) continue;
                String comm = stat.substring(open + 1, close);
                String[] fields = stat.substring(close + 2).split(" ");
                long ticks = Long.parseLong(fields[11]) + Long.parseLong(fields[12]); // utime + stime

                String statm = read(entry.getPath() + "/statm");
                String[] memFields = statm.trim().split(" ");
                if (memFields.length > 1) rssPages += Long.parseLong(memFields[1]);

                current.put(pid, ticks);
                Long previous = lastTicks.get(pid);
                long delta = previous != null ? Math.max(0, ticks - previous) : 0;
                totalDeltaTicks += delta;
                if (delta > topDelta) {
                    topDelta = delta;
                    sample.topProcess = comm;
                }
                sample.processes++;
            }
            catch (Exception ignored) {
                // The process ended while being read, or the entry is not accessible.
            }
        }

        lastTicks.clear();
        lastTicks.putAll(current);
        sample.rssMb = (rssPages * pageSize) >> 20;
        if (elapsedSeconds > 0) {
            sample.cpuPercent = (float)(100.0 * totalDeltaTicks / clockTicks / elapsedSeconds);
            sample.topProcessCpuPercent = (float)(100.0 * Math.max(0, topDelta) / clockTicks / elapsedSeconds);
        }
        return sample;
    }

    private String read(String path) throws IOException {
        try (FileInputStream in = new FileInputStream(path)) {
            int length = in.read(buffer);
            return length > 0 ? new String(buffer, 0, length, StandardCharsets.US_ASCII) : "";
        }
    }
}
