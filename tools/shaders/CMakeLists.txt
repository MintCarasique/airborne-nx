cmake_minimum_required(VERSION 3.20)
project(AirborneShaderTranslator LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XENOS "${ROOT}/tools/XenosRecomp")
add_executable(airborne-shader-translate translate.cpp
    "${XENOS}/XenosRecomp/shader_recompiler.cpp"
    "${XENOS}/thirdparty/fmt/src/format.cc")
target_include_directories(airborne-shader-translate PRIVATE
    "${XENOS}/XenosRecomp" "${XENOS}/thirdparty/fmt/include"
    "${XENOS}/thirdparty/dxc-bin/inc" "${XENOS}/thirdparty/smol-v/source"
    "${XENOS}/thirdparty/xxHash" "${XENOS}/thirdparty/zstd/lib")
target_compile_definitions(airborne-shader-translate PRIVATE NOMINMAX AIRBORNE_RECOMP)
# Keep assertions enabled: upstream has incomplete instruction paths.
target_compile_options(airborne-shader-translate PRIVATE -O1 -UNDEBUG
    "SHELL:-include stdlib.h" "SHELL:-include ${XENOS}/XenosRecomp/pch.h"
    -Wno-switch -Wno-null-arithmetic -fms-extensions)
