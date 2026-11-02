#!/bin/bash
# The headless CLI end to end: valtzctl over the installed models, each
# command's exit code, what it made (the project's atlas) and the files
# it wrote (valtzctl probe). Usage: valtzctl-smoke.sh [quick|full]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"

tier="${1:-full}"
work="$E2E_OUT/ctl"
mkdir -p "$work"
proj="$work/Smoke.valtz"
log="$work/valtzctl.log"

# step <name> <needs: ids, comma-separated or -> <tier: quick|full> -- <valtzctl args...>
# Passes on exit 0; the step's output goes to the log and $work/<name>.out.
step() {
  local name="$1" needs="$2" when="$3"
  shift 4
  if [ "$when" = full ] && [ "$tier" = quick ]; then
    return 0
  fi
  if [ "$needs" != - ]; then
    local miss
    miss="$(missing_of ${needs//,/ })"
    if [ -n "$miss" ]; then
      record ctl "$name" skip 0 "needs $miss"
      return 0
    fi
  fi
  local t0 rc
  t0="$(now)"
  echo "== $name: valtzctl $*" >>"$log"
  "$VALTZCTL" "$@" >"$work/$name.out" 2>&1
  rc=$?
  cat "$work/$name.out" >>"$log"
  if [ $rc -eq 0 ]; then
    record ctl "$name" pass $(($(now) - t0))
  else
    record ctl "$name" fail $(($(now) - t0)) "exit $rc: $(grep -iE 'error|refused|failed' "$work/$name.out" | tail -1 | cut -c1-160)"
  fi
  return $rc
}

# The number of assets in the project (its atlas).
assets() {
  "$VALTZCTL" atlas "$proj" --json 2>/dev/null |
    jq '[.. | objects | select(has("asset") or has("id"))] | length' 2>/dev/null || echo 0
}

# check <name> <condition text> <test command...>
check() {
  local name="$1" what="$2"
  shift 2
  if "$@"; then
    record ctl "$name" pass 0
  else
    record ctl "$name" fail 0 "$what"
  fi
}

# The asset a step's job finished with (its job.finished line).
made_by() {  # step name
  grep '"kind":"job.finished"' "$work/$1.out" 2>/dev/null | tail -1 |
    jq -r '.asset // empty' 2>/dev/null
}

probe_has() {  # file pattern
  [ -f "$1" ] && "$VALTZCTL" probe "$1" 2>/dev/null | grep -qiE "$2"
}

echo "valtzctl smoke ($tier) -> $work"
rm -rf "$proj"

step hw - quick -- hw
step caps - quick -- caps
step models - quick -- models
step families - full -- families
step auto - full -- auto
step storage - full -- storage
step paths - full -- paths
step tuning-video minimax-h3-fl2va full -- tuning minimax-h3-fl2va --speed
step tuning-custom minimax-h3-fl2va,minimax-h3-pika-flex full -- \
  tuning minimax-h3-fl2va --tune pika_flex=true
step new - quick -- new "$proj" Smoke

# Pictures.
step generate krea2-turbo quick -- generate "$proj" "a red fox in fresh snow" \
  --model krea2-turbo --size 512x512 --seed 7 --speed
step generate-repeat krea2-turbo full -- generate "$proj" "a teapot, studio light" \
  --model krea2-turbo --size 512x512 --seed 1 --repeat 2

cp "$here/fixtures/base.png" "$work/base.png"
if [ -f "$work/base.png" ]; then
  step import - full -- import "$proj" --copy "$work/base.png"
  step edit qwen-image-2.1-turbo full -- generate "$proj" "paint a small white sailboat in the middle" \
    --model qwen-image-2.1-turbo --base "$work/base.png" --mode edit --size 512x384 --seed 3
fi

# Clips.
step video minimax-h3-fl2va quick -- video "$proj" "waves rolling onto a beach, gulls" \
  --size 832x480 --frames 39 --speed --seed 5
if [ -f "$work/base.png" ]; then
  step video-first minimax-h3-fl2va full -- video "$proj" "the scene comes alive" \
    --size 832x480 --frames 39 --speed --seed 6 --first "$work/base.png"
  step video-first-last minimax-h3-fl2va full -- video "$proj" "the scene comes alive" \
    --size 832x480 --frames 39 --speed --seed 6 --first "$work/base.png" --last "$work/base.png"
fi
step video-pika minimax-h3-fl2va,minimax-h3-pika-flex full -- video "$proj" "a hummingbird at a flower" \
  --size 832x480 --frames 39 --seed 21 --tune pika_flex=true
step video-taomate minimax-h3-fl2va,minimax-h3-turbo-3step full -- video "$proj" "a hummingbird at a flower" \
  --size 832x480 --frames 39 --seed 21 --tune taomate=true
step video-2k minimax-h3-fl2va,open-h3-regenerate-2k full -- video "$proj" "city lights at night" \
  --size 1920x1088 --frames 22 --speed --seed 13 --regenerate

# The helper.
step enhance qwen3.5-9b quick -- enhance "a cat on a windowsill"
step enhance-video qwen3.5-9b full -- enhance "a rocket launch" --video --frames 39
step intent qwen3.5-9b full -- intent "make a five second video of a rocket launching"
step agent qwen3.5-9b,krea2-turbo full -- agent "$proj" "Generate one picture of a green apple on a white plate." --auto

clip="$(made_by video)"
if [ -n "$clip" ] && [ "$tier" = full ]; then
  step summarize qwen3.5-9b,minimax-h3-fl2va full -- summarize "$proj" "$clip"
  step export-h264 minimax-h3-fl2va full -- export "$proj" "$clip" --format h264 -o "$work/clip.mp4"
  check export-h264-audio "the exported clip has sound" probe_has "$work/clip.mp4" "audio"
fi

# The project: what it holds, export, undo/redo.
step info - quick -- info "$proj"
step atlas - full -- atlas "$proj" --json
first="$(made_by generate)"
if [ -n "$first" ]; then
  step export-png - quick -- export "$proj" "$first" --format png -o "$work/first.png"
  check export-png-file "the export is a picture" probe_has "$work/first.png" "png|image"
fi
before="$(assets)"
step undo - full -- undo "$proj"
step redo - full -- redo "$proj"
[ "$tier" = full ] &&
  check undo-redo-roundtrip "undo then redo leaves the asset count" test "$(assets)" = "$before"

echo "valtzctl smoke done: $(grep -c $'\tpass\t' "$RESULTS") pass so far"
