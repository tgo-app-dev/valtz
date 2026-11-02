# Shared by the e2e drivers (sourced): where things are, and a results
# file -- one line a check, "<layer>\t<name>\t<pass|fail|skip>\t<seconds>\t<note>"
# -- that run-all.sh turns into report.md.

: "${VALTZ_BUILD:=$HOME/dump/valtz-build-e2e}"
: "${E2E_OUT:=$HOME/dump/valtz-e2e/$(date +%Y%m%d-%H%M%S)}"
VALTZCTL="$VALTZ_BUILD/tools/valtzctl/valtzctl"
VALTZ_APP="$VALTZ_BUILD/app/macos/Valtz.app"
VALTZ_TEST="$VALTZ_BUILD/tests/valtz_test"
RESULTS="$E2E_OUT/results.tsv"
mkdir -p "$E2E_OUT"
touch "$RESULTS"

record() {  # layer name status seconds note
  printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "${5:-}" >>"$RESULTS"
  case "$3" in
    pass) printf '  \033[32mPASS\033[0m %s/%s (%ss)\n' "$1" "$2" "$4" ;;
    skip) printf '  \033[33mSKIP\033[0m %s/%s -- %s\n' "$1" "$2" "${5:-}" ;;
    *)    printf '  \033[31mFAIL\033[0m %s/%s -- %s\n' "$1" "$2" "${5:-}" ;;
  esac
}

# The catalog ids installed here, one a line (valtzctl models).
installed_models() {
  "$VALTZCTL" models 2>/dev/null | awk '$2 == "installed" { print $1 }'
}

has_model() {
  grep -qx "$1" "$E2E_OUT/installed.txt" 2>/dev/null ||
    { installed_models >"$E2E_OUT/installed.txt"; grep -qx "$1" "$E2E_OUT/installed.txt"; }
}

# Each model id given installed, or the first missing one named.
missing_of() {
  for id in "$@"; do
    has_model "$id" || { echo "$id"; return; }
  done
}

now() { date +%s; }
