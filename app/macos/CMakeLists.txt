# Valtz.app -- the SwiftUI front end.
#
# Built by CMake (Ninja or Xcode generator) as a MACOSX_BUNDLE. Swift
# talks to the core only through ValtzBridge (Swift/C++ interop).
#
# Dev builds run in place and find libvpipe through the absolute build
# RPATH CMake records. A distributable bundle -- libvpipe (and Sparkle,
# optionally LGPL FFmpeg) in Contents/Frameworks, `valtzctl` in
# Contents/Helpers, rpaths rewritten, everything signed -- is made from
# it by bundle-app.sh: the `valtz-dist` target (top-level CMakeLists.txt)
# runs it ad hoc, a release with a Developer ID.

set(VALTZ_BUNDLE_ID "com.tgous.valtz" CACHE STRING "Valtz bundle id")

# CFBundleVersion must increase monotonically for Sparkle to recognise an
# update, and must not depend on who builds. The source tree's commit
# count is both -- in the PUBLIC tree a release is built from. A tree
# without history (a tarball, a shallow clone) falls back rather than
# failing the configure. Taken at configure time: reconfigure to refresh.
execute_process(
  COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-list --count HEAD
  OUTPUT_VARIABLE VALTZ_APP_BUILD
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT VALTZ_APP_BUILD)
  set(VALTZ_APP_BUILD 1)
endif()

# Sparkle auto-update. Optional, and off unless a framework is supplied:
# a 15 MB binary dependency only a published build needs, so
# fetch-sparkle.sh gets it out of tree and this points at it. Without it
# the app builds and runs and never offers an update.
set(VALTZ_SPARKLE_DIR "" CACHE PATH
    "Directory containing Sparkle.framework (see fetch-sparkle.sh)")
# The feed: an asset of the LATEST GitHub release, so publishing a release
# is what moves it.
set(VALTZ_APP_FEED_URL
    "https://github.com/tgo-app-dev/valtz/releases/latest/download/appcast.xml"
    CACHE STRING "Sparkle appcast URL")
# The public half of Valtz's OWN EdDSA pair -- not vpipe's: one app's
# key leaking or rotating leaves the other's updates alone. Its private
# half is in the release Mac's keychain under the account "valtz"
# (`generate_keys --account valtz`), never in this tree; the release
# reads the public half from there, passes it in, and refuses one that
# disagrees with this. Empty until that key is made, and then auto-update
# is LEFT OUT: an updater that cannot verify an update installs none.
set(VALTZ_APP_ED_PUBKEY "" CACHE STRING
    "Sparkle EdDSA public key: Valtz's own (generate_keys --account valtz)")
set(VALTZ_SPARKLE OFF CACHE INTERNAL "Sparkle built in")
if(VALTZ_SPARKLE_DIR)
  if(NOT EXISTS "${VALTZ_SPARKLE_DIR}/Sparkle.framework")
    message(FATAL_ERROR
      "VALTZ_SPARKLE_DIR=${VALTZ_SPARKLE_DIR} has no Sparkle.framework. "
      "Run app/macos/fetch-sparkle.sh and pass the directory it prints.")
  endif()
  if(VALTZ_APP_ED_PUBKEY)
    set(VALTZ_SPARKLE ON CACHE INTERNAL "Sparkle built in")
    message(STATUS "valtz: auto-update     Sparkle from ${VALTZ_SPARKLE_DIR}")
  else()
    message(WARNING
      "VALTZ_SPARKLE_DIR is set but VALTZ_APP_ED_PUBKEY is empty: auto-"
      "update is left out, since Sparkle verifies every update against "
      "that key. Pass -DVALTZ_APP_ED_PUBKEY=<the public half of "
      "`generate_keys --account valtz`>.")
  endif()
else()
  message(STATUS "valtz: auto-update     off (no VALTZ_SPARKLE_DIR)")
endif()

set(VALTZ_APP_SOURCES
  Sources/ValtzApp.swift
  Sources/Core/CoreService.swift
  Sources/Core/Updater.swift
  Sources/Core/MachineMonitor.swift
  Sources/Core/MarkupDTO.swift
  Sources/Core/SettingsDTO.swift
  Sources/Core/L10n.swift
  Sources/Core/DTO.swift
  Sources/AppModel.swift
  Sources/Views/RootView.swift
  Sources/Views/CapabilitiesView.swift
  Sources/Views/HelperView.swift
  Sources/Views/StorageView.swift
  Sources/Views/SettingsView.swift
  Sources/Views/TitleBar.swift
  Sources/Views/InspectorView.swift
  Sources/Views/CanvasSize.swift
  Sources/Views/ProjectOutput.swift
  Sources/Views/AssetsSection.swift
  Sources/Views/StatusBar.swift
  Sources/Views/MarkupBar.swift
  Sources/Views/PromptBar.swift
  Sources/Views/LogView.swift
  Sources/Simple/SimpleView.swift
  Sources/Simple/ComposerStack.swift
  Sources/Simple/CropTrim.swift
  Sources/Simple/Keyframes.swift
  Sources/Simple/TuningPanel.swift
  Sources/Simple/Markup.swift
  Sources/Simple/MarkupTextPanel.swift
  Sources/Simple/MarkupRender.swift
  Sources/Simple/PromptEditor.swift
  Sources/Simple/PromptMarkdown.swift
  Sources/Simple/PromptStudio.swift
  Sources/Simple/SuggestionPane.swift
  Sources/Simple/ReferenceRow.swift
  Sources/Simple/Adjustments.swift
  Sources/Simple/SettingsSearch.swift
  Sources/Simple/AttachCapture.swift
  Sources/Simple/Capture.swift
  Sources/Simple/ClipTime.swift
  Sources/Viewer/CompareBar.swift
  Sources/Viewer/CompareCanvas.swift
  Sources/Viewer/GPUPicture.swift
  Sources/Viewer/StackCompositor.swift
  Sources/Viewer/AudioWaveform.swift
  Sources/Viewer/MotionViews.swift
  Sources/Viewer/MarkupOverlay.swift
  Sources/Viewer/ViewerView.swift
  Sources/Viewer/Guides.swift
)

# valtz-icon: renders the Valtz mark with the app's own Metal renderer
# (Sources/Mark). The build uses it to make the app icon.
set(VALTZ_MARK_SOURCES
  Sources/Mark/MarkGeometry.swift
  Sources/Mark/MarkRenderer.swift
  Sources/Mark/MarkShaders.swift
  Sources/Mark/MarkTrace.swift
)
add_executable(valtz-icon
  "${PROJECT_SOURCE_DIR}/tools/valtz-icon/main.swift"
  ${VALTZ_MARK_SOURCES})
set_target_properties(valtz-icon PROPERTIES Swift_LANGUAGE_VERSION 6)
target_link_libraries(valtz-icon PRIVATE
  "-framework Metal" "-framework CoreGraphics" "-framework ImageIO")

# Bundle resources (Contents/Resources). The mark in the app is drawn
# live by Sources/Mark, so there is no bitmap of it here.
set(VALTZ_APP_RESOURCES)

# The asset catalog -- today only the app's accent color (a near-black;
# near-white in the dark appearance), which AppKit and SwiftUI controls
# use for highlights instead of the system blue. Compiled with actool, as
# Xcode would; Info.plist names it (NSAccentColorName).
set(VALTZ_XCASSETS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Assets.xcassets")
set(VALTZ_ASSETS_CAR "${CMAKE_CURRENT_BINARY_DIR}/assets/Assets.car")
file(GLOB_RECURSE VALTZ_XCASSETS_FILES CONFIGURE_DEPENDS
     "${VALTZ_XCASSETS}/*.json")
add_custom_command(OUTPUT "${VALTZ_ASSETS_CAR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets"
  COMMAND xcrun actool "${VALTZ_XCASSETS}"
          --compile "${CMAKE_CURRENT_BINARY_DIR}/assets"
          --platform macosx
          --minimum-deployment-target ${CMAKE_OSX_DEPLOYMENT_TARGET}
          --accent-color AccentColor
          --output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/assets/partial.plist"
          --output-format human-readable-text >/dev/null
  DEPENDS ${VALTZ_XCASSETS_FILES}
  COMMENT "Compiling Assets.xcassets")
list(APPEND VALTZ_APP_RESOURCES "${VALTZ_ASSETS_CAR}")

# App icon: RENDERED at build time by valtz-icon, from the same geometry
# and shaders the app draws its mark with -- each size natively, at its
# own level of detail (no sprocket holes below 64 px), on the macOS icon
# grid. VALTZ_RENDER_ICON=OFF falls back to scaling the painted master
# (Resources/AppIcon-1024.png), for a build machine without a GPU.
option(VALTZ_RENDER_ICON "Render the app icon with Metal (valtz-icon)" ON)
set(VALTZ_ICON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon-1024.png")
set(VALTZ_ICONSET "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset")
set(VALTZ_ICNS "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
set(VALTZ_ICON_CMDS)
foreach(size 16 32 128 256 512)
  math(EXPR size2 "${size} * 2")
  foreach(pair "${size};icon_${size}x${size}.png"
               "${size2};icon_${size}x${size}@2x.png")
    list(GET pair 0 px)
    list(GET pair 1 name)
    # The 16 pt icon (16 px, and 32 px at 2x) is a flat white V: the
    # ribbon's shading does not survive that size.
    set(flat "")
    if(size EQUAL 16)
      set(flat --flat)
    endif()
    if(VALTZ_RENDER_ICON)
      list(APPEND VALTZ_ICON_CMDS COMMAND "$<TARGET_FILE:valtz-icon>"
           --size ${px} ${flat} --out "${VALTZ_ICONSET}/${name}")
    else()
      list(APPEND VALTZ_ICON_CMDS COMMAND sips -z ${px} ${px}
           "${VALTZ_ICON_SRC}" --out "${VALTZ_ICONSET}/${name}" >/dev/null)
    endif()
  endforeach()
endforeach()
if(VALTZ_RENDER_ICON)
  set(VALTZ_ICON_DEPENDS valtz-icon)
else()
  set(VALTZ_ICON_DEPENDS "${VALTZ_ICON_SRC}")
endif()
add_custom_command(OUTPUT "${VALTZ_ICNS}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${VALTZ_ICONSET}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${VALTZ_ICONSET}"
  ${VALTZ_ICON_CMDS}
  COMMAND iconutil -c icns "${VALTZ_ICONSET}" -o "${VALTZ_ICNS}"
  DEPENDS ${VALTZ_ICON_DEPENDS}
  COMMENT "Generating AppIcon.icns")
list(APPEND VALTZ_APP_RESOURCES "${VALTZ_ICNS}")

set_source_files_properties(${VALTZ_APP_RESOURCES} PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources)

# Third-party text compiled into the core travels with its license and
# the notice that license requires (3rd-party/minimax-h3).
set(VALTZ_H3_LICENSE
  "${PROJECT_SOURCE_DIR}/3rd-party/minimax-h3/LICENSE"
  "${PROJECT_SOURCE_DIR}/3rd-party/minimax-h3/NOTICE")
set_source_files_properties(${VALTZ_H3_LICENSE} PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources/Licenses/MiniMax-H3)
list(APPEND VALTZ_APP_RESOURCES ${VALTZ_H3_LICENSE})

# ---- Localization (DESIGN §10b) ------------------------------------------
#
# One String Catalog, Resources/Localizable.xcstrings, holds every UI
# string in every language -- the app's own, and the core's message keys
# (core/include/valtz/base/message.h). Xcode would compile it; with Ninja
# the build does the same with Apple's tool: `xcstringstool compile` into
# <lang>.lproj/Localizable.strings(dict), copied into the bundle.
#
# The catalog is kept up to date from the code: swiftc emits the strings
# each source passes to a localizing API (-emit-localized-strings), and
#   cmake --build --preset dev --target valtz-update-strings
# merges them into the catalog in the source tree (new keys added, keys
# no longer used marked stale). Run it after changing UI text; it is not
# part of the ordinary build, which never writes the source tree.
#
# VALTZ_LOCALIZATIONS lists the languages the bundle declares
# (CFBundleLocalizations); a language needs its translations in the
# catalog to appear.
set(VALTZ_LOCALIZATIONS "en;zh-Hans" CACHE STRING
    "Languages Valtz.app declares (BCP-47)")
set(VALTZ_STRINGS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Localizable.xcstrings")
set(VALTZ_LOC_DIR "${CMAKE_CURRENT_BINARY_DIR}/loc")
set(VALTZ_LOC_STAMP "${VALTZ_LOC_DIR}/compiled.stamp")
set(VALTZ_STRINGSDATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/stringsdata")
add_custom_command(OUTPUT "${VALTZ_LOC_STAMP}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${VALTZ_LOC_DIR}/lproj"
  COMMAND xcrun xcstringstool compile "${VALTZ_STRINGS_SRC}"
          --output-directory "${VALTZ_LOC_DIR}/lproj"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${VALTZ_LOC_DIR}/lproj"
  COMMAND ${CMAKE_COMMAND} -E touch "${VALTZ_LOC_STAMP}"
  DEPENDS "${VALTZ_STRINGS_SRC}"
  COMMENT "Compiling Localizable.xcstrings")
add_custom_target(valtz-loc DEPENDS "${VALTZ_LOC_STAMP}")
# The strings of the sources the app is built from, by name: a deleted
# source's file stays in the build tree, and its strings would come back.
set(VALTZ_STRINGSDATA "")
foreach(src IN LISTS VALTZ_APP_SOURCES VALTZ_MARK_SOURCES
        ITEMS Sources/Mark/MarkView.swift)
  if(src MATCHES "\\.swift$")
    get_filename_component(stem "${src}" NAME_WE)
    list(APPEND VALTZ_STRINGSDATA "${VALTZ_STRINGSDATA_DIR}/${stem}.stringsdata")
  endif()
endforeach()
add_custom_target(valtz-update-strings
  COMMAND xcrun xcstringstool sync "${VALTZ_STRINGS_SRC}"
          --stringsdata ${VALTZ_STRINGSDATA}
  DEPENDS Valtz
  COMMENT "Updating Localizable.xcstrings from the Swift sources"
  VERBATIM)
string(REPLACE ";" "</string><string>" VALTZ_LOCALIZATIONS_PLIST
       "${VALTZ_LOCALIZATIONS}")

configure_file(Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

add_executable(Valtz MACOSX_BUNDLE ${VALTZ_APP_SOURCES}
  ${VALTZ_MARK_SOURCES} Sources/Mark/MarkView.swift
  ${VALTZ_APP_RESOURCES})
target_link_libraries(Valtz PRIVATE valtz_bridge)
target_compile_options(Valtz PRIVATE
  "$<$<COMPILE_LANGUAGE:Swift>:-parse-as-library>"
  "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-emit-localized-strings -emit-localized-strings-path ${VALTZ_STRINGSDATA_DIR}>")
file(MAKE_DIRECTORY "${VALTZ_STRINGSDATA_DIR}")
set_target_properties(Valtz PROPERTIES
  Swift_LANGUAGE_VERSION 6
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${VALTZ_BUNDLE_ID}"
  XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
    "${CMAKE_CURRENT_SOURCE_DIR}/Valtz.entitlements")

# The compiled strings ride in Contents/Resources/<lang>.lproj. Relink (and
# so re-copy and re-sign) when only the catalog changed.
add_dependencies(Valtz valtz-loc)
set_property(TARGET Valtz APPEND PROPERTY LINK_DEPENDS "${VALTZ_LOC_STAMP}")
add_custom_command(TARGET Valtz POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${VALTZ_LOC_DIR}/lproj"
          "$<TARGET_BUNDLE_CONTENT_DIR:Valtz>/Resources"
  COMMENT "Copying localizations into Valtz.app"
  VERBATIM)

# The engine's vpipe plugin (vpipe-plugin/) rides in Contents/PlugIns,
# where the engine looks first. Nested code is signed before the bundle
# that seals it.
if(TARGET valtz-vpipe)
  add_dependencies(Valtz valtz-vpipe)
  # Relink (and so re-copy and re-sign) when only the plugin changed.
  set_property(TARGET Valtz APPEND PROPERTY
    LINK_DEPENDS "$<TARGET_FILE:valtz-vpipe>")
  add_custom_command(TARGET Valtz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:Valtz>/PlugIns"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:valtz-vpipe>"
            "$<TARGET_BUNDLE_CONTENT_DIR:Valtz>/PlugIns/"
    COMMAND codesign --force --sign - --options runtime
            "$<TARGET_BUNDLE_CONTENT_DIR:Valtz>/PlugIns/$<TARGET_FILE_NAME:valtz-vpipe>"
    COMMENT "Copying valtz-vpipe.so into Valtz.app"
    VERBATIM)
endif()

# Sparkle, when built in: `import Sparkle` cannot even be parsed without
# the framework, so the Swift sources guard it with `#if VALTZ_SPARKLE`
# and the same sources build both ways. A dev build carries the framework
# in Contents/Frameworks so it runs as built; bundle-app.sh replaces it.
if(VALTZ_SPARKLE)
  target_compile_definitions(Valtz PRIVATE VALTZ_SPARKLE)
  target_compile_options(Valtz PRIVATE
    "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-F ${VALTZ_SPARKLE_DIR}>")
  target_link_options(Valtz PRIVATE
    "SHELL:-F ${VALTZ_SPARKLE_DIR}" "SHELL:-framework Sparkle")
  set_property(TARGET Valtz APPEND PROPERTY
    BUILD_RPATH "@executable_path/../Frameworks")
  add_custom_command(TARGET Valtz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:Valtz>/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_BUNDLE_CONTENT_DIR:Valtz>/Frameworks/Sparkle.framework"
    # cp -R, not copy_directory: a framework is a symlink farm.
    COMMAND cp -R "${VALTZ_SPARKLE_DIR}/Sparkle.framework"
            "$<TARGET_BUNDLE_CONTENT_DIR:Valtz>/Frameworks/"
    COMMENT "Copying Sparkle.framework into Valtz.app"
    VERBATIM)
endif()

# Ad-hoc sign after linking so the hardened-runtime entitlements (library
# validation off, for dlopen'd FFmpeg and vpipe plugins) are exercised in
# dev builds the same way they will be in release.
add_custom_command(TARGET Valtz POST_BUILD
  COMMAND codesign --force --sign - --options runtime
          --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/Valtz.entitlements"
          "$<TARGET_BUNDLE_DIR:Valtz>"
  COMMENT "Ad-hoc signing Valtz.app"
  VERBATIM)
