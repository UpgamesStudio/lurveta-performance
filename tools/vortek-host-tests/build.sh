#!/bin/sh
# Builds the host tests against the real Vortek sources. Output goes to $1 (default ./build).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../app/src/main/cpp"
OUT=${1:-$HERE/build}
mkdir -p "$OUT"
CFLAGS="-O1 -g -std=gnu11 -Wall -Wno-unused-function -Wno-unused-variable -Wno-incompatible-pointer-types-discards-qualifiers -Wno-discarded-qualifiers -Wno-unused-but-set-variable -Wno-switch -Wno-format-truncation"
INC="-I$HERE/stubs -I$SRC/winlator/include -I$SRC/vortekrenderer/include"
DEFS="-DVT_SERVER=1 -DVK_NO_PROTOTYPES=1 -DVK_USE_PLATFORM_ANDROID_KHR=1 -DAPP_ID=\"com.lurveta.co\""
SAN=${SAN:--fsanitize=address,undefined}

cc $CFLAGS $SAN $INC -DAPP_ID=\"com.lurveta.co\" "$HERE/bc_decode_cli.c" -o "$OUT/bc_decode_cli" -lpthread
cc $CFLAGS $SAN $INC $DEFS "$HERE/test_texture_decoder_vk.c" "$SRC/vortekrenderer/src/texture_decoder.c" "$SRC/winlator/src/arrays.c" \
   -o "$OUT/test_texture_decoder_vk" -ldl -lpthread -lm
glslangValidator -V -S comp "$HERE/spec.comp" -o "$OUT/spec.comp.spv" >/dev/null
mkdir -p "$OUT/cache"
cc $CFLAGS $SAN $INC $DEFS -DPIPELINE_CACHE_DIR=\"$OUT/cache\" "$HERE/test_pipeline_cache_vk.c" "$SRC/vortekrenderer/src/pipeline_cache.c" "$SRC/winlator/src/arrays.c" \
   -o "$OUT/test_pipeline_cache_vk" -ldl -lpthread -lm
cc $CFLAGS $SAN $INC $DEFS "$HERE/test_fence_export.c" "$SRC/vortekrenderer/src/fence_export.c" "$SRC/winlator/src/arrays.c" \
   -o "$OUT/test_fence_export" -lpthread
echo "built: $OUT/bc_decode_cli $OUT/test_texture_decoder_vk"
