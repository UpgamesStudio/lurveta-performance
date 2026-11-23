import com.winlator.core.LaunchAnalysis;

/** Host test of the launch verdict logic: each stopping point must give the right diagnosis. */
public class LaunchAnalysisTest {
    static int fails = 0;
    static void check(boolean c, String m) { if (!c) { System.out.println("FAIL: " + m); fails++; } }
    static boolean[] upTo(int stage) {
        boolean[] r = new boolean[LaunchAnalysis.stageCount()];
        for (int i = 0; i <= stage; i++) r[i] = true;
        return r;
    }
    public static void main(String[] a) {
        check(LaunchAnalysis.describeExit(0).contains("normal"), "exit 0");
        check(LaunchAnalysis.describeExit(139).contains("SIGSEGV"), "139 = SIGSEGV");
        check(LaunchAnalysis.describeExit(137).contains("SIGKILL"), "137 = SIGKILL");
        check(LaunchAnalysis.describeExit(1).equals("código de salida 1"), "exit 1");
        check(LaunchAnalysis.describeExit(200).equals("código de salida 200"), "200 is not a signal");

        check(LaunchAnalysis.verdict("Cannot run program", upTo(0), true, 0, 10).contains("No se pudo lanzar Box64"), "exec error");
        check(LaunchAnalysis.verdict(null, upTo(1), true, 1, 900).contains("casi al instante"), "box64 dies instantly");
        check(LaunchAnalysis.verdict(null, upTo(1), true, 1, 900).contains("sin llegar a ejecutar Wine"), "box64 never ran wine");
        check(LaunchAnalysis.verdict(null, upTo(3), true, 0, 20000).contains("nunca llegó a crearse"), "wine ran, no game");
        check(LaunchAnalysis.verdict(null, upTo(4), true, 1, 20000).contains("nunca abrió Vulkan"), "game never opened Vulkan");
        check(LaunchAnalysis.verdict(null, upTo(5), true, 1, 20000).contains("no se creó el dispositivo"), "client but no device");
        check(LaunchAnalysis.verdict(null, upTo(7), true, 139, 90000).contains("ningún fotograma"), "device but no frame");
        check(LaunchAnalysis.verdict(null, upTo(7), true, 139, 90000).contains("SIGSEGV"), "crash signal reported");
        check(LaunchAnalysis.verdict(null, upTo(8), true, 0, 600000).contains("presentó fotogramas y luego terminó"), "frames then end");
        check(LaunchAnalysis.verdict(null, upTo(4), false, 0, 50000).contains("se quedó"), "still running, stuck");
        check(LaunchAnalysis.verdict(null, upTo(8), false, 0, 50000).contains("presentando fotogramas"), "running fine");

        check(LaunchAnalysis.isAbnormalEnd("x", upTo(0), 0, 1), "exec failure is abnormal");
        check(LaunchAnalysis.isAbnormalEnd(null, upTo(8), 139, 500000), "crash after frames is abnormal");
        check(!LaunchAnalysis.isAbnormalEnd(null, upTo(8), 0, 500000), "normal exit after playing is not abnormal");
        check(LaunchAnalysis.isAbnormalEnd(null, upTo(4), 0, 30000), "clean exit with no frames soon after start is abnormal");
        check(LaunchAnalysis.lastStage(new boolean[LaunchAnalysis.stageCount()]) == -1, "nothing reached");
        System.out.println(fails == 0 ? "ALL LAUNCH ANALYSIS TESTS PASSED" : fails + " FAILURES");
        System.exit(fails == 0 ? 0 : 1);
    }
}
