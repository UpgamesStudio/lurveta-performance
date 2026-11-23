package android.system;
/** Host stub backed by the real Linux /proc and getconf. */
public class Os {
    public static StructStat stat(String path) throws Exception {
        return new StructStat((Integer)java.nio.file.Files.getAttribute(java.nio.file.Paths.get(path), "unix:uid"));
    }
    public static String readlink(String path) throws Exception {
        return java.nio.file.Files.readSymbolicLink(java.nio.file.Paths.get(path)).toString();
    }
    public static long sysconf(int name) {
        String key = name == OsConstants._SC_CLK_TCK ? "CLK_TCK" : "PAGESIZE";
        try {
            java.lang.Process p = new ProcessBuilder("getconf", key).start();
            return Long.parseLong(new String(p.getInputStream().readAllBytes()).trim());
        } catch (Exception e) { return name == OsConstants._SC_CLK_TCK ? 100 : 4096; }
    }
}
