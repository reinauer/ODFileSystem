#!/bin/sh
# Reject writable allocated sections in m68k release handlers.
# SPDX-License-Identifier: BSD-2-Clause

set -eu
export LC_ALL=C

if [ "$#" -eq 0 ]; then
    echo "Usage: $0 BINARY [BINARY ...]" >&2
    exit 1
fi

objdump=${OBJDUMP:-m68k-amigaos-objdump}
for binary do
    sections=$("$objdump" -h "$binary")
    printf '%s\n' "$sections" | awk -v binary="$binary" '
        /file format/ { format = $NF }
        /^[[:space:]]*[0-9]+[[:space:]]/ {
            name = $2
            size = $3
            getline
            if (size !~ /^0+$/ && /ALLOC/) {
                # Amiga HUNK_CODE has CODE but no READONLY flag.
                if (/CODE/)
                    code = 1
                else if (!/READONLY/) {
                    printf "%s: writable section %s (0x%s bytes)\n", \
                           binary, name, size
                    bad = 1
                }
            }
        }
        END {
            if (format != "amiga" || !code) {
                printf "%s: expected an Amiga binary with nonempty code\n", \
                       binary
                bad = 1
            }
            exit bad
        }
    ' >&2
    echo "$binary: ROM section check passed"
done
