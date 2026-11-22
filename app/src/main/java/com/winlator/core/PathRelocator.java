package com.winlator.core;

import android.util.Log;

import com.winlator.BuildConfig;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * LURVETA Performance — rootfs path relocation.
 *
 * The prebuilt guest components (glibc rootfs, Box64, Wine, Vortek ICD, Mesa, ...) have the
 * absolute rootfs path of the upstream package compiled in: ELF interpreters (PT_INTERP),
 * RPATH/RUNPATH entries, string literals, ld.so.cache and text configuration files.
 *
 * An audit of every bundled archive showed that 100% of those occurrences share one prefix:
 *     /data/data/com.winlator/files/rootfs      (36 bytes)
 * With applicationId "com.lurveta.co" and the rootfs stored in files/"rtfs" the new absolute
 * path has exactly the same length:
 *     /data/data/com.lurveta.co/files/rtfs      (36 bytes)
 * so every occurrence can be replaced byte-for-byte without shifting any offset or NUL
 * terminator. Nothing else is touched. If a future change breaks the equal-length invariant
 * the binary patching is disabled (and logged) instead of corrupting files.
 */
public final class PathRelocator {
    private static final String TAG = "PathRelocator";

    /** Absolute rootfs path compiled into the upstream prebuilt binaries. */
    public static final String LEGACY_ROOTFS_PATH = "/data/data/com.winlator/files/rootfs";
    /** Name of the rootfs directory inside Context.getFilesDir(). */
    public static final String ROOTFS_DIR_NAME = "rtfs";
    /** Absolute rootfs path as seen by the guest processes. */
    public static final String ROOTFS_PATH = "/data/data/" + BuildConfig.APPLICATION_ID + "/files/" + ROOTFS_DIR_NAME;

    private static final byte[] NEEDLE = LEGACY_ROOTFS_PATH.getBytes(StandardCharsets.US_ASCII);
    private static final byte[] REPLACEMENT = ROOTFS_PATH.getBytes(StandardCharsets.US_ASCII);
    public static final boolean BINARY_RELOCATION_ENABLED = NEEDLE.length == REPLACEMENT.length && !LEGACY_ROOTFS_PATH.equals(ROOTFS_PATH);

    static {
        if (!BINARY_RELOCATION_ENABLED && !LEGACY_ROOTFS_PATH.equals(ROOTFS_PATH)) {
            Log.e(TAG, "Equal-length invariant broken: '" + LEGACY_ROOTFS_PATH + "' (" + NEEDLE.length + ") vs '" + ROOTFS_PATH + "' (" + REPLACEMENT.length + "). Binary relocation disabled.");
        }
    }

    private PathRelocator() {}

    /** Relocates a path string (symlink targets, text values). Length is not constrained here. */
    public static String relocate(String value) {
        if (value == null || LEGACY_ROOTFS_PATH.equals(ROOTFS_PATH)) return value;
        return value.contains(LEGACY_ROOTFS_PATH) ? value.replace(LEGACY_ROOTFS_PATH, ROOTFS_PATH) : value;
    }

    /**
     * Copies inStream to outStream replacing every occurrence of the legacy rootfs path with the
     * new one (same length). Matches that straddle read-buffer boundaries are handled by keeping a
     * carry of (needle length - 1) bytes between reads.
     *
     * @return number of replacements, or -1 on I/O error.
     */
    public static long copy(InputStream inStream, OutputStream outStream) {
        try {
            if (!BINARY_RELOCATION_ENABLED) {
                byte[] buffer = new byte[StreamUtils.BUFFER_SIZE];
                int amountRead;
                while ((amountRead = inStream.read(buffer)) != -1) outStream.write(buffer, 0, amountRead);
                outStream.flush();
                return 0;
            }

            final int keep = NEEDLE.length - 1;
            final byte[] buffer = new byte[StreamUtils.BUFFER_SIZE + keep];
            int filled = 0;
            long replacements = 0;
            int amountRead;

            while ((amountRead = inStream.read(buffer, filled, StreamUtils.BUFFER_SIZE)) != -1) {
                filled += amountRead;
                replacements += replaceAll(buffer, filled);

                if (filled > keep) {
                    int flushable = filled - keep;
                    outStream.write(buffer, 0, flushable);
                    System.arraycopy(buffer, flushable, buffer, 0, keep);
                    filled = keep;
                }
            }

            if (filled > 0) outStream.write(buffer, 0, filled);
            outStream.flush();
            return replacements;
        }
        catch (IOException e) {
            Log.e(TAG, "copy failed", e);
            return -1;
        }
    }

    /** Replaces in place every complete occurrence of NEEDLE inside data[0..length). */
    static int replaceAll(byte[] data, int length) {
        final byte first = NEEDLE[0];
        final int last = length - NEEDLE.length;
        int count = 0;
        for (int i = 0; i <= last; i++) {
            if (data[i] != first) continue;
            int j = 1;
            while (j < NEEDLE.length && data[i + j] == NEEDLE[j]) j++;
            if (j == NEEDLE.length) {
                System.arraycopy(REPLACEMENT, 0, data, i, REPLACEMENT.length);
                count++;
                i += NEEDLE.length - 1;
            }
        }
        return count;
    }
}
