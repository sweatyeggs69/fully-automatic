# Shared by the top-level project and runtime/: defines fa_core (ISO + XEX
# parsing) and fa_hle (file system, controller input, audio output, XMA
# decoding). Needs SDL2 and FFmpeg's libavcodec/libavutil (pkg-config).
get_filename_component(FA_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)

if(NOT TARGET fa_core)
  add_library(fa_core STATIC ${FA_ROOT}/src/xdvdfs.cpp ${FA_ROOT}/src/xex.cpp)
  target_include_directories(fa_core PUBLIC ${FA_ROOT}/include)
endif()

if(NOT TARGET fa_hle)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
  find_package(Threads REQUIRED)
  add_library(fa_hle STATIC
    ${FA_ROOT}/src/hle/vfs.cpp ${FA_ROOT}/src/hle/input.cpp ${FA_ROOT}/src/hle/audio_system.cpp
    ${FA_ROOT}/src/hle/xma_decoder.cpp ${FA_ROOT}/src/hle/xwb.cpp ${FA_ROOT}/src/hle/services.cpp)
  target_include_directories(fa_hle PUBLIC ${FA_ROOT}/include)
  target_link_libraries(fa_hle PUBLIC fa_core PkgConfig::SDL2 PkgConfig::FFMPEG Threads::Threads)
endif()
