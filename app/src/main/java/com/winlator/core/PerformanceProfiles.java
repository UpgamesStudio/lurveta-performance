package com.winlator.core;

import android.content.Context;

import com.winlator.box64.Box64Preset;

/**
 * LURVETA Performance — per-game profiles.
 *
 * A profile is only a starting point made of EXISTING settings (Box64 preset, internal resolution,
 * Vortek texture-cache size and performance hints). Selecting one fills the fields of the game's
 * shortcut settings; the user can still change anything before saving, and nothing is applied
 * automatically. "Automatic" only suggests a profile from the detected GPU and RAM.
 * The DXVK frame-rate limit is deliberately not touched (a cap that does not divide the display
 * refresh rate causes frame pacing problems).
 */
public final class PerformanceProfiles {
    public enum Id {CUSTOM, PERFORMANCE, BALANCED, COMPATIBILITY, STABILITY}

    public static final class Profile {
        public final Id id;
        public final String box64Preset;
        public final String screenSize;
        public final int vortekImageCacheSizeMB;
        public final boolean performanceHints;

        Profile(Id id, String box64Preset, String screenSize, int vortekImageCacheSizeMB, boolean performanceHints) {
            this.id = id;
            this.box64Preset = box64Preset;
            this.screenSize = screenSize;
            this.vortekImageCacheSizeMB = vortekImageCacheSizeMB;
            this.performanceHints = performanceHints;
        }
    }

    private PerformanceProfiles() {}

    public static Profile get(Id id) {
        switch (id) {
            case PERFORMANCE:   return new Profile(id, Box64Preset.PERFORMANCE,   "960x544",  512, true);
            case BALANCED:      return new Profile(id, Box64Preset.INTERMEDIATE,  "1280x720", 256, true);
            case COMPATIBILITY: return new Profile(id, Box64Preset.CONSERVATIVE,  "1280x720", 256, true);
            case STABILITY:     return new Profile(id, Box64Preset.STABILITY,     "1280x720", 256, false);
            default:            return null;
        }
    }

    /** Suggestion only (the user decides). */
    public static Id suggest(Context context) {
        HardwareInfo.Gpu gpu = HardwareInfo.getGpu(context);
        long totalMem = HardwareInfo.getTotalMemory(context);
        if (gpu.isMali()) return totalMem >= (7L << 30) ? Id.BALANCED : Id.PERFORMANCE;
        return Id.COMPATIBILITY;
    }

    public static Id parse(String value) {
        if (value != null) {
            for (Id id : Id.values()) if (id.name().equalsIgnoreCase(value)) return id;
        }
        return Id.CUSTOM;
    }
}
