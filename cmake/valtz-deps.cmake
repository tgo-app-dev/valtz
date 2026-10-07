# Third-party dependencies.
#
#   lmdb       extern/lmdb (git submodule, pinned to the same release
#              vpipe builds), compiled as an OBJECT library so its
#              symbols bind inside valtz_core and never to the copy
#              libvpipe also exports.
#   nlohmann   3rd-party/nlohmann/json.hpp (vendored single header):
#              record encoding (CBOR on disk) and the JSON the bridge
#              and pipeline specs speak.
#   vpipe      find_package(vpipe) against an INSTALLED prefix. vpipe's
#              CMake assumes it is the top-level project, so it cannot
#              be add_subdirectory()'d; the installed package is also the
#              API boundary Valtz is meant to respect.

set(_lmdb_src "${PROJECT_SOURCE_DIR}/extern/lmdb/libraries/liblmdb")
if(NOT EXISTS "${_lmdb_src}/mdb.c")
  message(FATAL_ERROR
    "extern/lmdb is empty. Run: git submodule update --init --recursive")
endif()

find_package(Threads REQUIRED)

add_library(valtz_lmdb OBJECT "${_lmdb_src}/mdb.c" "${_lmdb_src}/midl.c")
target_include_directories(valtz_lmdb SYSTEM PUBLIC "${_lmdb_src}")
target_link_libraries(valtz_lmdb PUBLIC Threads::Threads)
set_target_properties(valtz_lmdb PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON)
# LMDB's own sources are not ours to keep warning-clean.
target_compile_options(valtz_lmdb PRIVATE -w)

add_library(valtz_json INTERFACE)
target_include_directories(valtz_json SYSTEM INTERFACE
  "${PROJECT_SOURCE_DIR}/3rd-party")
# Keep std::string implicit conversions off: they make overloads ambiguous
# and hide copies.
target_compile_definitions(valtz_json INTERFACE
  $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:JSON_USE_IMPLICIT_CONVERSIONS=0>)

set(VALTZ_HAVE_VPIPE OFF)
if(VALTZ_WITH_VPIPE)
  find_package(vpipe CONFIG
    HINTS "${VALTZ_VPIPE_PREFIX}"
    PATH_SUFFIXES lib/cmake/vpipe)
  if(NOT vpipe_FOUND)
    message(FATAL_ERROR
      "libvpipe not found under VALTZ_VPIPE_PREFIX=${VALTZ_VPIPE_PREFIX}.\n"
      "Install a vpipe build there (cmake --install <vpipe-build> "
      "--prefix <prefix>), point VALTZ_VPIPE_PREFIX at it, or configure "
      "with -DVALTZ_WITH_VPIPE=OFF for a build without the engine.")
  endif()
  set(VALTZ_HAVE_VPIPE ON)
  get_target_property(_vpipe_loc vpipe::vpipe IMPORTED_LOCATION_RELEASE)
  message(STATUS "valtz: libvpipe        ${_vpipe_loc}")
endif()
