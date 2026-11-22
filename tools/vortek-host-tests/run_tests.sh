#!/bin/sh
# Builds and runs the Vortek host tests (no phone required). Needs: cc, python3 + Pillow,
# Mesa lavapipe and the Khronos validation layer (Debian/Ubuntu: mesa-vulkan-drivers vulkan-validationlayers).
#   ./run_tests.sh [build_dir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/build}
"$HERE/build.sh" "$OUT"

echo; echo "### BC decoder vs. independent reference (Pillow)"
python3 -I "$HERE/test_bc_reference.py" "$OUT/bc_decode_cli" "$OUT/bc-ref"

echo; echo "### Texture decoder on lavapipe + validation + synchronization validation"
ICD=/usr/share/vulkan/icd.d/lvp_icd.json
[ -f "$ICD" ] && export VK_ICD_FILENAMES="$ICD"
"$OUT/test_texture_decoder_vk"

echo; echo "### Persistent pipeline cache on lavapipe + validation"
"$OUT/test_pipeline_cache_vk" "$OUT/spec.comp.spv"
