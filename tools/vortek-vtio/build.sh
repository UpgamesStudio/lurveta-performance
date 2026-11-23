#!/bin/bash
# Rebuilds app/src/main/assets/graphics_driver/vortek-2.1.tzst with the EINTR-safe client:
#   1. builds liblurveta_vtio.so (aarch64 glibc),
#   2. renames the client's imports __poll_chk/recvmsg/read -> lurveta_*, adds the DT_NEEDED,
#   3. repacks the archive (same layout, plus usr/lib/liblurveta_vtio.so).
# The original client is taken from git (the 1dc6101 Winlator 11.2 import) so the patch is reproducible.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out="${1:-$here/out}"
asset="$repo/app/src/main/assets/graphics_driver/vortek-2.1.tzst"
rm -rf "$out"; mkdir -p "$out/orig" "$out/root"

git -C "$repo" show 1dc6101:app/src/main/assets/graphics_driver/vortek-2.1.tzst > "$out/orig.tzst"
tar --zstd -xf "$out/orig.tzst" -C "$out/root"
tar --zstd -tvf "$out/orig.tzst" > "$out/orig-listing.txt"
cp "$out/root/usr/lib/libvulkan_vortek.so" "$out/orig/"

aarch64-linux-gnu-gcc -O2 -fPIC -shared -U_FORTIFY_SOURCE -Wall -Wextra -Werror \
    -Wl,-soname,liblurveta_vtio.so -o "$out/root/usr/lib/liblurveta_vtio.so" "$here/vtio.c"
aarch64-linux-gnu-strip --strip-unneeded "$out/root/usr/lib/liblurveta_vtio.so"

client="$out/root/usr/lib/libvulkan_vortek.so"
printf '__poll_chk lurveta_poll_chk\nrecvmsg lurveta_recvmsg\nread lurveta_read\n' > "$out/renames.txt"
patchelf --rename-dynamic-symbols "$out/renames.txt" "$client"
patchelf --add-needed liblurveta_vtio.so "$client"
patchelf --set-rpath '$ORIGIN' "$client"

# Same owner/permissions/order as the original archive.
(cd "$out/root" && tar --owner=0 --group=0 --numeric-owner --sort=name --mtime='2026-01-01' \
    -cf - . | zstd -19 -q -o "$out/vortek-2.1.tzst")
if [ "${INSTALL:-0}" = 1 ]; then cp "$out/vortek-2.1.tzst" "$asset"; echo "installed $asset"; fi
echo "built $out/vortek-2.1.tzst"
