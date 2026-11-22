package com.winlator.core;
public class HwTest {
  static int fails = 0;
  static void expect(String[] lines, HardwareInfo.GpuFamily f) { HardwareInfo.Gpu g = HardwareInfo.parseGpu(lines); if (g.family != f) { System.out.println("FAIL " + String.join(",", lines) + " -> " + g.family + " (expected " + f + ")"); fails++; } }
  public static void main(String[] a) {
    expect(new String[]{"vendorID=5045","deviceName=Mali-G615 MC6","driverID=9","textureCompressionBC=0"}, HardwareInfo.GpuFamily.ARM_MALI);   // 0x13B5 = 5045
    expect(new String[]{"vendorID=5045","deviceName=Immortalis-G925 MC12","driverID=9"}, HardwareInfo.GpuFamily.ARM_IMMORTALIS);
    expect(new String[]{"vendorID=0","deviceName=Mali-G57","driverID=9"}, HardwareInfo.GpuFamily.ARM_MALI);          // driverID alone
    expect(new String[]{"vendorID=20803","deviceName=Adreno (TM) 750","driverID=8"}, HardwareInfo.GpuFamily.QUALCOMM_ADRENO); // 0x5143
    expect(new String[]{"vendorID=4112","deviceName=PowerVR B-Series BXM-8-256","driverID=7"}, HardwareInfo.GpuFamily.IMAGINATION_POWERVR); // 0x1010
    expect(new String[]{"vendorID=5197","deviceName=Samsung Xclipse 940","driverID=21"}, HardwareInfo.GpuFamily.SAMSUNG_XCLIPSE);
    expect(new String[]{"vendorID=4318","deviceName=Some GPU","driverID=0"}, HardwareInfo.GpuFamily.OTHER);
    expect(new String[0], HardwareInfo.GpuFamily.UNKNOWN);
    HardwareInfo.Gpu g = HardwareInfo.parseGpu(new String[]{"vendorID=5045","deviceName=Mali-G615 MC6","driverID=9","textureCompressionBC=0","textureCompressionASTC_LDR=1"});
    if (!g.isMali() || g.textureCompressionBC || !g.textureCompressionASTC) { System.out.println("FAIL flags"); fails++; }
    if (!"Cortex-A715".equals(HardwareInfo.armCoreName("0xd4d")) || !"Cortex-A510".equals(HardwareInfo.armCoreName("0xd46"))) { System.out.println("FAIL core names"); fails++; }
    System.out.println(fails == 0 ? "ALL HARDWAREINFO TESTS PASSED" : fails + " FAILURES");
    System.exit(fails == 0 ? 0 : 1);
  }
}
