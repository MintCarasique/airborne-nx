#!/bin/bash
# Builds the Switch NRO of rex/ inside WSL with the workspace's devkitPro
# (tools/toolchain/nx-sdk, Linux binaries from fetch_switch_sdk.py) and the
# static NVK from build_nvk.py (generated/nvk/libvulkan.a).
#
#   wsl --exec bash tools/build_switch.sh [jobs]
#
# Run tools\build_rexglue.ps1 first: it applies the SDK patch and generates
# rex/generated/default (codegen is a host tool, there is none for the Switch).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${1:-8}
export DEVKITPRO=$ROOT/tools/toolchain/nx-sdk/opt/devkitpro
SDK=$ROOT/tools/reference-nfsmw/sdk
BUILD=$ROOT/rex/out/switch

# The SDK expects the Mesa Horizon SDK layout: <dir>/lib/libvulkan.a.
NVK=$ROOT/generated/nvk-sdk
mkdir -p "$NVK/lib"
cp -u "$ROOT/generated/nvk/libvulkan.a" "$NVK/lib/libvulkan.a"

test -f "$ROOT/rex/generated/default/sources.cmake" || {
  echo "no generated code: run tools\\build_rexglue.ps1 first" >&2; exit 1; }

cmake -S "$ROOT/rex" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/reference-nfsmw/tools/switch/cmake/switch-devkitA64.cmake" \
  -DDEVKITPRO="$DEVKITPRO" -DREXSDK_DIR="$SDK" -DREXGLUE_SWITCH_NVK_SDK="$NVK"
cmake --build "$BUILD" -j "$JOBS"
find "$BUILD" -maxdepth 2 -name '*.nro' -printf '%p %s bytes\n'
