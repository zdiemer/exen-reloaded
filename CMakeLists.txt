cmake_minimum_required(VERSION 3.20)
project(exen_reloaded VERSION 0.1.1 LANGUAGES C)

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
  src/core/extnatives.c
  src/core/fx.c
  src/core/graphics.c
  src/core/gamedb.c
  src/core/hash.c
  src/core/heap.c
  src/core/interp.c
  src/core/loader.c
  src/core/melody.c
  src/core/natives.c
  src/core/platform.c
  src/core/playfield.c
  src/core/raycast.c
  src/core/resource.c
  src/core/sms.c
  src/core/smsserver.c
  src/core/state.c
  src/core/ui.c
  src/core/vec3d.c
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
    add_executable(exen-reloaded WIN32 src/sdl/main.c src/sdl/input.c src/sdl/paths.c
      src/sdl/persist.c)
    target_link_libraries(exen-reloaded PRIVATE exen_core SDL3::SDL3)
    # Input bindings test: synthetic events, no window or gamepad needed.
    add_executable(sdl-input-test tests/sdl_input_test.c src/sdl/input.c)
    target_include_directories(sdl-input-test PRIVATE src/sdl)
    target_link_libraries(sdl-input-test PRIVATE exen_core SDL3::SDL3)
    add_test(NAME sdl_input COMMAND sdl-input-test)
  else()
    message(WARNING "SDL3 not found; the SDL frontend will not be built")
  endif()
endif()

# libretro core (docs/LIBRETRO.md): exen_reloaded_libretro.dll / .so / .dylib, exporting only
# the retro_* entry points, plus a headless test frontend that loads it like a frontend would.
option(EXEN_BUILD_LIBRETRO "Build the libretro core" OFF)
if(EXEN_BUILD_LIBRETRO)
  # A position-independent, hidden-visibility build of the core for the shared library; exen_core
  # itself stays as it is for the executables.
  get_target_property(EXEN_CORE_SOURCES exen_core SOURCES)
  add_library(exen_core_pic STATIC ${EXEN_CORE_SOURCES})
  target_include_directories(exen_core_pic PUBLIC src/core)
  set_target_properties(exen_core_pic PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
  add_library(exen_reloaded_libretro SHARED src/libretro/libretro.c)
  target_compile_definitions(exen_reloaded_libretro PRIVATE
    EXEN_LIBRETRO_VERSION="${PROJECT_VERSION}")
  target_link_libraries(exen_reloaded_libretro PRIVATE exen_core_pic)
  set_target_properties(exen_reloaded_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
  # the core info file goes beside the built core, ready to copy into a frontend
  configure_file(src/libretro/exen_reloaded_libretro.info exen_reloaded_libretro.info COPYONLY)
  if(WIN32)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_link_options(exen_reloaded_libretro PRIVATE -static-libgcc)
    endif()
  elseif(APPLE)
    target_link_options(exen_reloaded_libretro PRIVATE "LINKER:-exported_symbol,_retro_*")
  else()
    target_link_options(exen_reloaded_libretro PRIVATE
      "LINKER:--version-script=${PROJECT_SOURCE_DIR}/src/libretro/libretro.map"
      "LINKER:--no-undefined")
    set_property(TARGET exen_reloaded_libretro APPEND PROPERTY LINK_DEPENDS
      "${PROJECT_SOURCE_DIR}/src/libretro/libretro.map")
  endif()

  add_executable(exen-libretro-test src/libretro/test_frontend.c src/cli/png.c)
  target_include_directories(exen-libretro-test PRIVATE src/libretro src/cli)
  target_link_libraries(exen-libretro-test PRIVATE ${CMAKE_DL_LIBS})
  target_compile_definitions(exen-libretro-test PRIVATE
    EXEN_LIBRETRO_CORE_PATH="$<TARGET_FILE:exen_reloaded_libretro>"
    EXEN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  add_dependencies(exen-libretro-test exen_reloaded_libretro)
  # Asset-free check on the vendored download1.exn; the corpus run is documented in LIBRETRO.md.
  add_test(NAME libretro_core
    COMMAND exen-libretro-test --frames 300 "${PROJECT_SOURCE_DIR}/vendor/gensimu/flash/download1.exn")
  set_tests_properties(libretro_core PROPERTIES TIMEOUT 300 LABELS smoke)
endif()

# Tests: ctest --test-dir build --output-on-failure. enable_testing() is unconditional so that
# tests declared above (sdl_input) register too; EXEN_BUILD_TESTS adds the unit tests in tests/.
option(EXEN_BUILD_TESTS "Build the unit tests" OFF)
enable_testing()
if(EXEN_BUILD_TESTS)
  add_subdirectory(tests)
endif()
