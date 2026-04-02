#!/bin/sh
# Golden test: AmigaOS 3.2 CD ISO image
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Validates ODFileSystem tools against the known AmigaOS 3.2 CD-ROM image.
# Set AMIGAOS32_ISO to the path of AmigaOS3.2CD.iso before running.

set -e

TOOLS="${TOOLS:-./build/host/tools}"
ISO="${AMIGAOS32_ISO:-/Users/stepan/AmigaOS/AmigaOS-3.2-full/AmigaOS3.2CD.iso}"

if [ ! -f "$ISO" ]; then
    echo "SKIP: AmigaOS 3.2 ISO not found at $ISO"
    echo "Set AMIGAOS32_ISO to the path"
    exit 0
fi

PASS=0
FAIL=0

assert_eq() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        PASS=$((PASS + 1))
        printf "  PASS: %s\n" "$desc"
    else
        FAIL=$((FAIL + 1))
        printf "  FAIL: %s\n    expected: %s\n    actual:   %s\n" "$desc" "$expected" "$actual"
    fi
}

assert_contains() {
    local desc="$1" needle="$2" haystack="$3"
    if echo "$haystack" | grep -qF "$needle"; then
        PASS=$((PASS + 1))
        printf "  PASS: %s\n" "$desc"
    else
        FAIL=$((FAIL + 1))
        printf "  FAIL: %s (missing: %s)\n" "$desc" "$needle"
    fi
}

echo "=== Golden test: AmigaOS 3.2 CD ==="

# ---- imginfo ----
INFO=$("$TOOLS/imginfo" "$ISO" 2>/dev/null)
assert_contains "imginfo: backend" "iso9660" "$INFO"
assert_contains "imginfo: volume" "AmigaOS3.2CD" "$INFO"
assert_contains "imginfo: sector count" "38037" "$INFO"
assert_contains "imginfo: sector size" "2048" "$INFO"

# ---- root directory listing ----
ROOT=$("$TOOLS/imgls" "$ISO" 2>/dev/null)
assert_contains "root: ADF dir" "ADF" "$ROOT"
assert_contains "root: C dir" "C" "$ROOT"
assert_contains "root: Install dir" "Install" "$ROOT"
assert_contains "root: Libs dir" "Libs" "$ROOT"
assert_contains "root: S dir" "S" "$ROOT"
assert_contains "root: System dir" "System" "$ROOT"
assert_contains "root: CDVersion file" "CDVersion" "$ROOT"
assert_contains "root: Disk.info file" "Disk.info" "$ROOT"

# count root entries
ROOT_COUNT=$(echo "$ROOT" | wc -l | tr -d ' ')
assert_eq "root: entry count" "27" "$ROOT_COUNT"

# ---- CDVersion file content ----
CDVER=$("$TOOLS/imgcat" "$ISO" /CDVersion 2>/dev/null)
assert_eq "CDVersion content" '$VER: AmigaOS 3.2 CD-ROM Release 47.1' "$CDVER"

# ---- /C directory ----
CDIR=$("$TOOLS/imgls" "$ISO" /C 2>/dev/null)
assert_contains "C: Copy command" "Copy" "$CDIR"
assert_contains "C: Dir command" "Dir" "$CDIR"
assert_contains "C: List command" "List" "$CDIR"
assert_contains "C: SetPatch command" "SetPatch" "$CDIR"
assert_contains "C: Version command" "Version" "$CDIR"

# count C entries
C_COUNT=$(echo "$CDIR" | wc -l | tr -d ' ')
assert_eq "C: entry count" "54" "$C_COUNT"

# ---- /Libs directory ----
LIBS=$("$TOOLS/imgls" "$ISO" /Libs 2>/dev/null)
assert_contains "Libs: asl.library" "asl.library" "$LIBS"
assert_contains "Libs: icon.library" "icon.library" "$LIBS"
assert_contains "Libs: workbench.library" "workbench.library" "$LIBS"

# ---- /Install directory ----
INST=$("$TOOLS/imgls" "$ISO" /Install 2>/dev/null)
assert_contains "Install: ReadMe.txt" "ReadMe.txt" "$INST"

# ---- deep path resolution ----
STAT=$("$TOOLS/imgstat" "$ISO" /C/Copy 2>/dev/null)
assert_contains "stat: C/Copy is file" "file" "$STAT"
assert_contains "stat: C/Copy size" "5812" "$STAT"

# ---- file size checks ----
assert_contains "C/DiskDoctor size" "172280" "$CDIR"
assert_contains "C/SetPatch size" "22640" "$CDIR"

# ---- imgbench cache stats ----
BENCH=$("$TOOLS/imgbench" "$ISO" 2>/dev/null)
assert_contains "bench: has reads" "reads:" "$BENCH"
assert_contains "bench: has hits" "hits:" "$BENCH"

# ---- summary ----
echo ""
echo "$PASS passed, $FAIL failed, $((PASS + FAIL)) total"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "=== Golden test PASSED ==="
