import com.winlator.core.PathRelocator;
import org.apache.commons.compress.archivers.tar.*;
import java.io.*; import java.nio.file.*;
// Mirrors TarCompressorUtils.extract(): regular files via PathRelocator.copy, symlink targets via relocate().
public class RelocTest {
  public static void main(String[] a) throws Exception {
    File dest = new File(a[1]); long files=0, repl=0, links=0;
    try (TarArchiveInputStream tar = new TarArchiveInputStream(new BufferedInputStream(new FileInputStream(a[0]), 65536))) {
      TarArchiveEntry e;
      while ((e = tar.getNextTarEntry()) != null) {
        File f = new File(dest, e.getName());
        if (e.isDirectory()) { f.mkdirs(); continue; }
        f.getParentFile().mkdirs();
        if (e.isSymbolicLink()) { Files.deleteIfExists(f.toPath()); Files.createSymbolicLink(f.toPath(), Paths.get(PathRelocator.relocate(e.getLinkName()))); links++; continue; }
        try (OutputStream o = new BufferedOutputStream(new FileOutputStream(f), 65536)) { long r = PathRelocator.copy(tar, o); if (r < 0) throw new IOException("copy failed " + f); repl += r; }
        files++;
      }
    }
    System.out.println("enabled=" + PathRelocator.BINARY_RELOCATION_ENABLED + " new=" + PathRelocator.ROOTFS_PATH + " files=" + files + " symlinks=" + links + " replacements=" + repl);
  }
}
