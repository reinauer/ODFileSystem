#!/bin/sh
# Golden test: Rock Ridge, Joliet, and format precedence
#
# SPDX-License-Identifier: BSD-2-Clause

set -e

TOOLS="${TOOLS:-./build/host/tools}"
IMAGES="tests/images"

# check test images exist
for img in test_plain.iso test_rr.iso test_joliet.iso test_rr_joliet.iso; do
    if [ ! -f "$IMAGES/$img" ]; then
        echo "SKIP: test image $IMAGES/$img not found"
        echo "Run: make test-images"
        exit 0
    fi
done

PASS=0
FAIL=0

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf "  FAIL: %s\n    expected: %s\n    actual:   %s\n" "$desc" "$expected" "$actual"
    fi
}

assert_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf "  FAIL: %s (missing: %s)\n" "$desc" "$needle"
    fi
}

assert_not_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if ! echo "$haystack" | grep -qF "$needle"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf "  FAIL: %s (unexpected: %s)\n" "$desc" "$needle"
    fi
}

echo "=== Format tests ==="

# ---- Plain ISO9660 ----
INFO=$("$TOOLS/imginfo" "$IMAGES/test_plain.iso" 2>/dev/null)
ROOT=$("$TOOLS/imgls" "$IMAGES/test_plain.iso" 2>/dev/null)
assert_contains "plain: backend" "iso9660" "$INFO"
assert_contains "plain: volume" "PLAIN_TEST" "$INFO"
assert_contains "plain: truncated name" "LONG_FIL.TXT" "$ROOT"
assert_contains "plain: uppercase" "SHORT.TXT" "$ROOT"
assert_not_contains "plain: no symlink" "symlink" "$ROOT"

# ---- Rock Ridge ----
INFO=$("$TOOLS/imginfo" "$IMAGES/test_rr.iso" 2>/dev/null)
ROOT=$("$TOOLS/imgls" "$IMAGES/test_rr.iso" 2>/dev/null)
assert_contains "rr: backend" "rock_ridge" "$INFO"
assert_contains "rr: long name" "long filename with spaces.txt" "$ROOT"
assert_contains "rr: accented" "txt" "$ROOT"
assert_contains "rr: mixed case" "MixedCase.Txt" "$ROOT"
assert_contains "rr: symlink" "symlink" "$ROOT"
assert_contains "rr: deep dir" "deep" "$ROOT"

DEEP=$("$TOOLS/imgls" "$IMAGES/test_rr.iso" /deep/subdir 2>/dev/null)
assert_contains "rr: nested file" "nested.txt" "$DEEP"

CONTENT=$("$TOOLS/imgcat" "$IMAGES/test_rr.iso" /SHORT.TXT 2>/dev/null)
assert_eq "rr: file content" "Short" "$CONTENT"

# ---- Joliet ----
INFO=$("$TOOLS/imginfo" "$IMAGES/test_joliet.iso" 2>/dev/null)
ROOT=$("$TOOLS/imgls" "$IMAGES/test_joliet.iso" 2>/dev/null)
assert_contains "joliet: backend" "joliet" "$INFO"
assert_contains "joliet: volume" "JOLIET_TEST" "$INFO"
assert_contains "joliet: long name" "long filename with spaces.txt" "$ROOT"
assert_contains "joliet: mixed case" "MixedCase.Txt" "$ROOT"
assert_not_contains "joliet: no truncated" "LONG_FIL" "$ROOT"

DEEP=$("$TOOLS/imgls" "$IMAGES/test_joliet.iso" /deep/subdir 2>/dev/null)
assert_contains "joliet: nested file" "nested.txt" "$DEEP"

CONTENT=$("$TOOLS/imgcat" "$IMAGES/test_joliet.iso" /SHORT.TXT 2>/dev/null)
assert_eq "joliet: file content" "Short" "$CONTENT"

# ---- RR + Joliet hybrid (RR should win) ----
INFO=$("$TOOLS/imginfo" "$IMAGES/test_rr_joliet.iso" 2>/dev/null)
ROOT=$("$TOOLS/imgls" "$IMAGES/test_rr_joliet.iso" 2>/dev/null)
assert_contains "hybrid: backend is RR" "rock_ridge" "$INFO"
assert_contains "hybrid: RR long name" "long filename with spaces.txt" "$ROOT"
assert_contains "hybrid: RR symlink" "symlink" "$ROOT"

# ---- Multisession ----
INFO=$("$TOOLS/imginfo" "$IMAGES/test_multisession.iso" 2>/dev/null)
ROOT=$("$TOOLS/imgls" "$IMAGES/test_multisession.iso" 2>/dev/null)
assert_contains "multisession: volume is SESSION2" "SESSION2" "$INFO"
assert_contains "multisession: session 2 file" "file2.txt" "$ROOT"
assert_contains "multisession: session 2 updated" "updated.txt" "$ROOT"
assert_contains "multisession: session 1 merged" "FILE1.TXT" "$ROOT"

CONTENT=$("$TOOLS/imgcat" "$IMAGES/test_multisession.iso" /updated.txt 2>/dev/null)
assert_eq "multisession: read session 2 file" "Updated" "$CONTENT"

# ---- summary ----
echo ""
echo "$PASS passed, $FAIL failed, $((PASS + FAIL)) total"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "=== Format tests PASSED ==="
