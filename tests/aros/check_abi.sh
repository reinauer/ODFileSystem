#!/bin/sh
# Run inside the x86_64 AROS toolchain's Linux container.
# SPDX-License-Identifier: BSD-2-Clause
set -eu

AROS_CC=${AROS_CC:-x86_64-aros-gcc}
LINUX_CC=${LINUX_CC:-gcc-11}
AROS_SYSROOT=${AROS_SYSROOT:-/opt/x86_64-aros}
AROS_TEST_BUILD=${AROS_TEST_BUILD:-build/aros-abi}
mkdir -p "$AROS_TEST_BUILD"

# Compile the production helpers against AROS headers, then link only the
# tested sections into a Linux executable. Both use the x86_64 SysV C ABI.
# No Exec or DOS calls survive garbage collection in this isolated harness.
"$AROS_CC" --sysroot="$AROS_SYSROOT" -DAMIGA -DODFS_SERIAL_DEBUG=0 \
    -DODFS_FEATURE_LOG=0 -DODFS_AMIGA_DATE='"test"' -O2 \
    -ffunction-sections -fdata-sections \
    -Iinclude -Ibackends -Iplatform/amiga -Iplatform/amiga/common \
    -Iplatform/amiga/aros -c tests/aros/test_abi.c \
    -o "$AROS_TEST_BUILD/test_abi.o"
"$LINUX_CC" -B/usr/bin/x86_64-linux-gnu- -no-pie -Wl,--gc-sections \
    "$AROS_TEST_BUILD/test_abi.o" \
    -o "$AROS_TEST_BUILD/test_abi"
"$AROS_TEST_BUILD/test_abi"
echo 'AROS x86_64 packet ABI checks passed (isolated Linux harness)'
