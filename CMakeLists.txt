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
  src/core/codec.c
  src/core/graphics.c
  src/core/hash.c
  src/core/heap.c
  src/core/interp.c
  src/core/loader.c
  src/core/natives.c
  src/core/platform.c
  src/core/playfield.c
  src/core/resource.c
)
target_include_directories(exen_core PUBLIC src/core)

# Headless runner, used by the compatibility harness.
add_executable(exen-run src/cli/exen_run.c src/cli/png.c)
target_link_libraries(exen-run PRIVATE exen_core)

# SDL3 frontend. Uses an installed SDL3 if available, otherwise fetches and builds it.
option(EXEN_BUILD_SDL "Build the SDL3 frontend" ON)
option(EXEN_FETCH_SDL "Download SDL3 if it is not installed" ON)
if(EXEN_BUILD_SDL)
  find_package(SDL3 CONFIG QUIET)
  if(NOT SDL3_FOUND AND EXEN_FETCH_SDL)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG release-3.4.10
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
  endif()
  if(TARGET SDL3::SDL3)
    add_executable(exen-reloaded WIN32 src/sdl/main.c)
    target_link_libraries(exen-reloaded PRIVATE exen_core SDL3::SDL3)
  else()
    message(WARNING "SDL3 not found; the SDL frontend will not be built")
  endif()
endif()
