# ValtzBridge -- the Swift-importable facade over valtz_core.
# Swift sees only include/valtz-bridge.h (through module.modulemap).

add_library(valtz_bridge STATIC src/valtz-bridge.cc)
target_include_directories(valtz_bridge PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(valtz_bridge PUBLIC valtz::core
  "-framework CoreGraphics")
valtz_target_warnings(valtz_bridge)

# Swift targets that link the bridge compile with C++ interop on, parsing
# the header as C++20 like the rest of the layer.
target_compile_options(valtz_bridge INTERFACE
  "$<$<COMPILE_LANGUAGE:Swift>:-cxx-interoperability-mode=default>"
  "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-Xcc -std=c++20>")
