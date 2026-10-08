#!/bin/bash
#
# bundle-app.sh -- make a RELOCATABLE, signed Valtz.app from a built one.
#
# The build's Valtz.app runs in place: it finds libvpipe through an
# absolute rpath into the vpipe installation it was built against. This
# turns it into a bundle that runs on a Mac with neither that
# installation nor a build tree:
#
#   * libvpipe (and, when asked, LGPL FFmpeg and Sparkle) is copied into
#     Contents/Frameworks, `valtzctl` into Contents/Helpers;
#   * every absolute LC_RPATH is DELETED, not appended to -- on the build
#     machine it still resolves, so a bundle that kept it would load the
#     build tree's libvpipe and seem to work until opened anywhere else --
#     and each Mach-O is pointed at Frameworks instead;
#   * every non-system dependency is copied in and rewritten to @rpath,
#     chased to a fixpoint, then checked: nothing may still point outside
#     the bundle, and every @rpath name must exist inside it;
#   * the licences of what it carries go in Contents/Resources/Licenses;
#   * everything is signed inside out, the app last.
#
# Modelled on vpipe's apps/macos-app/bundle-app.sh, whose comments record
# why each step is the way it is.
#
# Used by the `valtz-dist` target (ad hoc) and by the release, with a
# Developer ID. Usage: see print_usage_ below.

set -euo pipefail

APP_IN=""; APP=""; LIBVPIPE=""; VALTZCTL=""; VPIPE_DOC=""; SOURCE_DIR=""
ENTITLEMENTS=""; FFMPEG_DIR=""; SPARKLE_FRAMEWORK=""; SIGN_ID="-"

print_usage_() {
  cat <<'EOF'
Usage: bundle-app.sh --app-in BUILT.app --app OUT.app --libvpipe PATH
                     --source-dir DIR [--valtzctl PATH] [--vpipe-doc DIR]
                     [--entitlements PATH] [--ffmpeg-dir DIR]
                     [--sparkle-framework PATH] [--sign-identity ID]

  --app-in             the build's Valtz.app (left untouched)
  --app                bundle to create (removed and rebuilt)
  --libvpipe           the libvpipe dylib -> Contents/Frameworks/<its id>
  --source-dir         the Valtz source tree (LICENSE, NOTICE,
                       THIRD_PARTY_LICENSES.md, app/macos/licenses)
  --valtzctl           the CLI -> Contents/Helpers/valtzctl
  --vpipe-doc          vpipe's installed share/doc/vpipe (its LICENSE,
                       NOTICE, THIRD_PARTY_LICENSES.md) -> Licenses/vpipe
  --entitlements       entitlements for the executables (required with a
                       real identity)
  --ffmpeg-dir         directory of LGPL libav*/libsw* dylibs to bundle.
                       Omit to leave FFmpeg to the host.
  --sparkle-framework  Sparkle.framework to bundle (auto-update). Omit for
                       a build without it.
  --sign-identity      codesign identity; "-" (default) is ad hoc: fine
                       to run locally, cannot be notarized.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --app-in)            APP_IN="$2"; shift 2 ;;
    --app)               APP="$2"; shift 2 ;;
    --libvpipe)          LIBVPIPE="$2"; shift 2 ;;
    --source-dir)        SOURCE_DIR="$2"; shift 2 ;;
    --valtzctl)          VALTZCTL="$2"; shift 2 ;;
    --vpipe-doc)         VPIPE_DOC="$2"; shift 2 ;;
    --entitlements)      ENTITLEMENTS="$2"; shift 2 ;;
    --ffmpeg-dir)        FFMPEG_DIR="$2"; shift 2 ;;
    --sparkle-framework) SPARKLE_FRAMEWORK="$2"; shift 2 ;;
    --sign-identity)     SIGN_ID="$2"; shift 2 ;;
    -h|--help)           print_usage_; exit 0 ;;
    *) echo "bundle-app.sh: unknown argument '$1'" >&2
       print_usage_ >&2; exit 2 ;;
  esac
done

require_() {
  if [ -z "$2" ]; then
    echo "bundle-app.sh: $1 is required" >&2; exit 2
  fi
}
require_ --app-in     "$APP_IN"
require_ --app        "$APP"
require_ --libvpipe   "$LIBVPIPE"
require_ --source-dir "$SOURCE_DIR"
if [ "$SIGN_ID" != "-" ] && [ -z "$ENTITLEMENTS" ]; then
  echo "bundle-app.sh: --entitlements is required with a real identity" >&2
  exit 2
fi
if [ ! -d "$APP_IN/Contents/MacOS" ]; then
  echo "bundle-app.sh: '$APP_IN' is not a built app bundle" >&2; exit 1
fi

CONTENTS="$APP/Contents"
FRAMEWORKS="$CONTENTS/Frameworks"
LICENSES="$CONTENTS/Resources/Licenses"

# codesign hands entitlements to AMFI, whose XML parser is stricter than
# `plutil -lint` (a "--" inside a comment passes one and fails the other,
# at the very end of a release). Fail here, with the reason.
if [ -n "$ENTITLEMENTS" ] && command -v xmllint >/dev/null 2>&1; then
  if ! err="$(xmllint --noout "$ENTITLEMENTS" 2>&1)"; then
    echo "bundle-app.sh: $ENTITLEMENTS is not valid XML:" >&2
    printf '%s\n' "$err" | sed 's/^/  /' >&2
    exit 1
  fi
fi

# ---------------------------------------------------------------------
# helpers (see vpipe's bundle-app.sh for why each is written so)
# ---------------------------------------------------------------------

# Real dependencies are TAB-indented in otool's output; the per-
# architecture headers of a universal binary are not.
deps_of_() {
  otool -L "$1" 2>/dev/null | grep '^	' | awk '{print $1}'
}

# The install name a Mach-O records for ITSELF (never a dependency).
own_id_() {
  otool -D "$1" 2>/dev/null | grep -v '(architecture ' | grep -v ':$' \
    | head -1 || true
}

is_system_dep_() {
  case "$1" in
    /usr/lib/*|/System/*) return 0 ;;
    @rpath/*|@loader_path/*|@executable_path/*) return 0 ;;
    *) return 1 ;;
  esac
}

# Every Mach-O we are responsible for, frameworks excluded (they arrive
# self-contained and are signed apart, inside out).
bundle_machos_() {
  find "$CONTENTS/MacOS" "$CONTENTS/Helpers" "$CONTENTS/PlugIns" \
       "$FRAMEWORKS" -type f 2>/dev/null | grep -v '\.framework/' |
    while read -r f; do
      if file -b "$f" | grep -q "Mach-O"; then echo "$f"; fi
    done
}

# Every LC_RPATH deleted, then `$2` added.
reset_rpaths_() {
  local bin="$1" rp
  while read -r rp; do
    [ -n "$rp" ] || continue
    install_name_tool -delete_rpath "$rp" "$bin" 2>/dev/null || true
  done < <(otool -l "$bin" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}')
  install_name_tool -add_rpath "$2" "$bin"
}

# ---------------------------------------------------------------------
# 1. lay out the bundle
# ---------------------------------------------------------------------
echo "bundle-app.sh: $APP_IN -> $APP"
rm -rf "$APP"
mkdir -p "$(dirname "$APP")"
# cp -R keeps symlinks as symlinks (a framework is a symlink farm).
cp -R "$APP_IN" "$APP"
# The build's ad hoc signature is replaced below; its seal would only be
# wrong once anything inside changes.
rm -rf "$CONTENTS/_CodeSignature"
mkdir -p "$FRAMEWORKS" "$CONTENTS/Helpers" "$LICENSES"

# libvpipe under the name its dependents ask for: its own install id
# (@rpath/libvpipe.0.dylib), not the versioned file it was built as.
vpipe_name="$(basename "$(own_id_ "$LIBVPIPE")")"
[ -n "$vpipe_name" ] || vpipe_name="$(basename "$LIBVPIPE")"
cp -L "$LIBVPIPE" "$FRAMEWORKS/$vpipe_name"
chmod u+w "$FRAMEWORKS/$vpipe_name"
echo "bundle-app.sh: + $vpipe_name"

if [ -n "$VALTZCTL" ]; then
  cp "$VALTZCTL" "$CONTENTS/Helpers/valtzctl"
  echo "bundle-app.sh: + Helpers/valtzctl"
fi

# FFmpeg: real files AND their version symlinks (dependents ask for the
# middle name, @rpath/libavutil.59.dylib).
if [ -n "$FFMPEG_DIR" ]; then
  if [ ! -d "$FFMPEG_DIR" ]; then
    echo "bundle-app.sh: --ffmpeg-dir '$FFMPEG_DIR' is not a directory" >&2
    exit 1
  fi
  n=0
  for lib in "$FFMPEG_DIR"/libav*.dylib "$FFMPEG_DIR"/libsw*.dylib; do
    [ -e "$lib" ] || continue
    cp -a "$lib" "$FRAMEWORKS/"
    if [ ! -L "$lib" ]; then n=$((n + 1)); fi
  done
  if [ "$n" -eq 0 ]; then
    echo "bundle-app.sh: no libav*/libsw* dylibs in '$FFMPEG_DIR'" >&2
    exit 1
  fi
  echo "bundle-app.sh: + $n FFmpeg libraries"
fi

# Sparkle, symlinks preserved. The build may have put one there already
# (a dev build runs with it); the one asked for wins.
if [ -n "$SPARKLE_FRAMEWORK" ]; then
  if [ ! -d "$SPARKLE_FRAMEWORK" ]; then
    echo "bundle-app.sh: --sparkle-framework '$SPARKLE_FRAMEWORK' is not" \
         "a directory" >&2
    exit 1
  fi
  rm -rf "$FRAMEWORKS/Sparkle.framework"
  cp -R "$SPARKLE_FRAMEWORK" "$FRAMEWORKS/"
  echo "bundle-app.sh: + Sparkle.framework"
fi

# ---------------------------------------------------------------------
# 2. licences
# ---------------------------------------------------------------------
# What the bundle carries, it carries the licences of. (The ones of text
# compiled into the core -- Qwen-Image 2.1, MiniMax H3 -- the build has
# put here already.)
mkdir -p "$LICENSES/Valtz"
for f in LICENSE NOTICE THIRD_PARTY_LICENSES.md; do
  cp "$SOURCE_DIR/$f" "$LICENSES/Valtz/"
done
if [ -n "$VPIPE_DOC" ] && [ -d "$VPIPE_DOC" ]; then
  mkdir -p "$LICENSES/vpipe"
  cp "$VPIPE_DOC"/* "$LICENSES/vpipe/"
  # Apache-2.0 section 4(d): libvpipe travels with its NOTICE. An install
  # from before vpipe installed it lacks the file.
  if [ ! -f "$LICENSES/vpipe/NOTICE" ]; then
    echo "bundle-app.sh: $VPIPE_DOC has no NOTICE -- reinstall vpipe" \
         "(its install carries it since 2026-10-06)" >&2
    exit 1
  fi
else
  echo "bundle-app.sh: --vpipe-doc is required: the bundle carries" \
       "libvpipe, which carries its licence" >&2
  exit 1
fi
if [ -d "$FRAMEWORKS/Sparkle.framework" ]; then
  mkdir -p "$LICENSES/Sparkle"
  cp "$SOURCE_DIR/app/macos/licenses/MIT-Sparkle.txt" "$LICENSES/Sparkle/"
fi
if [ -n "$FFMPEG_DIR" ]; then
  mkdir -p "$LICENSES/FFmpeg"
  cp "$SOURCE_DIR/app/macos/licenses/LGPL-2.1.txt" "$LICENSES/FFmpeg/"
fi

# ---------------------------------------------------------------------
# 3. rpaths
# ---------------------------------------------------------------------
# The executables live one level under Contents; the plugin is dlopen'd,
# so its own rpath resolves against itself (@loader_path).
reset_rpaths_ "$CONTENTS/MacOS/Valtz" "@executable_path/../Frameworks"
if [ -f "$CONTENTS/Helpers/valtzctl" ]; then
  reset_rpaths_ "$CONTENTS/Helpers/valtzctl" "@executable_path/../Frameworks"
fi
for so in "$CONTENTS/PlugIns"/*.so; do
  [ -f "$so" ] || continue
  reset_rpaths_ "$so" "@loader_path/../Frameworks"
done
# A dylib in Frameworks resolving a sibling. Symlinks skipped: each would
# rewrite its target again.
for lib in "$FRAMEWORKS"/*.dylib; do
  [ -f "$lib" ] || continue
  [ -L "$lib" ] && continue
  reset_rpaths_ "$lib" "@loader_path"
done

# ---------------------------------------------------------------------
# 4. relocate dependencies to a fixpoint
# ---------------------------------------------------------------------
pass=0
while : ; do
  pass=$((pass + 1))
  changed=0
  while read -r macho; do
    [ -n "$macho" ] || continue
    id="$(own_id_ "$macho")"
    while read -r dep; do
      [ -n "$dep" ] || continue
      [ "$dep" = "$id" ] && continue
      if is_system_dep_ "$dep"; then continue; fi
      base="$(basename "$dep")"
      if [ ! -f "$FRAMEWORKS/$base" ]; then
        if [ ! -f "$dep" ]; then
          echo "bundle-app.sh: WARNING: $macho needs '$dep', which does" \
               "not exist -- leaving the reference alone" >&2
          continue
        fi
        cp "$dep" "$FRAMEWORKS/$base"
        chmod u+w "$FRAMEWORKS/$base"
        install_name_tool -id "@rpath/$base" "$FRAMEWORKS/$base"
        install_name_tool -add_rpath "@loader_path" \
                          "$FRAMEWORKS/$base" 2>/dev/null || true
        echo "bundle-app.sh: + $base  (from $dep)"
        changed=1
      fi
      install_name_tool -change "$dep" "@rpath/$base" "$macho"
      changed=1
    done < <(deps_of_ "$macho")
  done < <(bundle_machos_)
  [ "$changed" -eq 0 ] && break
  if [ "$pass" -ge 16 ]; then
    echo "bundle-app.sh: dependency relocation did not settle after" \
         "$pass passes" >&2
    exit 1
  fi
done
echo "bundle-app.sh: dependencies settled after $pass pass(es)"

# Each Frameworks dylib calls itself @rpath/<name> -- unless it already
# has an @rpath id (FFmpeg's is its soname, deliberately not its file).
for lib in "$FRAMEWORKS"/*.dylib; do
  [ -f "$lib" ] || continue
  [ -L "$lib" ] && continue
  case "$(own_id_ "$lib")" in
    @rpath/*) continue ;;
  esac
  install_name_tool -id "@rpath/$(basename "$lib")" "$lib"
done

# ---------------------------------------------------------------------
# 5. verify before signing
# ---------------------------------------------------------------------
# Both failures are invisible on the build machine, where the old paths
# still resolve.
leaks=0; missing=0
while read -r macho; do
  [ -n "$macho" ] || continue
  id="$(own_id_ "$macho")"
  while read -r dep; do
    [ -n "$dep" ] || continue
    [ "$dep" = "$id" ] && continue
    case "$dep" in
      @rpath/*)
        if [ ! -e "$FRAMEWORKS/${dep#@rpath/}" ]; then
          echo "bundle-app.sh: MISSING: $(basename "$macho") needs" \
               "'$dep', which is not in Frameworks" >&2
          missing=$((missing + 1))
        fi
        ;;
      *)
        if ! is_system_dep_ "$dep"; then
          echo "bundle-app.sh: LEAK: $(basename "$macho") -> $dep" >&2
          leaks=$((leaks + 1))
        fi
        ;;
    esac
  done < <(deps_of_ "$macho")
  while read -r rp; do
    case "$rp" in
      @executable_path/*|@loader_path|@loader_path/*) ;;
      *) echo "bundle-app.sh: LEAK: $(basename "$macho") rpath $rp" >&2
         leaks=$((leaks + 1)) ;;
    esac
  done < <(otool -l "$macho" | awk '/LC_RPATH/{f=1} f&&/path /{print $2; f=0}')
done < <(bundle_machos_)
if [ "$leaks" -gt 0 ] || [ "$missing" -gt 0 ]; then
  echo "bundle-app.sh: $leaks reference(s) outside the bundle, $missing" \
       "missing from it; refusing to sign. It would run here and fail" \
       "on a clean Mac." >&2
  exit 1
fi
echo "bundle-app.sh: every reference inside the bundle"

# Nothing in it may need a newer macOS than the app says it runs on
# (LSMinimumSystemVersion): dyld refuses a binary built for a later
# system, and the app does not open at all -- as when swiftc, given no
# target, built the app for the release machine's macOS 27 while the
# app said 26.2, or a dylib comes from a package manager's bottle for
# the build machine's own release. Frameworks' binaries included.
floor="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' \
           "$CONTENTS/Info.plist" 2>/dev/null || true)"
if [ -n "$floor" ]; then
  newer=0
  while read -r macho; do
    [ -n "$macho" ] || continue
    file -b "$macho" | grep -q "Mach-O" || continue
    minos="$(otool -l "$macho" | awk '
      /LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}
      /LC_VERSION_MIN_MACOSX/ {v=1} v && /version/ {print $2; exit}')"
    [ -n "$minos" ] || continue
    if [ "$(printf '%s\n%s\n' "$floor" "$minos" | sort -V | tail -1)" \
         != "$floor" ]; then
      echo "bundle-app.sh: TOO NEW: ${macho#"$CONTENTS/"} needs macOS" \
           "$minos, the app says $floor" >&2
      newer=$((newer + 1))
    fi
  done < <(find "$CONTENTS" -type f \( -perm -u+x -o -name '*.dylib' \
                 -o -name '*.so' \) 2>/dev/null)
  if [ "$newer" -gt 0 ]; then
    echo "bundle-app.sh: $newer binary(ies) need a newer macOS than" \
         "$floor; refusing to sign. Rebuild them for $floor" \
         "(CMAKE_OSX_DEPLOYMENT_TARGET, MACOSX_DEPLOYMENT_TARGET)." >&2
    exit 1
  fi
  echo "bundle-app.sh: every binary runs on macOS $floor"
fi

# ---------------------------------------------------------------------
# 6. sign, inside out
# ---------------------------------------------------------------------
# Nested code before its container: signing the app seals a hash of
# everything in it. Hardened runtime (notarization requires it) and a
# trusted timestamp with a real identity; ad hoc stays simple.
sign_args=(--force --timestamp --options runtime --sign "$SIGN_ID")
if [ "$SIGN_ID" = "-" ]; then
  sign_args=(--force --sign "-")
fi
if [ -n "$ENTITLEMENTS" ]; then
  sign_exe_args=("${sign_args[@]}" --entitlements "$ENTITLEMENTS")
else
  sign_exe_args=("${sign_args[@]}")
fi

for lib in "$FRAMEWORKS"/*.dylib; do
  [ -f "$lib" ] || continue
  [ -L "$lib" ] && continue
  codesign "${sign_args[@]}" "$lib"
done

# Sparkle's own helpers (XPC services, Updater.app, Autoupdate) before
# the framework that holds them, each version directory being where the
# framework's seal lives. None get Valtz's entitlements: an updater has
# no use for a microphone or for loading unsigned code.
if [ -d "$FRAMEWORKS/Sparkle.framework" ]; then
  SPK="$FRAMEWORKS/Sparkle.framework"
  for v in "$SPK"/Versions/*/; do
    [ -d "$v" ] || continue
    case "$(basename "$v")" in Current) continue ;; esac
    for xpc in "$v"XPCServices/*.xpc; do
      if [ -d "$xpc" ]; then codesign "${sign_args[@]}" "$xpc"; fi
    done
    if [ -d "${v}Updater.app" ]; then
      codesign "${sign_args[@]}" "${v}Updater.app"
    fi
    if [ -f "${v}Autoupdate" ]; then
      codesign "${sign_args[@]}" "${v}Autoupdate"
    fi
    codesign "${sign_args[@]}" "$v"
  done
fi

# The engine's plugin: loaded into Valtz and valtzctl, it takes no
# entitlements of its own.
for so in "$CONTENTS/PlugIns"/*.so; do
  [ -f "$so" ] || continue
  codesign "${sign_args[@]}" "$so"
done

# valtzctl loads what the app loads (libvpipe, its plugin, extension
# backends), so it runs under the same entitlements.
if [ -f "$CONTENTS/Helpers/valtzctl" ]; then
  codesign "${sign_exe_args[@]}" "$CONTENTS/Helpers/valtzctl"
fi

codesign "${sign_exe_args[@]}" "$APP"
codesign --verify --deep --strict --verbose=1 "$APP"
echo "bundle-app.sh: $APP signed with identity '$SIGN_ID'"
