cmake_minimum_required(VERSION 3.10)
project(FullAutoNative CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Core library: disc image + XEX parsing (no graphics dependencies).
add_library(fa_core STATIC src/xdvdfs.cpp src/xex.cpp)
target_include_directories(fa_core PUBLIC include)

# Xenos GPU layer: PM4 command processor + backend interface (no graphics deps).
add_library(fa_gpu src/gpu/command_processor.cpp src/gpu/texture.cpp src/gpu/frame_backend.cpp)
target_include_directories(fa_gpu PUBLIC include)

# Command line tool: list/extract ISO contents, inspect default.xex.
add_executable(faiso tools/faiso.cpp)
target_link_libraries(faiso fa_core)

# The game window (SDL2 + Vulkan presenter). fawindow shows a test pattern through it.
include(cmake/fa_present.cmake)
if(TARGET fa_present)
  add_executable(fawindow tools/fawindow.cpp)
  target_link_libraries(fawindow fa_present)
endif()

enable_testing()
add_test(NAME iso_roundtrip
         COMMAND ${CMAKE_COMMAND} -DFAISO=$<TARGET_FILE:faiso> -DWORKDIR=${CMAKE_BINARY_DIR}
                 -P ${CMAKE_SOURCE_DIR}/tests/iso_roundtrip.cmake)

add_executable(gpu_pm4_test tests/gpu_pm4_test.cpp)
target_link_libraries(gpu_pm4_test fa_gpu)
add_test(NAME gpu_pm4 COMMAND gpu_pm4_test)

add_executable(gpu_texture_test tests/gpu_texture_test.cpp)
target_link_libraries(gpu_texture_test fa_gpu)
add_test(NAME gpu_texture COMMAND gpu_texture_test)

# HLE services used by the runtime: file system (ISO / folders), controller
# input (SDL2), audio output (SDL2) and XMA decoding (system FFmpeg).
include(cmake/fa_hle.cmake)

# Wave bank tool: list/decode .xwb audio straight from the ISO.
add_executable(fawave tools/fawave.cpp)
target_link_libraries(fawave fa_hle)

add_executable(hle_tests tests/hle_tests.cpp)
target_link_libraries(hle_tests fa_hle)
add_test(NAME hle_tests
         COMMAND ${CMAKE_COMMAND} -DTESTS=$<TARGET_FILE:hle_tests> -DWORKDIR=${CMAKE_BINARY_DIR}
                 -P ${CMAKE_SOURCE_DIR}/tests/hle_tests.cmake)
