cmake_minimum_required(VERSION 3.20)
project(fa_runtime C CXX)

# Runtime for the recompiled game: guest memory, kernel layer, threads, imports.
#
#   cmake -S runtime -B runtime/out -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build runtime/out --target fullauto     (needs the recompiled ppc/ directory)
#   ctest --test-dir runtime/out                     (unit test, uses a fake ppc/ directory)
#
# Expects XenonRecomp (with submodules) at third_party/XenonRecomp, as for tools/xexscan.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(BUILD_SHARED_LIBS OFF)

set(FA_XENONRECOMP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/XenonRecomp CACHE PATH "XenonRecomp checkout")
set(FA_PPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ppc CACHE PATH "XenonRecomp output directory")
set(THIRDPARTY_ROOT ${FA_XENONRECOMP}/thirdparty)

# fa_core (ISO/XEX) and fa_hle (file system, input, audio, XMA): needs SDL2 + FFmpeg.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/fa_hle.cmake)

file(GLOB FA_RT_SOURCES CONFIGURE_DEPENDS
    src/memory.cpp src/kernel.cpp src/crash.cpp src/timebase.cpp src/profiler.cpp src/kernel/*.cpp
    src/gpu/*.cpp src/audio/*.cpp src/input/*.cpp src/vfs/*.cpp)
# The GPU command processor lives in the top-level src/gpu.
list(APPEND FA_RT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu/command_processor.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu/texture.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu/frame_backend.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu/shader_translator.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/gpu/primitive.cpp)
# The game window (SDL2 + Vulkan). Optional: FA_NO_WINDOW=ON or no Vulkan -> headless build.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/fa_present.cmake)

function(fa_rt_library name ppc_include)
    add_library(${name} STATIC ${FA_RT_SOURCES} src/loader.cpp)
    target_include_directories(${name} PUBLIC include ${ppc_include}
        ${THIRDPARTY_ROOT}/simde ${FA_XENONRECOMP}/XenonUtils ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_definitions(${name} PUBLIC
        FA_XENON_CONTEXT_H="${FA_XENONRECOMP}/XenonUtils/ppc_context.h")
    target_compile_options(${name} PRIVATE -Wall -Wno-unused-parameter)
    find_package(Threads REQUIRED)
    target_link_libraries(${name} PUBLIC Threads::Threads fa_hle)
endfunction()

# ---- unit test against a fake ppc/ directory ------------------------------------------------
fa_rt_library(fa_rt_fake ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_ppc)
add_executable(rt_test tests/rt_test.cpp)
target_link_libraries(rt_test fa_rt_fake)
enable_testing()
add_test(NAME rt_test COMMAND rt_test)
add_executable(hle_import_test tests/hle_import_test.cpp)
target_link_libraries(hle_import_test fa_rt_fake)
add_test(NAME hle_import_test COMMAND hle_import_test)
add_executable(nt_file_test tests/nt_file_test.cpp)
target_link_libraries(nt_file_test fa_rt_fake)
add_test(NAME nt_file_test COMMAND nt_file_test)
add_executable(timebase_test tests/timebase_test.cpp)
target_link_libraries(timebase_test fa_rt_fake)
add_test(NAME timebase_test COMMAND timebase_test)

# ---- the game, once XenonRecomp output exists --------------------------------------------------
if(EXISTS ${FA_PPC_DIR}/ppc_config.h)
    set(THIRDPARTY_ROOT ${FA_XENONRECOMP}/thirdparty)
    add_subdirectory(${THIRDPARTY_ROOT} thirdparty)
    add_subdirectory(${FA_XENONRECOMP}/XenonUtils XenonUtils)

    fa_rt_library(fa_rt ${FA_PPC_DIR})
    target_compile_definitions(fa_rt PUBLIC FA_WITH_XENONUTILS)
    target_link_libraries(fa_rt PUBLIC XenonUtils)

    file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${FA_PPC_DIR}/ppc_recomp.*.cpp ${FA_PPC_DIR}/ppc_func_mapping.cpp)
    set(STUBS ${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp)
    add_custom_command(OUTPUT ${STUBS}
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_import_stubs.py
                ${FA_PPC_DIR}/ppc_func_mapping.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${FA_XENONRECOMP}/XenonUtils/xbox/xboxkrnl_table.inc ${FA_XENONRECOMP}/XenonUtils/xbox/xam_table.inc
                > ${STUBS}
        DEPENDS ${FA_PPC_DIR}/ppc_func_mapping.cpp ${FA_RT_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_import_stubs.py
        VERBATIM)

    add_executable(fullauto src/main.cpp ${STUBS} ${PPC_SOURCES})
    target_link_libraries(fullauto PRIVATE fa_rt)
    target_include_directories(fullauto PRIVATE ${FA_PPC_DIR})
    target_link_options(fullauto PRIVATE -rdynamic)
    if(TARGET fa_present)
        target_link_libraries(fullauto PRIVATE fa_present)
        target_compile_definitions(fullauto PRIVATE FA_WITH_PRESENTER)
    endif()
    if(TARGET fa_render)
        target_link_libraries(fullauto PRIVATE fa_render)
    endif()
    # Optimisation for the recompiled game code. -O2 with AVX2 (x86-64-v3, Haswell and newer) lets
    # simde map VMX to native SIMD; the movie decoder was CPU bound at -O1 without it. A full
    # rebuild of ppc/ takes well under a minute on 16 threads. -ffp-contract=off keeps separate
    # PowerPC multiplies and adds from being fused (FMA rounds differently). Older CPUs: -DFA_PPC_OPT="-O2;-march=x86-64-v2"
    set(FA_PPC_OPT "-O2;-march=x86-64-v3" CACHE STRING "Compiler flags for the recompiled ppc/ sources")
    set_source_files_properties(${PPC_SOURCES} PROPERTIES COMPILE_OPTIONS "${FA_PPC_OPT};-fno-strict-aliasing;-ffp-contract=off")
else()
    message(STATUS "No ${FA_PPC_DIR}/ppc_config.h: skipping the fullauto target (run XenonRecomp first)")
endif()

# Links the Vd/GPU glue into an executable so a missing GPU source fails here, not on the PC.
add_executable(vd_link_test tests/vd_link_test.cpp)
target_link_libraries(vd_link_test fa_rt_fake)
add_test(NAME vd_link_test COMMAND vd_link_test)
