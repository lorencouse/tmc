#!/usr/bin/env bash
# Run every tools/*_regression_test.py and tools/test_*.py suite (no ROM needed).
#
#   tools/run_regression_tests.sh            # all suites
#   tools/run_regression_tests.sh gfx config # only suites whose name contains a word
#
# region_regression_test.py links the production port_rom.c object, so it is
# skipped unless `xmake build -y tmc_pc` has produced exactly one. Compilers and
# header locations come from CC/CXX, TMC_SDL3_INCLUDE and TMC_JSON_INCLUDE; see
# tools/regression_build.py. Suites run one at a time and every failure is
# reported before the script exits nonzero.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PYTHON="${PYTHON:-python3}"
cd "$ROOT" || exit 1

failed=()
passed=0
skipped=0
for suite in tools/*_regression_test.py tools/test_*.py; do
    name="$(basename "$suite" .py)"
    if [ $# -gt 0 ]; then
        match=0
        for word in "$@"; do
            case "$name" in *"$word"*) match=1 ;; esac
        done
        [ "$match" = 1 ] || continue
    fi
    if [ "$name" = region_regression_test ]; then
        objects=$(find build/.objs/tmc_pc -path '*/port/port_rom.c.o' 2>/dev/null | wc -l | tr -d " ")
        if [ "$objects" -ne 1 ]; then
            echo "=== $name: SKIP (needs exactly one built tmc_pc port_rom.c.o, found $objects)"
            skipped=$((skipped + 1))
            continue
        fi
    fi
    echo "=== $name"
    if "$PYTHON" "$suite"; then
        passed=$((passed + 1))
    else
        echo "=== $name: FAIL"
        failed+=("$name")
    fi
done

echo "=== regression suites: $passed passed, ${#failed[@]} failed, $skipped skipped"
if [ ${#failed[@]} -gt 0 ]; then
    printf '  failed: %s\n' "${failed[@]}"
    exit 1
fi
