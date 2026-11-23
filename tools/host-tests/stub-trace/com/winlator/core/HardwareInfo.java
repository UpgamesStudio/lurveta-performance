package com.winlator.core;
/** Host stub for the launch-trace test. */
public final class HardwareInfo {
    public static final class Gpu { public String deviceName = "host-gpu", driverName = "-", driverInfo = "-"; public String getApiVersionString() { return "-"; } }
    public static String getSocName() { return "host"; }
    public static String getCpuSummary() { return "host"; }
    public static Gpu getGpu(android.content.Context c) { return new Gpu(); }
}
