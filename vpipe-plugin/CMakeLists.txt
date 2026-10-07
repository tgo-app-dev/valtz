# valtz-vpipe -- the vpipe plugin Valtz exchanges data through.
#
# A vpipe plugin like any other: built with the installed SDK's
# vpipe_add_plugin (a dlopen-only MODULE linked to libvpipe, no RPATH:
# it is only ever loaded into a process that already has libvpipe), and
# loaded by the Valtz engine through the session config's `plugins`
# list. The app bundle carries it in Contents/PlugIns (app/macos).
#
# valtz_vpipe_contract is the vocabulary both sides compile
# (include/valtz-vpipe/exchange.h): the plugin and valtz_core's engine.

add_library(valtz_vpipe_contract INTERFACE)
target_include_directories(valtz_vpipe_contract INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

vpipe_add_plugin(valtz-vpipe SOURCES
  src/plugin.cc
  src/beat-buffer.cc
  src/sink-stage.cc
  src/source-stage.cc
  src/tap-stage.cc)
target_include_directories(valtz-vpipe PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(valtz-vpipe PRIVATE valtz_vpipe_contract
  "-framework CoreFoundation")
target_compile_definitions(valtz-vpipe PRIVATE
  VALTZ_VERSION="${PROJECT_VERSION}")
valtz_target_warnings(valtz-vpipe)
