#!/bin/bash
# The app end to end: Valtz.app launched once a scenario with its
# snapshot hooks (VALTZ_SNAPSHOT_TASKS / _AGENT, app/macos/Sources/
# ValtzApp.swift) driving the real UI paths over the installed models;
# each scenario's stderr `snapshot:` lines checked -- no error logged,
# no memory failure, the assets it should have made -- and its window
# captures kept. Usage: app-run.sh [quick|full] [scenario...]
#
# Run it in the foreground of a shell with Screen Recording permission:
# the captures use ScreenCaptureKit, which fails from a background shell
# or a sleeping display (it wakes the display first).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"

tier="${1:-full}"
shift || true
only=("$@")
work="$E2E_OUT/app"
mkdir -p "$work"
bin="$VALTZ_APP/Contents/MacOS/Valtz"

# The value of `key=` in the last full state line ("snapshot: ... assets=").
state_of() {  # file key
  grep -E '^snapshot: .*assets=' "$1" | tail -1 | tr ' ' '\n' |
    awk -F= -v k="$2" '$1 == k { sub(/^[^=]*=/, ""); print; exit }'
}

# scenario <name> <needs ids|-> <tier> <timeout s> <expect-assets>= ... -- env assignments...
# Each env assignment is passed to the app; VALTZ_SNAPSHOT and the project
# are set here. The scenario passes when the app exited, logged no error,
# had no memory failure, and holds at least <expect-assets> assets.
scenario() {
  local name="$1" needs="$2" when="$3" limit="$4" want="$5"
  shift 6
  if [ ${#only[@]} -gt 0 ] && [[ ! " ${only[*]} " =~ " $name " ]]; then
    return 0
  fi
  if [ "$when" = full ] && [ "$tier" = quick ]; then
    return 0
  fi
  if [ "$needs" != - ]; then
    local miss
    miss="$(missing_of ${needs//,/ })"
    if [ -n "$miss" ]; then
      record app "$name" skip 0 "needs $miss"
      return 0
    fi
  fi
  local out="$work/$name" t0 rc
  mkdir -p "$out"
  rm -rf "$out/$name.valtz"
  t0="$(now)"
  caffeinate -u -t 2 >/dev/null 2>&1
  env "$@" \
    VALTZ_DEFAULT_PROJECT="$out/$name.valtz" \
    VALTZ_SNAPSHOT="$out/final.png" \
    VALTZ_SNAPSHOT_DELAY="${VALTZ_SNAPSHOT_DELAY:-5}" \
    VALTZ_SNAPSHOT_SIZE="${VALTZ_SNAPSHOT_SIZE:-1440x900}" \
    caffeinate -d -i timeout "$limit" "$bin" >"$out/stdout.log" 2>"$out/stderr.log"
  rc=$?
  local secs=$(($(now) - t0))
  grep '^snapshot:' "$out/stderr.log" "$out/stdout.log" -h >"$out/snapshot.log"
  local err mem n
  err="$(state_of "$out/snapshot.log" lastError)"
  mem="$(state_of "$out/snapshot.log" memoryFailure)"
  n="$(state_of "$out/snapshot.log" assets)"
  if [ $rc -eq 124 ]; then
    record app "$name" fail "$secs" "timed out after ${limit}s"
  elif [ $rc -ne 0 ]; then
    record app "$name" fail "$secs" "exit $rc (stderr.log)"
  elif [ -z "$n" ]; then
    record app "$name" fail "$secs" "no final state line"
  elif [ -n "$err" ] && [ "$err" != - ]; then
    record app "$name" fail "$secs" "error: $(echo "$err" | cut -c1-140)"
  elif [ -n "$mem" ] && [ "$mem" != - ] && [ "$mem" != false ]; then
    record app "$name" fail "$secs" "memory failure: $mem"
  elif [ "$n" -lt "$want" ]; then
    record app "$name" fail "$secs" "assets=$n, wanted >= $want"
  else
    record app "$name" pass "$secs" "assets=$n"
  fi
}

echo "app scenarios ($tier) -> $work"
pic="a red fox sitting in fresh snow, morning light"

scenario image krea2-turbo quick 600 1 -- \
  VALTZ_SNAPSHOT_TASKS="modality:image;model:krea2-turbo;size:512x512;prompt:$pic;start;memory:$work/image/memory.png;activity:$work/image/activity.png;idle:300;view:newest-image;shot:$work/image/result.png"

scenario video minimax-h3-fl2va quick 1500 1 -- \
  VALTZ_SNAPSHOT_FAVOR=speed \
  VALTZ_SNAPSHOT_TASKS="modality:video;model:minimax-h3-fl2va;length:2;prompt:waves rolling onto a beach at sunset, gulls calling;start;watch:1;memory:$work/video/memory.png;idle:1200;shot:$work/video/result.png;export:h264:$work/video/clip.mp4"

if [ -d "$work/video" ]; then
  if [ -s "$work/video/clip.mp4" ] && "$VALTZCTL" probe "$work/video/clip.mp4" 2>/dev/null | grep -q '"type": "video"'; then
    record app video-export pass 0
  else
    record app video-export fail 0 "no exported clip at $work/video/clip.mp4"
  fi
fi

scenario cancel-undo krea2-turbo full 600 1 -- \
  VALTZ_SNAPSHOT_TASKS="modality:image;model:krea2-turbo;size:1024x1024;prompt:a lighthouse at dusk;start;wait:1;cancel:1;idle:60;prompt:a kite over a field;start;idle:300;undo;wait:2;shot:$work/cancel-undo/after-undo.png"

scenario favor-pika minimax-h3-fl2va,minimax-h3-pika-flex full 1500 1 -- \
  VALTZ_SNAPSHOT_FAVOR=custom VALTZ_SNAPSHOT_TUNE="pika_flex=true" \
  VALTZ_SNAPSHOT_TASKS="modality:video;model:minimax-h3-fl2va;length:2;prompt:a hummingbird hovering at a red flower;start;idle:1200;shot:$work/favor-pika/result.png"

scenario edit qwen-image-2.1-turbo,krea2-turbo full 900 2 -- \
  VALTZ_SNAPSHOT_TASKS="modality:image;model:krea2-turbo;size:640x480;prompt:an empty blue lake at noon;start;idle:300;view:newest-image;model:qwen-image-2.1-turbo;prompt:paint a small white sailboat in the middle;start;idle:600;shot:$work/edit/result.png"

scenario agent qwen3.5-9b,krea2-turbo full 1500 1 -- \
  VALTZ_SNAPSHOT_AGENT="ask:Generate one picture of a green apple on a white plate.;wait:review@300;all;wait:done@1200;shot:$work/agent/done.png"

scenario capabilities - quick 120 0 -- \
  VALTZ_SNAPSHOT_SETTINGS=capabilities

scenario download-taef1 - full 300 0 -- \
  VALTZ_SNAPSHOT_DOWNLOAD=taef1 \
  VALTZ_SNAPSHOT_TASKS="wait:60"

echo "app scenarios done"
