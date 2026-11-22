#!/bin/bash
# Builds the shim + patched client (build.sh) and runs test_vtio under qemu-aarch64.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$here/out}"
"$here/build.sh" "$out" >/dev/null
lib="$out/root/usr/lib"
aarch64-linux-gnu-gcc -O2 -fPIC -shared -o "$out/libtestvtio.so" "$here/test_lib.c"
nm -D --undefined-only "$out/libtestvtio.so" | grep -q __poll_chk || { echo "test lib does not import __poll_chk"; exit 1; }
patchelf --rename-dynamic-symbols "$out/renames.txt" "$out/libtestvtio.so"
patchelf --add-needed liblurveta_vtio.so "$out/libtestvtio.so"
aarch64-linux-gnu-gcc -O2 -o "$out/test_vtio" "$here/test_vtio.c" -ldl -lpthread
QEMU_LD_PREFIX=/usr/aarch64-linux-gnu LD_LIBRARY_PATH="$lib" qemu-aarch64 "$out/test_vtio" "$lib/libvulkan_vortek.so" "$out/libtestvtio.so" 2>&1 | tee "$out/main.txt" | grep -v "count [0-9]*, retried"
grep -q "^VTIO TEST PASSED" "$out/main.txt" || exit 1
grep -q "ANOMALY recvmsg (fd reply): reply arrived without any fd" "$out/main.txt" && echo "ok: fd reply without fds reported as ANOMALY" || { echo "FAIL: anomaly not reported"; exit 1; }
echo "--- A/B: LURVETA_VTIO_RETRY=0 must reproduce the failure"
if QEMU_LD_PREFIX=/usr/aarch64-linux-gnu LD_LIBRARY_PATH="$lib" LURVETA_VTIO_RETRY=0 qemu-aarch64 "$out/test_vtio" "$lib/libvulkan_vortek.so" "$out/libtestvtio.so" > "$out/ab.txt" 2>&1; then
  echo "FAIL: retries off but the test passed (signals not reaching the wait?)"; cat "$out/ab.txt"; exit 1
else grep -c "^FAIL" "$out/ab.txt" | sed 's/^/ok: retries off -> failures reproduced: /'; fi
