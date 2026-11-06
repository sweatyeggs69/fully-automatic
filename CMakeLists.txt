cmake_minimum_required(VERSION 3.16)
project(FullAutoNative CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Core library: disc image + XEX parsing (no graphics dependencies).
add_library(fa_core STATIC src/xdvdfs.cpp src/xex.cpp)
target_include_directories(fa_core PUBLIC include)

# Xenos GPU layer: PM4 command processor + backend interface (no graphics deps).
add_library(fa_gpu src/gpu/command_processor.cpp src/gpu/texture.cpp src/gpu/frame_backend.cpp
            src/gpu/shader_translator.cpp src/gpu/primitive.cpp src/mem/write_watch.cpp)
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

# Kernel write tracking (userfaultfd + PAGEMAP_SCAN); skips itself on kernels without it.
add_executable(write_watch_test tests/write_watch_test.cpp)
target_link_libraries(write_watch_test fa_gpu)
add_test(NAME write_watch COMMAND write_watch_test)

# Shader translator: every ALU op, control flow and fetch format must compile with shaderc.
if(TARGET PkgConfig::SHADERC)
  add_executable(gpu_shader_test tests/gpu_shader_test.cpp)
  target_include_directories(gpu_shader_test PRIVATE tests)
  target_link_libraries(gpu_shader_test fa_gpu PkgConfig::SHADERC)
  add_test(NAME gpu_shader COMMAND gpu_shader_test)
endif()
# Renderer end to end (PM4 -> draws -> resolve into memory). Skips itself without a Vulkan device.
if(TARGET fa_render)
  add_executable(gpu_render_test tests/gpu_render_test.cpp)
  target_include_directories(gpu_render_test PRIVATE tests)
  target_link_libraries(gpu_render_test fa_render fa_gpu)
  add_test(NAME gpu_render COMMAND gpu_render_test)
  add_test(NAME gpu_render_frames COMMAND gpu_render_test)
  set_tests_properties(gpu_render_frames PROPERTIES ENVIRONMENT "FA_FRAMES=2")
endif()

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
