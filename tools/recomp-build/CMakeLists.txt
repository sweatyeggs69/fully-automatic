cmake_minimum_required(VERSION 3.20)
project(fa_recomp CXX)

# Compiles the XenonRecomp output (../../ppc/*.cpp) into a static library.
# This only checks that the generated code builds; it is not runnable without a runtime.
# Needs clang. Large: use few parallel jobs (e.g. cmake --build out -j4).
set(PPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../ppc)
set(SIMDE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/XenonRecomp/thirdparty/simde)

file(GLOB PPC_SOURCES ${PPC_DIR}/ppc_recomp.*.cpp ${PPC_DIR}/ppc_func_mapping.cpp)
list(LENGTH PPC_SOURCES PPC_COUNT)
if(PPC_COUNT EQUAL 0)
  message(FATAL_ERROR "No generated sources in ${PPC_DIR}. Run XenonRecomp first.")
endif()

add_library(fa_recomp STATIC ${PPC_SOURCES})
set_target_properties(fa_recomp PROPERTIES CXX_STANDARD 20)
target_include_directories(fa_recomp PUBLIC ${PPC_DIR} ${SIMDE})
target_compile_options(fa_recomp PRIVATE -O1 -msse4.1 -mssse3 -fno-strict-aliasing
                       -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses-equality)
