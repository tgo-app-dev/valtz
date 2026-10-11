#!/bin/bash
#
# check-ffmpeg.sh -- is this FFmpeg one Valtz may bundle beside libvpipe?
#
# Modelled on vpipe's with-ffmpeg release, whose macOS app bundles one
# the same way: a bundled FFmpeg is an LGPL-only build (vpipe's
# apps/macos-app/build-lgpl-ffmpeg.sh makes one), named in the notice by
# its EXACT version -- the version names the corresponding source the
# notice offers. And here, besides: it is the FFmpeg libvpipe was
# COMPILED against. vpipe dlopens FFmpeg but bakes the headers' struct
# layouts into its objects, so libraries of another major load fine and
# then corrupt every call that passes a struct.
#
# Checks, each refused by name:
#   * the directory holds libav*/libsw* dylibs;
#   * none was configured --enable-gpl, --enable-nonfree or
#     --enable-version3 (Homebrew's is GPL: it would put the whole app
#     under the GPL);
#   * its version is readable (libavutil/ffversion.h beside it), or given;
#   * its libraries' majors are the ones vpipe's install records
#     (<prefix>/share/vpipe/ffmpeg-headers, written by vpipe since its
#     FFmpeg header record: a vpipe configured -DVPIPE_BUNDLE_FFMPEG=ON
#     -DVPIPE_FFMPEG_BUNDLE_DIR=<this directory> is compiled against it).
#
# Prints the version on success.
#
# Usage: check-ffmpeg.sh --ffmpeg-dir DIR --libvpipe PATH [--version V]

set -euo pipefail

FFMPEG_DIR=""; LIBVPIPE=""; VERSION=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ffmpeg-dir) FFMPEG_DIR="$2"; shift 2 ;;
    --libvpipe)   LIBVPIPE="$2"; shift 2 ;;
    --version)    VERSION="$2"; shift 2 ;;
    -h|--help)    sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "check-ffmpeg.sh: unknown argument '$1'" >&2; exit 2 ;;
  esac
done
if [ -z "$FFMPEG_DIR" ] || [ -z "$LIBVPIPE" ]; then
  echo "check-ffmpeg.sh: --ffmpeg-dir and --libvpipe are required" >&2
  exit 2
fi
fail_() { echo "check-ffmpeg.sh: $*" >&2; exit 1; }

[ -d "$FFMPEG_DIR" ] || fail_ "'$FFMPEG_DIR' is not a directory"
reals=()
for lib in "$FFMPEG_DIR"/libav*.dylib "$FFMPEG_DIR"/libsw*.dylib; do
  [ -f "$lib" ] && [ ! -L "$lib" ] && reals+=("$lib")
done
[ "${#reals[@]}" -gt 0 ] ||
  fail_ "no libav*/libsw* dylibs in '$FFMPEG_DIR'"

# LGPL only: what each library was configured with is compiled into it.
for lib in "${reals[@]}"; do
  bad="$(strings "$lib" |
         grep -oE -- '--enable-(gpl|nonfree|version3)' | sort -u |
         tr '\n' ' ' || true)"
  if [ -n "$bad" ]; then
    fail_ "$(basename "$lib") was configured ${bad}-- not an LGPL-2.1" \
          "build, which is all Valtz may ship. Build one with vpipe's" \
          "apps/macos-app/build-lgpl-ffmpeg.sh."
  fi
done

# Its version: what the notice names, and the source it offers.
if [ -z "$VERSION" ]; then
  for inc in "$FFMPEG_DIR/../include" "$FFMPEG_DIR/include"; do
    f="$inc/libavutil/ffversion.h"
    if [ -z "$VERSION" ] && [ -f "$f" ]; then
      VERSION="$(sed -nE 's/^#define[ \t]+FFMPEG_VERSION[ \t]+"([^"]+)".*/\1/p' \
                 "$f")"
    fi
  done
fi
[ -n "$VERSION" ] ||
  fail_ "cannot tell its version (no libavutil/ffversion.h beside" \
        "'$FFMPEG_DIR'): give it (--ffmpeg-version). It names the source" \
        "the LGPL notice offers, so it is never guessed."

# The majors libvpipe was compiled against, against the ones here.
record="$(dirname "$LIBVPIPE")/../share/vpipe/ffmpeg-headers"
want="$(sed -n 's/^abi=//p' "$record" 2>/dev/null || true)"
built="$(sed -n 's/^version=//p' "$record" 2>/dev/null || true)"
have=""
for l in libavutil libavcodec libavformat libswresample libswscale; do
  m="$(ls "$FFMPEG_DIR" 2>/dev/null |
       sed -nE "s/^$l\.([0-9]+)\.dylib$/\1/p" | head -1)"
  have="$have$l${m:-x}-"
done
if [ -z "$want" ]; then
  fail_ "libvpipe's install records no FFmpeg headers ($record). Use a" \
        "vpipe installed since it records them, configured" \
        "-DVPIPE_BUNDLE_FFMPEG=ON -DVPIPE_FFMPEG_BUNDLE_DIR=$FFMPEG_DIR" \
        "-- compiled against the FFmpeg it will ship."
fi
if [ "$want" != "$have" ]; then
  fail_ "libvpipe was compiled against FFmpeg ${built:-?} ($want) and" \
        "'$FFMPEG_DIR' holds FFmpeg $VERSION ($have): a struct passed" \
        "between them would be misread. Rebuild and reinstall vpipe" \
        "configured -DVPIPE_BUNDLE_FFMPEG=ON" \
        "-DVPIPE_FFMPEG_BUNDLE_DIR=$FFMPEG_DIR, and bundle from that" \
        "install (--vpipe-prefix)."
fi
echo "$VERSION"
