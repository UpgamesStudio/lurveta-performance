package com.winlator.core;

/** LURVETA: process limits inherited by Wine/Box64 (see process_limits.c). */
public final class ProcessLimits {
    /** Below this many open files esync is not safe: big games run out of descriptors and hang. */
    public static final long MIN_FILES_FOR_ESYNC = 16384;

    static {
        System.loadLibrary("winlator");
    }

    private ProcessLimits() {}

    /** Raises the soft open-file limit to the hard limit. Returns {soft before, soft after, hard}. */
    public static native long[] raiseOpenFileLimit();
}
