import com.winlator.core.LaunchTrace;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

/**
 * Runs the REAL LaunchTrace on the host against a simulated GTA IV black screen:
 *  - "GTAIV.exe": a process with one busy thread; "gta4Browser.exe": a sleeping process;
 *  - Vortek reached instance + device but never presented, and its server is stuck in vkWaitForFences.
 * The report must show all of it. Run with -Dtest.filesDir=<empty dir>.
 */
public class LaunchTraceTest {
    static int fails = 0;
    static void check(boolean c, String m) { if (!c) { System.out.println("FAIL: " + m); fails++; } }

    public static void main(String[] args) throws Exception {
        File files = new File(System.getProperty("test.filesDir"));
        File logs = new File(files, "logs");
        logs.mkdirs();
        File bin = new File(files, "bin");
        bin.mkdirs();
        Files.createSymbolicLink(Paths.get(bin.getPath(), "GTAIV.exe"), Paths.get("/usr/bin/python3"));
        Files.createSymbolicLink(Paths.get(bin.getPath(), "gta4Browser.exe"), Paths.get("/bin/sleep"));

        long now = System.currentTimeMillis() / 1000;
        Files.write(Paths.get(logs.getPath(), "vortek-diagnostics.log"), (
            (now - 100) + " INFO STAGE first-present (an OLD session: must be ignored)\n" +
            now + " INFO STAGE client-connected (upload mode 0, pipeline cache 1)\n" +
            now + " INFO STAGE instance-created: app 'GTAIV.exe' engine 'DXVK' (engine version 0x1) requested Vulkan 0x403000\n" +
            now + " INFO STAGE device-created: device 'Mali-G615 MC6'\n" +
            now + " FAIL vkCreateSwapchainKHR -> VK_ERROR (-1000000000) \n").getBytes());
        Files.write(Paths.get(logs.getPath(), "vortek-activity.txt"),
            (now + " requests=4242 submits=17 presents=0 swapchains=0 images=311 allocMB=640 state=IN_CALL call=vkWaitForFences forMs=9000\n").getBytes());

        LaunchTrace.begin(new android.content.Context(), "GTA IV (simulated)", "GTAIV.exe", Arrays.asList("container: test"));
        Process game = new ProcessBuilder(bin + "/GTAIV.exe", "-c", "while True: pass").start();
        Process browser = new ProcessBuilder(bin + "/gta4Browser.exe", "60").start();
        Thread.sleep(13000);
        game.destroyForcibly();
        browser.destroyForcibly();
        boolean abnormal = LaunchTrace.end(137);

        String report = new String(Files.readAllBytes(Paths.get(files.getPath(), "logs/launch/launch-latest.txt")));
        System.out.println(report.substring(0, Math.min(report.length(), 4500)));
        check(report.contains("4=yes"), "game process stage detected from the process name");
        check(report.contains("7=yes") && report.contains("8=NO"), "device created, no frame (old session line ignored)");
        check(report.contains("ningún fotograma"), "verdict: device but no frame");
        check(report.contains("SIGKILL"), "exit signal decoded");
        check(report.contains("SERVER BLOCKED INSIDE THIS VULKAN CALL") && report.contains("vkWaitForFences"), "stuck Vulkan call flagged");
        check(report.matches("(?s).*probe GTAIV\\.exe pid \\d+: cpu (9\\d|1\\d\\d)%.*"), "busy game thread measured (~100% CPU)");
        check(report.matches("(?s).*probe gta4Browser\\.exe pid \\d+: cpu 0%.*IDLE/WAITING.*"), "sleeping helper reported idle");
        check(Arrays.stream(report.split("\n")).noneMatch(l -> l.contains("probe GTAIV.exe") && l.contains("IDLE")), "a busy process is never reported idle (first sample has no baseline)");
        check(report.contains("failed Vulkan calls: 1"), "Vortek failure counted");
        check(new File(files, "ext/Download/LURVETA/logs/launch-latest.txt").isFile(), "public copy written");
        check(abnormal && LaunchTrace.takePendingNotice(new android.content.Context()) != null, "menu notice pending");
        System.out.println(fails == 0 ? "ALL LAUNCH TRACE TESTS PASSED" : fails + " FAILURES");
        System.exit(fails == 0 ? 0 : 1);
    }
}
