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
