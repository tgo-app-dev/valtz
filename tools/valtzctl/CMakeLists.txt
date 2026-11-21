# valtzctl -- headless driver for the controller layer. Every feature the
# app has is reachable here first, so the core is testable (and
# scriptable, and runnable over ssh on a peer Mac) without the UI.
add_executable(valtzctl main.cc)
target_link_libraries(valtzctl PRIVATE valtz::core)
valtz_target_warnings(valtzctl)
# far memory's wire (far-lend): shared with the plugin
target_include_directories(valtzctl PRIVATE
  "${PROJECT_SOURCE_DIR}/vpipe-plugin/include")
