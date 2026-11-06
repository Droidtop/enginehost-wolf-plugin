cmake_minimum_required(VERSION 3.22)
project(enginehost_wolf C)

# The Wolf core in Enginehost's sandbox (WolfPlugin.java, wolf_enginehost.c): one library,
# libwolfrpg.so, with the core linked in statically, no C++ runtime and no SDL. The isolated
# runtime loads it from a descriptor. It draws in software (no GL in an isolated process:
# Enginehost docs/engine-sandbox.md); images are decoded with stb_image, from the vendor/stb
# submodule (nothings/stb, pinned by its gitlink).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The engine: core/ from the line's upstream commit (the engine branch, `creation`).
set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core)
if(NOT EXISTS ${ENGINE_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "No engine at ${ENGINE_DIR}; build a plugin/<line> branch, which carries the engine.")
endif()
add_subdirectory(${ENGINE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/core)

set(PLUGIN_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../plugin-native)

set(STB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb)
if(NOT EXISTS ${STB_DIR}/stb_image.h)
    message(FATAL_ERROR "No stb at ${STB_DIR}; run git submodule update --init.")
endif()

add_library(wolfrpg SHARED wolf_enginehost.c wolf_open.c wolf_files.c wolf_frame.c)
target_include_directories(wolfrpg PRIVATE ${PLUGIN_NATIVE_DIR} ${STB_DIR})
target_compile_options(wolfrpg PRIVATE -Wall -Wextra)
target_link_libraries(wolfrpg PRIVATE wolfcore android log m)

# Every file the core opens goes through Enginehost's file layer.
include(${PLUGIN_NATIVE_DIR}/enginehost_vfs.cmake)
enginehost_vfs(wolfrpg)
