# Valtz -- image / video generation and editing for macOS.
#
#   SwiftUI app  (app/macos)        Swift, talks to the core through
#        |                          bridge/ via Swift/C++ interop
#   ValtzBridge  (bridge)           the narrow, Swift-importable facade
#        |
#   valtz_core   (core)             C++20 model-controller layer: projects
#        |                          (LMDB), asset graph, media, models,
#        |                          jobs, peers
#   libvpipe     (installed pkg)    execution engine (models, kernels)
#
# Configure out of source (see README.md):
#
#   cmake --preset dev        # Ninja, ~/dump/valtz-build, vpipe from
#                             # ~/dump/vpipe-install
#   cmake --build --preset dev

cmake_minimum_required(VERSION 3.29)

# The oldest macOS Valtz runs on -- at or above libvpipe's own floor (an
# app cannot run below the dylib it links; vpipe's is 26.0, its NAX
# kernels 26.2).
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "26.2" CACHE STRING
      "Minimum macOS version" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

project(valtz VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

option(VALTZ_BUILD_APP   "Build the SwiftUI macOS app" ON)
option(VALTZ_BUILD_TESTS "Build the unit tests" ON)
option(VALTZ_WITH_VPIPE  "Link libvpipe as the execution engine" ON)
option(VALTZ_WERROR      "Treat warnings in Valtz sources as errors" ON)
set(VALTZ_VPIPE_PREFIX "$ENV{HOME}/dump/vpipe-install" CACHE PATH
    "Install prefix of a vpipe build (holds lib/cmake/vpipe)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
  message(FATAL_ERROR "Valtz targets Apple Silicon macOS only.")
endif()

if(VALTZ_BUILD_APP)
  if(NOT CMAKE_GENERATOR MATCHES "Ninja|Xcode")
    message(FATAL_ERROR
      "The SwiftUI app needs the Ninja or Xcode generator (CMake builds "
      "Swift with no other). Use `cmake --preset dev`, or pass "
      "-DVALTZ_BUILD_APP=OFF to build only the C++ layer.")
  endif()
  # swiftc targets the BUILD MACHINE's macOS unless it is told otherwise:
  # CMake hands the deployment target to clang, not to swiftc. Built on
  # macOS 27 the app said "macOS 27.0 or later" (its minos) and did not
  # open on 26.6, while libvpipe, the plugin and valtzctl said 26.0. The
  # triple carries it to every Swift compile and link.
  if(NOT CMAKE_Swift_COMPILER_TARGET)
    set(CMAKE_Swift_COMPILER_TARGET
        "arm64-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  enable_language(Swift)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(valtz-deps)
include(valtz-warnings)

if(VALTZ_HAVE_VPIPE)
  add_subdirectory(vpipe-plugin)
endif()
add_subdirectory(core)
add_subdirectory(tools/valtzctl)
if(VALTZ_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(VALTZ_BUILD_APP)
  add_subdirectory(bridge)
  add_subdirectory(app/macos)
endif()

# valtz-dist: a RELOCATABLE Valtz.app in <build>/dist -- libvpipe, the
# plugin, valtzctl (and Sparkle, when built in) inside the bundle, every
# reference rewritten, signed ad hoc (app/macos/bundle-app.sh). What a
# release signs with a Developer ID instead.
if(VALTZ_BUILD_APP AND VALTZ_HAVE_VPIPE)
  get_filename_component(_vpipe_prefix "${vpipe_DIR}/../../.." ABSOLUTE)
  set(_dist_sparkle)
  if(VALTZ_SPARKLE)
    set(_dist_sparkle
        --sparkle-framework "${VALTZ_SPARKLE_DIR}/Sparkle.framework")
  endif()
  add_custom_target(valtz-dist
    COMMAND bash "${PROJECT_SOURCE_DIR}/app/macos/bundle-app.sh"
            --app-in "$<TARGET_BUNDLE_DIR:Valtz>"
            --app "${CMAKE_BINARY_DIR}/dist/Valtz.app"
            --libvpipe "$<TARGET_FILE:vpipe::vpipe>"
            --valtzctl "$<TARGET_FILE:valtzctl>"
            --vpipe-doc "${_vpipe_prefix}/share/doc/vpipe"
            --source-dir "${PROJECT_SOURCE_DIR}"
            --entitlements
              "${PROJECT_SOURCE_DIR}/app/macos/Valtz.entitlements"
            ${_dist_sparkle}
    DEPENDS Valtz valtzctl
    COMMENT "Assembling a relocatable Valtz.app (ad hoc) in dist/"
    VERBATIM)
endif()

message(STATUS "valtz: build type      ${CMAKE_BUILD_TYPE}")
message(STATUS "valtz: deployment      macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}")
message(STATUS "valtz: vpipe engine    ${VALTZ_HAVE_VPIPE}")
if(VALTZ_HAVE_VPIPE)
  message(STATUS "valtz: vpipe plugin    valtz-vpipe.so")
endif()
message(STATUS "valtz: SwiftUI app     ${VALTZ_BUILD_APP}")
