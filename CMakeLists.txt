cmake_minimum_required(VERSION 3.20)
project(exen_reloaded VERSION 0.2.0 LANGUAGES C)

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
  src/core/gm.c
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
# TinySoundFont (vendor/tinysoundfont/PROVENANCE.md), included by src/core/gm.c only
target_include_directories(exen_core PRIVATE vendor/tinysoundfont)
# gm.c (TinySoundFont) uses <math.h>
if(UNIX)
  target_link_libraries(exen_core PUBLIC m)
endif()

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
    # microui (vendor/microui, MIT): the in-window menu strip and panels (src/sdl/ui.c)
    add_library(microui STATIC vendor/microui/microui.c)
    target_include_directories(microui PUBLIC vendor/microui)
    add_executable(exen-reloaded WIN32 src/sdl/main.c src/sdl/input.c src/sdl/menu.c
      src/sdl/paths.c src/sdl/persist.c src/sdl/settings.c src/sdl/ui.c src/cli/png.c)
    target_include_directories(exen-reloaded PRIVATE src/sdl src/cli)
    target_compile_definitions(exen-reloaded PRIVATE EXEN_VERSION="${PROJECT_VERSION}")
    target_link_libraries(exen-reloaded PRIVATE exen_core microui SDL3::SDL3)
    # The native menu bar: Win32 on Windows, AppKit on macOS; elsewhere the microui strip.
    if(WIN32)
      target_sources(exen-reloaded PRIVATE src/sdl/menu_win32.c)
    elseif(APPLE)
      enable_language(OBJC)
      target_sources(exen-reloaded PRIVATE src/sdl/menu_cocoa.m)
      target_link_libraries(exen-reloaded PRIVATE "-framework Cocoa")
    endif()
    # Input bindings test: synthetic events, no window or gamepad needed.
    add_executable(sdl-input-test tests/sdl_input_test.c src/sdl/input.c src/sdl/paths.c)
    target_include_directories(sdl-input-test PRIVATE src/sdl)
    target_link_libraries(sdl-input-test PRIVATE exen_core SDL3::SDL3)
    add_test(NAME sdl_input COMMAND sdl-input-test)
    # Menu model and settings.ini tests: no window either.
    add_executable(sdl-menu-test tests/sdl_menu_test.c src/sdl/menu.c)
    target_include_directories(sdl-menu-test PRIVATE src/sdl)
    target_link_libraries(sdl-menu-test PRIVATE SDL3::SDL3)
    add_test(NAME sdl_menu COMMAND sdl-menu-test)
    add_executable(sdl-settings-test tests/sdl_settings_test.c src/sdl/settings.c src/sdl/paths.c)
    target_include_directories(sdl-settings-test PRIVATE src/sdl src/core)
    target_link_libraries(sdl-settings-test PRIVATE SDL3::SDL3)
    add_test(NAME sdl_settings COMMAND sdl-settings-test)
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
  target_include_directories(exen_core_pic PRIVATE vendor/tinysoundfont)
  if(UNIX)
    target_link_libraries(exen_core_pic PUBLIC m)
  endif()
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
  # Again with the General MIDI voice and the vendored SoundFont in a system directory of its
  # own, so that the SoundFont voice's state round trip and determinism go through libretro too
  # (the save point falls in the middle of download1.exn's melody).
  set(EXEN_LR_SYSTEM "${CMAKE_BINARY_DIR}/libretro-system")
  configure_file(vendor/gensimu/gensimu.exe "${EXEN_LR_SYSTEM}/gensimu.exe" COPYONLY)
  configure_file(vendor/gensimu/flash/eeprom.dat "${EXEN_LR_SYSTEM}/flash/eeprom.dat" COPYONLY)
  configure_file(vendor/soundfont/TimGM6mb.sf2 "${EXEN_LR_SYSTEM}/TimGM6mb.sf2" COPYONLY)
  add_test(NAME libretro_core_gm
    COMMAND exen-libretro-test --system "${EXEN_LR_SYSTEM}" --option exen_melody_voice=piano
            --expect-audio --frames 300 "${PROJECT_SOURCE_DIR}/vendor/gensimu/flash/download1.exn")
  set_tests_properties(libretro_core_gm PROPERTIES TIMEOUT 300 LABELS smoke)
endif()

# Tests: ctest --test-dir build --output-on-failure. enable_testing() is unconditional so that
# tests declared above (sdl_input) register too; EXEN_BUILD_TESTS adds the unit tests in tests/.
option(EXEN_BUILD_TESTS "Build the unit tests" OFF)
enable_testing()
if(EXEN_BUILD_TESTS)
  add_subdirectory(tests)
endif()
