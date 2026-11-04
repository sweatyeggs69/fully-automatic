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
  else()
    message(WARNING "Vulkan headers/loader not found: building without a game window. "
                    "Install them with: sudo pacman -S --needed vulkan-headers vulkan-icd-loader")
  endif()
endif()
