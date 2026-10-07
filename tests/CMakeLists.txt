# Unit tests: one binary, a minimal self-registering harness
# (testing.h), each test registered with CTest by name so a failure is
# reported per test. Engine-dependent tests (real models) live behind
# VALTZ_TEST_* env vars and skip when unset -- check the "skipped"
# count, a skip is not a pass.
add_executable(valtz_test
  main.cc
  base-test.cc
  cache-test.cc
  lmdb-test.cc
  media-test.cc
  project-test.cc
  workspace-test.cc
  models-test.cc
  assist-test.cc
  ext-test.cc
  controller-test.cc
  model-input-test.cc
  gpu-planar-test.mm
  i18n-test.cc)
if(VALTZ_HAVE_VPIPE)
  # The valtz-vpipe plugin in a real vpipe session (no models needed).
  target_sources(valtz_test PRIVATE vpipe-plugin-test.cc
    graph-builder-test.cc)
  target_link_libraries(valtz_test PRIVATE valtz_vpipe_contract)
endif()
target_link_libraries(valtz_test PRIVATE valtz::core
  "-framework CoreGraphics" "-framework ImageIO" "-framework Metal"
  "-framework Foundation" "-framework AudioToolbox")
# The i18n test reads the app's String Catalog from the source tree.
target_compile_definitions(valtz_test PRIVATE
  VALTZ_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(valtz_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/core/src")
valtz_target_warnings(valtz_test)

add_test(NAME valtz_test COMMAND valtz_test)
