package android.os;
/** Host stub: only what ProcessSampler uses. */
public class Process {
    public static int myUid() {
        try { return (Integer)java.nio.file.Files.getAttribute(java.nio.file.Paths.get("/proc/self"), "unix:uid"); }
        catch (Exception e) { return -1; }
    }
}
