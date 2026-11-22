#!/bin/bash
# LURVETA Performance — host tests for the native changes (Linux x86-64).
# Requires: gcc, libvulkan.so.1 loader and a Vulkan ICD (e.g. Mesa lavapipe):
#   VK_ICD_FILENAMES=/path/to/lvp_icd.json ./tools/host-tests/run.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
C="$ROOT/app/src/main/cpp"; T="$(mktemp -d)"; J="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}/include"
FLAGS=(-std=gnu11 -g -O1 -fsanitize=address,undefined -DAPP_ID='"com.lurveta.co"' -DVK_USE_PLATFORM_ANDROID_KHR -DVT_SERVER=1 -DVK_NO_PROTOTYPES=1
       -I "$ROOT/tools/host-tests/stub" -I "$J" -I "$J/linux" -I "$C/vortekrenderer/include" -I "$C/winlator/include")
echo "== BC decoder unit tests"
gcc -std=c11 -O1 -g -fsanitize=address,undefined -I "$C/vortekrenderer/include" -o "$T/bc" "$ROOT/tools/host-tests/bc_decoder_test.c"
"$T/bc"
echo "== Texture decoder integration tests (Vulkan)"
mkdir -p /data/data/com.lurveta.co/cache && rm -rf /data/data/com.lurveta.co/cache/vortek
gcc "${FLAGS[@]}" -c "$C/vortekrenderer/src/texture_decoder.c" -o "$T/td.o"
gcc -std=gnu11 -g -O1 -fsanitize=address,undefined -I "$C/winlator/include" -c "$C/winlator/src/arrays.c" -o "$T/arrays.o"
gcc "${FLAGS[@]}" "$ROOT/tools/host-tests/texture_decoder_test.c" "$T/td.o" "$T/arrays.o" -o "$T/td" -ldl -lpthread
ASAN_OPTIONS=detect_leaks=0 "$T/td"

echo "== Vortek host tests (inline uploads, BC1-BC7 reference, pipeline cache)"
"$ROOT/tools/vortek-host-tests/run_tests.sh" "$T/vortek"
"$ROOT/tools/vortek-vtio/run_tests.sh" "$T/vtio"

echo "== Native crash handler (aarch64 under qemu)"
mkdir -p /data/data/com.lurveta.co/files/logs
aarch64-linux-gnu-gcc -O1 -g -Wall -rdynamic -funwind-tables -I "$C/vortekrenderer/include" -o "$T/crash_test" \
    "$ROOT/tools/host-tests/crash_handler_test.c" "$C/vortekrenderer/src/crash_handler.c" -ldl
QEMU_LD_PREFIX=/usr/aarch64-linux-gnu qemu-aarch64 "$T/crash_test" 2>/dev/null
CRASH=/data/data/com.lurveta.co/files/logs/native-crash.txt
grep -q "SIGSEGV" "$CRASH" && grep -q "fault address 0x10" "$CRASH" && grep -q "client pid 1234" "$CRASH" \
    && grep -q "lurvetaTestCrashHere" "$CRASH" && echo "ALL CRASH HANDLER TESTS PASSED" || { echo "FAIL: crash report incomplete"; cat "$CRASH"; exit 1; }

echo "== Benchmark samplers (ProcessSampler on the real /proc, GpuSampler fallback)"
JAVAC="${JAVA_HOME:+$JAVA_HOME/bin/}javac"; JAVARUN="${JAVA_HOME:+$JAVA_HOME/bin/}java"
mkdir -p "$T/samplers"
"$JAVAC" -d "$T/samplers" "$ROOT"/tools/host-tests/stub-java/android/os/Process.java "$ROOT"/tools/host-tests/stub-java/android/system/*.java \
    "$ROOT/app/src/main/java/com/winlator/core/ProcessSampler.java" "$ROOT/app/src/main/java/com/winlator/core/GpuSampler.java" "$ROOT/tools/host-tests/SamplersTest.java"
"$JAVARUN" -cp "$T/samplers" SamplersTest

echo "== Benchmark analysis metrics (synthetic logs with known answers)"
python3 -I "$ROOT/tools/host-tests/analyze_test.py"

echo "== Launch analysis (verdict for each point where a launch can stop)"
mkdir -p "$T/la"
"$JAVAC" -d "$T/la" "$ROOT/app/src/main/java/com/winlator/core/LaunchAnalysis.java" "$ROOT/tools/host-tests/LaunchAnalysisTest.java"
"$JAVARUN" -cp "$T/la" LaunchAnalysisTest

echo "== Launch trace end to end (simulated GTA IV black screen on the real /proc)"
mkdir -p "$T/lt/classes" "$T/lt/files"
"$JAVAC" -nowarn -d "$T/lt/classes" "$ROOT"/tools/host-tests/stub-java/android/content/*.java "$ROOT"/tools/host-tests/stub-java/android/os/*.java \
    "$ROOT"/tools/host-tests/stub-java/android/util/*.java "$ROOT"/tools/host-tests/stub-java/android/system/*.java "$ROOT"/tools/host-tests/stub-java/androidx/annotation/*.java \
    "$ROOT/tools/host-tests/stub-trace/com/winlator/BuildConfig.java" "$ROOT/tools/host-tests/stub-trace/com/winlator/core/HardwareInfo.java" \
    "$ROOT/app/src/main/java/com/winlator/core/LaunchTrace.java" "$ROOT/app/src/main/java/com/winlator/core/LaunchAnalysis.java" \
    "$ROOT/app/src/main/java/com/winlator/core/EnvVars.java" "$ROOT/tools/host-tests/LaunchTraceTest.java"
"$JAVARUN" -Dtest.filesDir="$T/lt/files" -cp "$T/lt/classes" LaunchTraceTest | grep -E "FAIL|PASSED|probe"
