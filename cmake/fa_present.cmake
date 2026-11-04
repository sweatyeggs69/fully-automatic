# Window + Vulkan presenter (src/gpu/vulkan). Optional: without Vulkan the game runs headless.
# CachyOS / Arch: sudo pacman -S --needed vulkan-headers vulkan-icd-loader sdl2
# plus the driver: vulkan-radeon (AMD), nvidia-utils (NVIDIA) or vulkan-intel (Intel).
get_filename_component(FA_ROOT ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)

if(NOT TARGET fa_present AND NOT FA_NO_WINDOW)
  find_package(Vulkan QUIET)
  find_package(PkgConfig REQUIRED)
  if(NOT SDL2_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
  endif()
  if(Vulkan_FOUND AND SDL2_FOUND)
    add_library(fa_present STATIC ${FA_ROOT}/src/gpu/vulkan/presenter.cpp)
    target_include_directories(fa_present PUBLIC ${FA_ROOT}/include)
    target_link_libraries(fa_present PUBLIC Vulkan::Vulkan PkgConfig::SDL2)
    message(STATUS "Vulkan presenter: on (${Vulkan_LIBRARY})")
    # The renderer (real Xenos draws) also needs shaderc to compile translated shaders at runtime.
    # CachyOS / Arch: sudo pacman -S --needed shaderc
    # It is an OBJECT library: its objects go straight into the executable, and the shader
    # translator / primitive / texture code it uses comes from fa_gpu or the runtime sources.
    if(NOT TARGET fa_render AND NOT FA_NO_RENDERER)
      pkg_check_modules(SHADERC IMPORTED_TARGET shaderc)
      if(SHADERC_FOUND)
        add_library(fa_render OBJECT ${FA_ROOT}/src/gpu/vulkan/renderer.cpp)
        target_include_directories(fa_render PUBLIC ${FA_ROOT}/include)
        target_link_libraries(fa_render PUBLIC Vulkan::Vulkan PkgConfig::SHADERC)
        target_compile_definitions(fa_render INTERFACE FA_WITH_RENDERER)
        message(STATUS "Vulkan renderer: on (shaderc ${SHADERC_VERSION})")
      else()
        message(WARNING "shaderc not found: the window shows the front buffer but nothing is rendered. "
                        "Install it with: sudo pacman -S --needed shaderc")
      endif()
    endif()
  else()
    message(WARNING "Vulkan headers/loader not found: building without a game window. "
                    "Install them with: sudo pacman -S --needed vulkan-headers vulkan-icd-loader")
  endif()
endif()
