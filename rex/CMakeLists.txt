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
    src/renderdoc_trigger.cpp
)

if(WIN32)
    add_executable(airborne WIN32 ${AIRBORNE_SOURCES})
else()
    add_executable(airborne ${AIRBORNE_SOURCES})
endif()

rexglue_setup_target(airborne GPU_PLUGINS xenos)

# With the SDK as a subdirectory, rex_app.cpp (compiled into this target)
# includes imgui.h, but rexruntime links imgui privately. Same workaround as
# tools/reference-nfsmw/app/CMakeLists.txt.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(airborne PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# renderdoc_trigger.cpp includes renderdoc_app.h through rex/ui/renderdoc_api.h.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(airborne PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
