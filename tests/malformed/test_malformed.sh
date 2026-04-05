#!/bin/sh
# Malformed-image smoke tests
#
# SPDX-License-Identifier: BSD-2-Clause

set -eu

TOOLS="${TOOLS:-./build/host/tools}"
CORPUS_DIR="tests/malformed/corpus"
IMAGES_DIR="tests/images"
TMPDIR_ROOT="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "$TMPDIR_ROOT/odfs-malformed.XXXXXX")"
PASS=0
FAIL=0

cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

run_tool() {
    label="$1"
    shift

    if command -v timeout >/dev/null 2>&1; then
        if timeout 5 "$@" >/dev/null 2>&1; then
            rc=0
        else
            rc=$?
        fi
    else
        if "$@" >/dev/null 2>&1; then
            rc=0
        else
            rc=$?
        fi
    fi

    if [ "$rc" -eq 0 ] || [ "$rc" -eq 1 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '  FAIL: %s (exit %s)\n' "$label" "$rc"
    fi
}

gen_truncated() {
    src="$1"
    dst="$2"
    size="$3"
    dd if="$src" of="$dst" bs=1 count="$size" status=none
}

gen_corrupt_byte_range() {
    src="$1"
    dst="$2"
    offset="$3"
    bytes="$4"
    cp "$src" "$dst"
    printf '%s' "$bytes" | dd of="$dst" bs=1 seek="$offset" conv=notrunc status=none
}

echo "=== Malformed image tests ==="

for tool in imginfo imgls imgcat imgstat; do
    if [ ! -x "$TOOLS/$tool" ]; then
        echo "missing tool: $TOOLS/$tool"
        exit 1
    fi
done

gen_truncated "$IMAGES_DIR/test_plain.iso" "$WORKDIR/truncated-before-pvd.iso" 2048
gen_truncated "$IMAGES_DIR/test_plain.iso" "$WORKDIR/truncated-after-pvd.iso" 36864
gen_truncated "$IMAGES_DIR/test_udf_only.img" "$WORKDIR/truncated-udf.img" 131072
gen_corrupt_byte_range "$IMAGES_DIR/test_plain.iso" "$WORKDIR/bad-pvd-signature.iso" 32769 "XXXXX"
gen_corrupt_byte_range "$IMAGES_DIR/test_joliet.iso" "$WORKDIR/bad-svd-signature.iso" 34817 "XXXXX"
: > "$WORKDIR/empty.img"

for image in "$CORPUS_DIR"/* "$WORKDIR"/*; do
    base="$(basename "$image")"
    run_tool "imginfo $base" "$TOOLS/imginfo" "$image"
    run_tool "imgls $base" "$TOOLS/imgls" "$image"
    run_tool "imgcat $base" "$TOOLS/imgcat" "$image" /SHORT.TXT
    run_tool "imgstat $base" "$TOOLS/imgstat" "$image" /SHORT.TXT
done

echo ""
echo "$PASS passed, $FAIL failed, $((PASS + FAIL)) total"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "=== Malformed tests PASSED ==="
