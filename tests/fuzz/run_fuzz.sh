#!/bin/sh
# Parser fuzz smoke-test runner
#
# SPDX-License-Identifier: BSD-2-Clause

set -eu

FUZZ_BINS="${FUZZ_BINS:-./build/host/tests}"
PASS=0
FAIL=0

collect_corpus() {
    find tests/images tests/malformed/corpus -type f | sort
}

run_one() {
    bin="$1"
    image="$2"
    label="$(basename "$bin") $(basename "$image")"

    if command -v timeout >/dev/null 2>&1; then
        timeout 5 "$bin" "$image" >/dev/null 2>&1
    else
        "$bin" "$image" >/dev/null 2>&1
    fi
    rc=$?

    if [ "$rc" -eq 0 ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf '  FAIL: %s (exit %s)\n' "$label" "$rc"
    fi
}

echo "=== Parser fuzz smoke tests ==="

for bin in \
    "$FUZZ_BINS/fuzz_auto" \
    "$FUZZ_BINS/fuzz_iso9660" \
    "$FUZZ_BINS/fuzz_joliet" \
    "$FUZZ_BINS/fuzz_udf" \
    "$FUZZ_BINS/fuzz_hfs" \
    "$FUZZ_BINS/fuzz_hfsplus"
do
    if [ ! -x "$bin" ]; then
        echo "missing fuzz binary: $bin"
        exit 1
    fi
done

for image in $(collect_corpus); do
    for bin in \
        "$FUZZ_BINS/fuzz_auto" \
        "$FUZZ_BINS/fuzz_iso9660" \
        "$FUZZ_BINS/fuzz_joliet" \
        "$FUZZ_BINS/fuzz_udf" \
        "$FUZZ_BINS/fuzz_hfs" \
        "$FUZZ_BINS/fuzz_hfsplus"
    do
        run_one "$bin" "$image"
    done
done

echo ""
echo "$PASS passed, $FAIL failed, $((PASS + FAIL)) total"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "=== Parser fuzz smoke tests PASSED ==="
