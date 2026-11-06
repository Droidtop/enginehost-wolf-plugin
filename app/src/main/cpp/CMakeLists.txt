cmake_minimum_required(VERSION 3.22)
project(enginehost_wolf C)

# The Wolf core (WolfPlugin.java, wolf_enginehost.c): one library, libwolfrpg.so, with the core
# linked in statically, no C++ runtime and no SDL. With a display it draws with GLES 2 and plays
# through AAudio (Enginehost's runtime process, allocate-then-clamp); isolated, it draws in software
# into the host's frames and fills the host's audio ring (Droidtop/tracker#858, 6691411927). Images
# are decoded with stb_image, from the vendor/stb submodule (nothings/stb, pinned by its gitlink).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The engine: core/ from the line's upstream commit (the engine branch, `creation`).
set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../core)
if(NOT EXISTS ${ENGINE_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "No engine at ${ENGINE_DIR}; build a plugin/<line> branch, which carries the engine.")
endif()
add_subdirectory(${ENGINE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/core)

# Enginehost's plugin-native, checked out by the plugin-api action at its pinned commit.
set(PLUGIN_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../build/enginehost/plugin-native)
if(NOT EXISTS ${PLUGIN_NATIVE_DIR}/enginehost_vfs.cmake)
    message(FATAL_ERROR "No plugin-native at ${PLUGIN_NATIVE_DIR}; the workflow's plugin-api action puts it there.")
endif()

set(STB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb)
if(NOT EXISTS ${STB_DIR}/stb_image.h)
    message(FATAL_ERROR "No stb at ${STB_DIR}; run git submodule update --init.")
endif()

add_library(wolfrpg SHARED wolf_enginehost.c wolf_open.c wolf_files.c wolf_frame.c wolf_decode.c wolf_gl.c wolf_fonts.c
            wolf_aaudio.c
            ${PLUGIN_NATIVE_DIR}/enginehost_audio_ring.c)
target_include_directories(wolfrpg PRIVATE ${PLUGIN_NATIVE_DIR} ${STB_DIR})
target_compile_options(wolfrpg PRIVATE -Wall -Wextra)
target_link_libraries(wolfrpg PRIVATE wolfcore GLESv2 aaudio android log m)

# Every file the core opens goes through Enginehost's file layer.
include(${PLUGIN_NATIVE_DIR}/enginehost_vfs.cmake)
enginehost_vfs(wolfrpg)
