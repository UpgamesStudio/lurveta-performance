package com.winlator.core;

import java.util.Arrays;

/**
 * LURVETA Performance — frame timing statistics.
 *
 * onFrame() is called for every presented frame from the Vortek request thread (the critical path
 * of the game), so it only stores one System.nanoTime() in a ring of primitives: no locks, no
 * allocation. Statistics are computed on demand by the HUD / benchmark threads.
 *
 * Definitions (documented in docs/BENCHMARKING.md):
 *  - average FPS   = frames / elapsed time of the window;
 *  - 1% / 0.1% low = 1000 / (mean of the slowest 1% / 0.1% frametimes), the convention used by
 *                    CapFrameX/PresentMon-style tools ("low average");
 *  - percentiles   = nearest-rank percentiles of the frametimes.
 */
public final class FrameStats {
    private static final int CAPACITY = 1 << 14; // 16384 frames (~4.5 min at 60 FPS)
    private static final int MASK = CAPACITY - 1;
    private final long[] timestamps = new long[CAPACITY];
    private volatile long frameCount = 0;

    public static final class Summary {
        public int frames;
        public double durationMs;
        public double averageFps;
        public double low1Fps;
        public double low01Fps;
        public double minFps;     // from the slowest frame
        public double maxFps;     // from the fastest frame
        public double averageFrametimeMs;
        public double p50Ms, p95Ms, p99Ms, maxFrametimeMs;
        public double frametimeStdDevMs;
    }

    /** Called once per presented frame. Must stay allocation- and lock-free. */
    public void onFrame() {
        long index = frameCount;
        timestamps[(int)(index & MASK)] = System.nanoTime();
        frameCount = index + 1;
    }

    public void reset() {
        frameCount = 0;
    }

    public long getFrameCount() {
        return frameCount;
    }

    /**
     * Copies the timestamps of frames [fromFrame, current) into out (oldest first) and returns the
     * number copied. Frames older than the ring capacity are lost (returns only what is retained).
     */
    public int copyTimestamps(long fromFrame, long[] out) {
        long end = frameCount;
        long start = Math.max(fromFrame, Math.max(0, end - CAPACITY + 1));
        int count = (int)Math.min(out.length, end - start);
        for (int i = 0; i < count; i++) out[i] = timestamps[(int)((start + i) & MASK)];
        return count;
    }

    /** Statistics of the frames presented during the last windowNanos (needs >= 2 frames). */
    public Summary summarize(long windowNanos) {
        long end = frameCount;
        int available = (int)Math.min(end, CAPACITY - 1);
        if (available < 2) return null;

        long now = timestamps[(int)((end - 1) & MASK)];
        int count = 1;
        while (count < available && now - timestamps[(int)((end - 1 - count) & MASK)] <= windowNanos) count++;
        if (count < 2) return null;

        double[] frametimes = new double[count - 1];
        for (int i = 0; i < frametimes.length; i++) {
            long a = timestamps[(int)((end - count + i) & MASK)];
            long b = timestamps[(int)((end - count + i + 1) & MASK)];
            frametimes[i] = (b - a) / 1e6;
        }
        return summarize(frametimes);
    }

    /** Statistics of an explicit list of frametimes in milliseconds. */
    public static Summary summarize(double[] frametimesMs) {
        if (frametimesMs.length == 0) return null;
        double[] sorted = frametimesMs.clone();
        Arrays.sort(sorted);

        Summary s = new Summary();
        s.frames = sorted.length;
        double total = 0;
        for (double ft : sorted) total += ft;
        s.durationMs = total;
        s.averageFrametimeMs = total / sorted.length;
        s.averageFps = total > 0 ? 1000.0 * sorted.length / total : 0;
        s.low1Fps = lowAverageFps(sorted, 0.01);
        s.low01Fps = lowAverageFps(sorted, 0.001);
        s.maxFrametimeMs = sorted[sorted.length - 1];
        s.minFps = s.maxFrametimeMs > 0 ? 1000.0 / s.maxFrametimeMs : 0;
        s.maxFps = sorted[0] > 0 ? 1000.0 / sorted[0] : 0;
        s.p50Ms = percentile(sorted, 0.50);
        s.p95Ms = percentile(sorted, 0.95);
        s.p99Ms = percentile(sorted, 0.99);

        double variance = 0;
        for (double ft : sorted) variance += (ft - s.averageFrametimeMs) * (ft - s.averageFrametimeMs);
        s.frametimeStdDevMs = Math.sqrt(variance / sorted.length);
        return s;
    }

    private static double lowAverageFps(double[] sortedAscending, double fraction) {
        int n = Math.max(1, (int)Math.ceil(sortedAscending.length * fraction));
        double sum = 0;
        for (int i = sortedAscending.length - n; i < sortedAscending.length; i++) sum += sortedAscending[i];
        double mean = sum / n;
        return mean > 0 ? 1000.0 / mean : 0;
    }

    private static double percentile(double[] sortedAscending, double p) {
        int rank = (int)Math.ceil(p * sortedAscending.length);
        return sortedAscending[Math.min(sortedAscending.length - 1, Math.max(0, rank - 1))];
    }
}
