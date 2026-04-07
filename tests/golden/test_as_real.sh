#!/bin/sh
# Golden test: real-world Rock Ridge AS image
#
# SPDX-License-Identifier: BSD-2-Clause

set -eu

TOOLS="${TOOLS:-./build/host/tools}"
FETCH="${FETCH:-tests/golden/fetch_real_as_fixture.sh}"

PASS=0
FAIL=0

assert_contains() {
    desc="$1"
    needle="$2"
    haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '  FAIL: %s (missing: %s)\n' "$desc" "$needle"
    fi
}

FIXTURE=$(sh "$FETCH") || rc=$?
rc=${rc:-0}
if [ "$rc" -eq 2 ]; then
    echo "SKIP: real AS fixture unavailable"
    exit 0
elif [ "$rc" -ne 0 ]; then
    echo "FAIL: fixture fetch helper failed"
    exit 1
fi

echo "=== Real AS image test ==="

INFO=$("$TOOLS/imginfo" "$FIXTURE" 2>/dev/null)
ROOT=$("$TOOLS/imgls" --amiga "$FIXTURE" 2>/dev/null)
SUBDIR=$("$TOOLS/imgls" --amiga "$FIXTURE" /c 2>/dev/null)

assert_contains "real-as: backend" "backend: rock_ridge" "$INFO"
assert_contains "real-as: volume" "Arabian_Nights" "$INFO"
assert_contains "real-as: root file" "anim.dsk" "$ROOT"
assert_contains "real-as: root protection" "amiga=00 00 ff 10" "$ROOT"
assert_contains "real-as: nested file" "SetPatch" "$SUBDIR"
assert_contains "real-as: nested protection" "amiga=00 00 ff 10" "$SUBDIR"

echo ""
echo "$PASS passed, $FAIL failed, $((PASS + FAIL)) total"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "=== Real AS image test PASSED ==="
