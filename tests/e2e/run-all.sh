#!/bin/bash
# The whole e2e suite, one layer after the other (they share the GPU):
# the C++ tests (valtz_test, VALTZ_TEST_E2E), the valtzctl smoke, the
# app's scenarios; then report.md beside their logs in
# ~/dump/valtz-e2e/<time>/. Usage:
#   run-all.sh [--tier quick|full] [--only cpp|ctl|app]...
# Environment: VALTZ_BUILD (default ~/dump/valtz-build-e2e),
# VALTZ_TEST_E2E_COMPRESS=1 (also compress Krea 2 beside itself, ~16 GB).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
tier=full
layers=()
while [ $# -gt 0 ]; do
  case "$1" in
    --tier) tier="$2"; shift 2 ;;
    --only) layers+=("$2"); shift 2 ;;
    *) echo "usage: $0 [--tier quick|full] [--only cpp|ctl|app]..." >&2; exit 2 ;;
  esac
done
[ ${#layers[@]} -eq 0 ] && layers=(cpp ctl app)
export E2E_OUT="${E2E_OUT:-$HOME/dump/valtz-e2e/$(date +%Y%m%d-%H%M%S)}"
. "$here/lib.sh"
export VALTZ_BUILD

# What the run needs before it starts.
for f in "$VALTZCTL" "$VALTZ_TEST" "$VALTZ_APP/Contents/MacOS/Valtz"; do
  [ -x "$f" ] || { echo "not built: $f (cmake --build $VALTZ_BUILD)" >&2; exit 2; }
done
if "$VALTZCTL" caps 2>&1 | grep -q "engine: none"; then
  echo "the build has no engine (configure with -DVALTZ_VPIPE_PREFIX=...)" >&2
  exit 2
fi
free_gb=$(df -g "$HOME" | awk 'NR == 2 { print $4 }')
echo "e2e $tier -> $E2E_OUT (${free_gb} GB free)"
installed_models >"$E2E_OUT/installed.txt"
echo "installed: $(tr '\n' ' ' <"$E2E_OUT/installed.txt")"

for layer in "${layers[@]}"; do
  case "$layer" in
    cpp)
      echo "== C++ (valtz_test --filter e2e)"
      gate=1
      [ "$tier" = quick ] && gate=quick
      t0=$(now)
      VALTZ_TEST_E2E=$gate "$VALTZ_TEST" --filter e2e >"$E2E_OUT/cpp.log" 2>&1
      # One result a test: its [ OK ] / [ FAIL ] / [ SKIP ] line.
      grep -E '^\[ *(OK|FAIL|SKIP) *\] e2e\.' "$E2E_OUT/cpp.log" |
        while read -r line; do
          status=$(echo "$line" | sed -E 's/^\[ *([A-Z]+) *\].*/\1/')
          name=$(echo "$line" | sed -E 's/^\[[^]]*\] e2e\.([^ ]+).*/\1/')
          why=$(echo "$line" | sed -nE 's/^[^(]*\((.*)\)$/\1/p')
          case "$status" in
            OK) record cpp "$name" pass - ;;
            SKIP) record cpp "$name" skip - "$why" ;;
            *) record cpp "$name" fail - "$(grep -E "FAILED .*e2e-test.cc" "$E2E_OUT/cpp.log" |
                 sed -E 's|.*e2e-test.cc:||' | tr '\n' ';' | cut -c1-200)" ;;
          esac
        done
      echo "  C++ took $(($(now) - t0)) s"
      ;;
    ctl) echo "== valtzctl"; "$here/valtzctl-smoke.sh" "$tier" ;;
    app) echo "== app"; "$here/app-run.sh" "$tier" ;;
  esac
done

# The report: a table a layer, the counts, where the files are.
{
  echo "# Valtz e2e — $tier — $(date '+%Y-%m-%d %H:%M')"
  echo
  echo "Build: \`$VALTZ_BUILD\` · models: $(wc -l <"$E2E_OUT/installed.txt" | tr -d ' ') installed · ${free_gb} GB free"
  echo
  for s in pass fail skip; do
    printf -- "- %s: %s\n" "$s" "$(awk -F'\t' -v s=$s '$3 == s' "$RESULTS" | wc -l | tr -d ' ')"
  done
  for layer in cpp ctl app; do
    awk -F'\t' -v l=$layer '$1 == l' "$RESULTS" | grep -q . || continue
    echo
    echo "## $layer"
    echo
    echo "| check | result | s | note |"
    echo "|---|---|---|---|"
    awk -F'\t' -v l=$layer '$1 == l { printf "| %s | %s | %s | %s |\n", $2, $3, $4, $5 }' "$RESULTS"
  done
  echo
  echo "## Not covered"
  echo
  echo "- Audio-only features (songs, speech/voice-over, transcribe, separate): their models are out of scope (disk)."
  echo "- The Balanced preset's 8-step Turbo LoRA (minimax-h3-turbo-8step): not installed by choice."
  echo "- minimax-h3-image-vae: no Valtz request decodes a one-frame H3 latent."
  echo "- Inpaint, ControlNet, face restore, frame interpolation: Valtz has no model kind for them."
} >"$E2E_OUT/report.md"
echo
echo "report: $E2E_OUT/report.md"
awk -F'\t' '$3 == "fail" { bad = 1 } END { exit bad }' "$RESULTS"
