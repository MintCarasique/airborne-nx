# airborne - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(airborne LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(AIRBORNE_SOURCES
    src/main.cpp
    src/input_script.cpp
    src/nxlink_log.cpp
    src/occlusion_queries.cpp
    src/gpu_wait.cpp
    src/audio_mixer.cpp
    src/renderdoc_trigger.cpp
)

# Native renderer, ported from the NFS: Most Wanted port (GPL-3.0); selected
# at run time with nfsmw_renderizador=nativo, the Xenos emulation otherwise.
file(GLOB AIRBORNE_NATIVE_SOURCES CONFIGURE_DEPENDS src/native/*.cpp)
list(APPEND AIRBORNE_SOURCES ${AIRBORNE_NATIVE_SOURCES})

if(WIN32)
    add_executable(airborne WIN32 ${AIRBORNE_SOURCES})
else()
    add_executable(airborne ${AIRBORNE_SOURCES})
endif()

rexglue_setup_target(airborne GPU_PLUGINS xenos)

# The native renderer uses SDK internals (src/core/threading_switch.h,
# graphics registers) and xxHash, which the runtime links privately.
target_include_directories(airborne PRIVATE "${REXSDK_DIR}/src" "${REXSDK_DIR}/src/ui/shaders" src/native)
if(TARGET xxHash::xxhash)
    target_link_libraries(airborne PRIVATE xxHash::xxhash)
endif()

# With the SDK as a subdirectory, rex_app.cpp (compiled into this target)
# includes imgui.h, but rexruntime links imgui privately. Same workaround as
# tools/reference-nfsmw/app/CMakeLists.txt.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(airborne PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Nintendo Switch: airborne.nro next to the ELF (tools/package_switch.py
# installs it as dist/switch/airborne-nx/airborne-nx.nro).
if(REXGLUE_PLATFORM_SWITCH)
    rexglue_switch_add_nro(airborne NAME "AirborneNX" AUTHOR "AirborneNX" VERSION "0.1.0")
endif()

# airborne_register.cpp is one function with ~55 000 SetFunction calls that
# runs once at startup; GCC at -O3 spends ~20 minutes on it. -O1 is enough.
if(REXGLUE_PLATFORM_SWITCH)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/airborne_register.cpp"
        APPEND PROPERTY COMPILE_OPTIONS -O1)
endif()

# renderdoc_trigger.cpp includes renderdoc_app.h through rex/ui/renderdoc_api.h.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(airborne PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
