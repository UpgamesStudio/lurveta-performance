import com.winlator.core.GpuSampler;
import com.winlator.core.ProcessSampler;

/** Host test of ProcessSampler (real Linux /proc) and GpuSampler (must degrade to "not readable"). */
public class SamplersTest {
    public static void main(String[] args) throws Exception {
        int fails = 0;
        ProcessSampler sampler = new ProcessSampler();
        sampler.sample();
        // A child process of the same uid that keeps one core busy (like a Wine process would).
        Process busy = new ProcessBuilder("sh", "-c", "while :; do :; done").start();
        sampler.sample();
        Thread.sleep(2000);
        ProcessSampler.Sample s = sampler.sample();
        busy.destroyForcibly();
        System.out.printf("same-uid processes=%d cpu=%.0f%% rss=%d MB top=%s (%.0f%%)%n", s.processes, s.cpuPercent, s.rssMb, s.topProcess, s.topProcessCpuPercent);
        if (s.processes < 2) { System.out.println("FAIL: expected at least the JVM and the busy child"); fails++; }
        if (s.cpuPercent < 60) { System.out.println("FAIL: a busy child should show ~100% of one core"); fails++; }
        if (!"sh".equals(s.topProcess)) { System.out.println("FAIL: top process should be the busy child (sh)"); fails++; }
        if (s.rssMb <= 0) { System.out.println("FAIL: rss should be > 0"); fails++; }

        GpuSampler gpu = new GpuSampler();
        float load = gpu.getLoadPercent(), freq = gpu.getFrequencyMhz();
        System.out.println("gpu: " + gpu.describeSources() + " -> load=" + load + " freq=" + freq);
        if (!Float.isNaN(load) && (load < 0 || load > 100)) { System.out.println("FAIL: load out of range"); fails++; }

        System.out.println(fails == 0 ? "ALL SAMPLER TESTS PASSED" : fails + " FAILURES");
        System.exit(fails == 0 ? 0 : 1);
    }
}
