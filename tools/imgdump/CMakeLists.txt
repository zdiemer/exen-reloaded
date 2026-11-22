# Standalone build of the imgdump tool (see imgdump.c):
#   cmake -S tools/imgdump -B build/imgdump -G Ninja && cmake --build build/imgdump
cmake_minimum_required(VERSION 3.20)
project(imgdump C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()

set(EXEN_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core)
add_executable(imgdump imgdump.c ${EXEN_CORE}/codec.c)
target_include_directories(imgdump PRIVATE ${EXEN_CORE})
