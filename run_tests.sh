#!/usr/bin/env bash
# Fast build + test loop for the mcap extension.
#
#   ./run_tests.sh            # incremental build, then run all test/sql/*.test
#   ./run_tests.sh mcap_all   # build, then run only test/sql/mcap_all.test
#   ./run_tests.sh --no-build  # skip the build, just run tests
#
# Regenerate the fixture first (once, inside the venv):
#   python3 -m venv .venv && . .venv/bin/activate && pip install mcap
#   python test/generate_sample_mcap.py
set -euo pipefail

cd "$(dirname "$0")"

UNITTEST=build/release/test/unittest
FIXTURE=test/mcap/sample.mcap

# --- ensure the fixtures exist ---
if [[ ! -f "$FIXTURE" || ! -f test/mcap/protobuf.mcap ]]; then
    echo ">> fixtures missing; generating..."
    if [[ -f .venv/bin/activate ]]; then
        # shellcheck disable=SC1091
        source .venv/bin/activate
    fi
    python test/generate_sample_mcap.py
    python test/generate_protobuf_mcap.py
fi

# --- build (unless --no-build) ---
do_build=1
args=()
for a in "$@"; do
    if [[ "$a" == "--no-build" ]]; then do_build=0; else args+=("$a"); fi
done

if [[ "$do_build" == 1 ]]; then
    echo ">> building (incremental)..."
    make >/tmp/mcap_build.log 2>&1 || { tail -30 /tmp/mcap_build.log; exit 1; }
    echo ">> build OK"
fi

# --- pick tests ---
if [[ ${#args[@]} -gt 0 ]]; then
    tests=()
    for name in "${args[@]}"; do tests+=("test/sql/${name#test/sql/}"); done
    # normalize: ensure .test suffix
    norm=()
    for t in "${tests[@]}"; do [[ "$t" == *.test ]] && norm+=("$t") || norm+=("$t.test"); done
    tests=("${norm[@]}")
else
    tests=(test/sql/*.test)
fi

echo ">> running: ${tests[*]}"
pass=0; fail=0; failed_names=()
for t in "${tests[@]}"; do
    if "$UNITTEST" "$t" >/tmp/mcap_test_out.log 2>&1; then
        printf '  PASS  %s\n' "$t"; ((pass++)) || true
    else
        printf '  FAIL  %s\n' "$t"; ((fail++)) || true
        failed_names+=("$t")
    fi
done

echo "-----------------------------------------"
echo ">> $pass passed, $fail failed"
if [[ "$fail" -gt 0 ]]; then
    echo ">> failures: ${failed_names[*]}"
    echo ">> last failure output (/tmp/mcap_test_out.log):"
    tail -40 /tmp/mcap_test_out.log
    exit 1
fi
