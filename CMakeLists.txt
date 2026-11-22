cmake_minimum_required(VERSION 3.20)
project(exen_reloaded C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Portable core: no OS headers, no SDL.
add_library(exen_core STATIC
  src/core/api.c
  src/core/hash.c
  src/core/heap.c
  src/core/interp.c
  src/core/loader.c
  src/core/natives.c
  src/core/platform.c
)
target_include_directories(exen_core PUBLIC src/core)

# Headless runner, used by the compatibility harness.
add_executable(exen-run src/cli/exen_run.c)
target_link_libraries(exen-run PRIVATE exen_core)
