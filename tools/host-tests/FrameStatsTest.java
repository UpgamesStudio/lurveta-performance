import com.winlator.core.FrameStats;
public class FrameStatsTest {
  static int fails = 0;
  static void check(boolean c, String m) { if (!c) { System.out.println("FAIL: " + m); fails++; } }
  static boolean near(double a, double b) { return Math.abs(a - b) < 1e-6 * Math.max(1, Math.abs(b)); }
  public static void main(String[] a) throws Exception {
    double[] ft = new double[100]; for (int i = 0; i < 99; i++) ft[i] = 10.0; ft[99] = 100.0;
    FrameStats.Summary s = FrameStats.summarize(ft);
    check(near(s.averageFps, 100 * 1000.0 / 1090.0), "avg fps " + s.averageFps);
    check(near(s.low1Fps, 10.0), "1% low " + s.low1Fps);
    check(near(s.low01Fps, 10.0), "0.1% low " + s.low01Fps);
    check(near(s.p50Ms, 10.0) && near(s.p99Ms, 10.0) && near(s.maxFrametimeMs, 100.0), "percentiles " + s.p50Ms + " " + s.p99Ms + " " + s.maxFrametimeMs);
    check(near(s.minFps, 10.0) && near(s.maxFps, 100.0), "min/max fps");
    // 1000 frames: 990 at 16.666 ms, 10 at 50 ms -> 1% low = mean of slowest 10 = 50 ms -> 20 fps
    double[] ft2 = new double[1000]; for (int i = 0; i < 1000; i++) ft2[i] = i % 100 == 0 ? 50.0 : 16.666;
    FrameStats.Summary s2 = FrameStats.summarize(ft2);
    check(near(s2.low1Fps, 20.0), "1% low (1000 frames) " + s2.low1Fps);
    check(near(s2.low01Fps, 20.0), "0.1% low (1000 frames) " + s2.low01Fps);
    // Ring + window: 120 frames 5 ms apart -> ~200 fps
    FrameStats fs = new FrameStats();
    for (int i = 0; i < 120; i++) { fs.onFrame(); long t = System.nanoTime(); while (System.nanoTime() - t < 5_000_000L) {} }
    FrameStats.Summary w = fs.summarize(10_000_000_000L);
    check(w != null && w.frames == 119 && w.averageFps > 180 && w.averageFps < 205, "window summary " + (w == null ? "null" : w.frames + " frames " + w.averageFps + " fps"));
    long[] buf = new long[64]; int n = fs.copyTimestamps(100, buf);
    check(n == 20 && buf[19] > buf[0], "copyTimestamps " + n);
    System.out.println(fails == 0 ? "ALL FRAMESTATS TESTS PASSED" : fails + " FAILURES");
    System.exit(fails == 0 ? 0 : 1);
  }
}
