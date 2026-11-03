cmake_minimum_required(VERSION 3.10)
project(FullAutoNative)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
# Note: These assume standard paths for Vulkan and GLFW
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

# Include directories
include_directories(include)

# Source files
set(SOURCES src/main.cpp)

# Executable
add_executable(FullAutoNative ${SOURCES})

# Linking libraries
target_link_libraries(FullAutoNative Vulkan::Vulkan glfw)
