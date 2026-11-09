# valtz-procedural -- a PROGRAM drawn (DESIGN §4t): JavaScript run in a
# sandboxed WKWebView, its frames taken and written. A process of its
# own, started by the core for each render (media/procedural.h); the app
# carries it in Contents/Helpers, Valtz's copies of three.js and
# Babylon.js in Contents/Resources/procedural.
add_executable(valtz-procedural main.mm)
target_link_libraries(valtz-procedural PRIVATE valtz_json
  "-framework Foundation"
  "-framework AppKit"
  "-framework WebKit"
  "-framework CoreImage"
  "-framework CoreVideo"
  "-framework CoreMedia"
  "-framework AVFoundation"
  "-framework VideoToolbox"
  "-framework ImageIO"
  "-framework UniformTypeIdentifiers")
set_source_files_properties(main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
valtz_target_warnings(valtz-procedural)
