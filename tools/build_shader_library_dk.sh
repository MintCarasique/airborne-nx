#!/bin/bash
# Builds the deko3d shader library (rex/out/native_shaders_dk/airborne_shaders_dk.nfsp) from all MoH:A containers,
# as tools/reference-nfsmw-dk/shaders/nfsmw_regenerar_biblioteca_pcf.sh does with DEKO3D=1: the same translator
# (tools/shaders_native, with its NFSMW_DEKO3D mode), DXC with -DNFSMW_DEKO3D and without -fvk-invert-y (deko3d's clip
# space y points up, as Direct3D's), then spirv-val and the packer of the Vulkan library.
#
#   bash tools/build_shader_library_dk.sh [jobs]
#
# Also checks that the translator change left the Vulkan library alone: VULKAN_CHECK (default 300) shaders compiled
# without the define must give the same SPIR-V as rex/out/native_shaders_v4/spirv.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${1:-16}
OUT=$ROOT/rex/out/native_shaders_dk
REF=$ROOT/tools/reference-nfsmw
NATIVE=$ROOT/tools/shaders_native
CLANG="/c/Program Files/LLVM/bin/clang++.exe"
DXC=$ROOT/tools/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe
VAL=$ROOT/rex/out/native_shaders_all/spirv-tools-build/tools/spirv-val.exe
V4=$ROOT/rex/out/native_shaders_v4
cd "$ROOT"

mkdir -p "$OUT/containers"
cp -u rex/out/native_shaders_all/containers/*.bin rex/out/unknown_containers/*.bin "$OUT/containers/"

# --- 0. translator and translation ---
"$CLANG" -std=c++23 -O1 -I"$NATIVE" -I"$NATIVE/XenosRecomp" -I"$REF/sdk/thirdparty/fmt/include" \
  -I"$ROOT/tools/mesa-switch/src/util" -include "$NATIVE/pch_min.h" -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DFMT_HEADER_ONLY \
  -DXXH_INLINE_ALL -DNFSMW_RECOMP "$NATIVE/nfsmw_hlsl.cpp" "$NATIVE/XenosRecomp/shader_recompiler.cpp" \
  -o "$OUT/nfsmw_hlsl.exe" > "$OUT/translator_build.log" 2>&1 || { echo "translator build failed (translator_build.log)"; exit 1; }
rm -rf "$OUT/hlsl"; mkdir -p "$OUT/hlsl"
"$OUT/nfsmw_hlsl.exe" "$OUT/containers" "$OUT/hlsl" tools/shaders_native/XenosRecomp/shader_common.h > "$OUT/translate.log" 2>&1
echo "translated: $(ls "$OUT/hlsl" | wc -l) of $(ls "$OUT/containers" | wc -l)"

# --- 1. DXC, deko3d mode ---
cat > "$OUT/dxc_one.sh" <<'EOS'
#!/bin/bash
# dxc_one.sh <dxc> <outdir> <name> <mode: dk|vk>
DXC=$1; OUT=$2; n=$3; mode=$4
b=$(od -An -tx1 -j3 -N1 "$OUT/containers/$n.bin" | tr -d ' ')
if [ "$b" = "01" ]; then t=vs_6_6; else t=ps_6_6; fi
if [ "$mode" = dk ]; then x=(-DNFSMW_DEKO3D); dst=spirv; logs=dxc_logs
else dst=spirv_vk; logs=dxc_logs_vk; x=(); [ "$b" = "01" ] && x=(-fvk-invert-y); fi
if ! "$DXC" -spirv -T $t -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Werror=parameter-usage "${x[@]}" \
    -Fo "$OUT/$dst/$n.spv" "$OUT/hlsl/$n.hlsl" > "$OUT/$logs/$n.log" 2>&1; then rm -f "$OUT/$dst/$n.spv"
else rm -f "$OUT/$logs/$n.log"; fi
EOS
rm -rf "$OUT/spirv" "$OUT/dxc_logs"; mkdir -p "$OUT/spirv" "$OUT/dxc_logs"
ls "$OUT/hlsl" | sed 's/\.hlsl$//' | xargs -P "$JOBS" -I{} bash "$OUT/dxc_one.sh" "$DXC" "$OUT" {} dk
: > "$OUT/dxc_failures.log"
for l in "$OUT"/dxc_logs/*.log; do
  [ -e "$l" ] || continue
  { echo "== $(basename "$l" .log)"; grep -m3 -iE 'error' "$l" || head -3 "$l"; } >> "$OUT/dxc_failures.log"
done
echo "dxc (deko3d): ok $(ls "$OUT/spirv" | wc -l), failed $(ls "$OUT/dxc_logs" | wc -l)"

# --- 1b. the Vulkan library must not change ---
rm -rf "$OUT/spirv_vk" "$OUT/dxc_logs_vk"; mkdir -p "$OUT/spirv_vk" "$OUT/dxc_logs_vk"
ls "$V4/spirv" | sed 's/\.spv$//' | awk 'NR % 33 == 0' | head -n "${VULKAN_CHECK:-300}" > "$OUT/vk_sample.txt"
xargs -P "$JOBS" -I{} bash "$OUT/dxc_one.sh" "$DXC" "$OUT" {} vk < "$OUT/vk_sample.txt"
same=0; diff=0
while read -r n; do
  if cmp -s "$OUT/spirv_vk/$n.spv" "$V4/spirv/$n.spv"; then same=$((same + 1)); else diff=$((diff + 1)); echo "$n" >> "$OUT/vk_differs.txt"; fi
done < "$OUT/vk_sample.txt"
echo "vulkan mode vs native_shaders_v4: $same identical, $diff different (vk_differs.txt)"

# --- 2. spirv-val and pack ---
rm -rf "$OUT/stage" "$OUT/val_logs"; mkdir -p "$OUT/stage" "$OUT/val_logs"
cp "$ROOT/rex/out/native_shaders_all/val_one.sh" "$OUT/val_one.sh"
ls "$OUT/spirv" | sed 's/\.spv$//' | xargs -P "$JOBS" -I{} bash "$OUT/val_one.sh" "$VAL" "$OUT" "$OUT/containers" {}
echo "spirv-val: ok $(ls "$OUT/stage" | wc -l), failed $(ls "$OUT/val_logs" | wc -l)"
"$CLANG" -std=c++23 -O2 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DXXH_INLINE_ALL -I"$ROOT/rex/src/native" \
  -I"$REF/sdk/thirdparty/xxHash" "$V4/pack.cpp" "$ROOT/rex/src/native/shader_library.cpp" -o "$OUT/pack.exe" \
  > "$OUT/pack_build.log" 2>&1 || { echo "pack build failed (pack_build.log)"; exit 1; }
"$OUT/pack.exe" "$OUT/stage" "$OUT/spirv" "$OUT/airborne_shaders_dk.nfsp" && ls -l "$OUT/airborne_shaders_dk.nfsp"
