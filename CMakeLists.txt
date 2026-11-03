cmake_minimum_required(VERSION 3.10)
project(FullAutoNative CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Core library: disc image + XEX parsing (no graphics dependencies).
add_library(fa_core src/xdvdfs.cpp src/xex.cpp)
target_include_directories(fa_core PUBLIC include)

# Command line tool: list/extract ISO contents, inspect default.xex.
add_executable(faiso tools/faiso.cpp)
target_link_libraries(faiso fa_core)

# The Vulkan/GLFW bootloader prototype is parked in prototype/ and is not built
# yet. It will be revived once the recompilation pipeline produces something to run.

enable_testing()
add_test(NAME iso_roundtrip
         COMMAND ${CMAKE_COMMAND} -DFAISO=$<TARGET_FILE:faiso> -DWORKDIR=${CMAKE_BINARY_DIR}
                 -P ${CMAKE_SOURCE_DIR}/tests/iso_roundtrip.cmake)
