cmake_minimum_required(VERSION 3.20)
project(xexscan C CXX)

# Standalone helper that reuses XenonRecomp's XEX loader (decrypt + decompress).
# Expects XenonRecomp cloned (with submodules) at third_party/XenonRecomp.
set(XR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/XenonRecomp)
set(THIRDPARTY_ROOT ${XR}/thirdparty)
set(CMAKE_CXX_STANDARD 20)
set(BUILD_SHARED_LIBS OFF)

add_subdirectory(${THIRDPARTY_ROOT} thirdparty)
add_subdirectory(${XR}/XenonUtils XenonUtils)

add_executable(xexscan main.cpp)
target_link_libraries(xexscan PRIVATE XenonUtils)
