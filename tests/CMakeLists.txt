# Unit tests (CTest). Asset-free: besides synthetic data built by the tests themselves they only
# read files that are part of the source tree (vendor/gensimu/gensimu.exe, the two vendored
# vendor/gensimu/flash/*.exn, data/gamedb.tsv).
#
#   cmake -S . -B build -DEXEN_BUILD_TESTS=ON && cmake --build build
#   ctest --test-dir build --output-on-failure
#   python -m unittest discover -s tests/python      # the Python tool tests (not in CTest)

add_library(exen_test_support STATIC c/support.c)
target_link_libraries(exen_test_support PUBLIC exen_core)
target_include_directories(exen_test_support PUBLIC c)
target_compile_definitions(exen_test_support PUBLIC
  EXEN_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  EXEN_GENSIMU_PATH="${PROJECT_SOURCE_DIR}/vendor/gensimu/gensimu.exe")

set(EXEN_C_TESTS hash loader interp heap codec melody gamedb boot sms playfield)
foreach(name IN LISTS EXEN_C_TESTS)
  add_executable(test_${name} c/test_${name}.c)
  target_link_libraries(test_${name} PRIVATE exen_test_support)
  add_test(NAME ${name} COMMAND test_${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 120 LABELS unit)
endforeach()

# The headless runner on a vendored .exn: boots, ticks, and must not halt (exit code 0).
add_test(NAME exen_run_smoke
  COMMAND exen-run --frames 120 --quiet
          --sim "${PROJECT_SOURCE_DIR}/vendor/gensimu/gensimu.exe"
          "${PROJECT_SOURCE_DIR}/vendor/gensimu/flash/download1.exn")
set_tests_properties(exen_run_smoke PROPERTIES
  TIMEOUT 120 LABELS smoke PASS_REGULAR_EXPRESSION "status: running")

# test_melody measures pitch and levels with <math.h>
if(UNIX)
  target_link_libraries(test_melody PRIVATE m)
endif()
