# CMake Build Configuration for root of project
cmake_minimum_required(VERSION 3.18)
project(distributed_acoustic_localization VERSION 0.1
        DESCRIPTION "Distributed acoustic localization on BeagleY-AI"
        LANGUAGES C)

# Compiler options (inherited by sub-folders)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Werror -Wpedantic -Wextra)
add_compile_definitions(_POSIX_C_SOURCE=200809L)
add_compile_options(-fdiagnostics-color)

# Address sanitizer is off by default (it conflicts with mlockall() on the node)
# Enable with: -DENABLE_ASAN=ON
option(ENABLE_ASAN "Build with address sanitizer" OFF)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()

# Enable PThread library for linking
add_compile_options(-pthread)
add_link_options(-pthread)

# Where executables are copied after building
set(DEPLOY_DIR "$ENV{HOME}/ensc351/public/myApps")

# Which folders to build
add_subdirectory(node)
add_subdirectory(master)
