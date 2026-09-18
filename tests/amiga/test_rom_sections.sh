#!/bin/sh
# Exercise the section check with real m68k HUNK binaries.
# SPDX-License-Identifier: BSD-2-Clause

set -eu
repo_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
amiga_cc=${AMIGA_CC:-m68k-amigaos-gcc}
check="$repo_dir/tools/check_rom_sections.sh"

cat > "$test_dir/code.c" <<'EOF'
int entry(void) { return 1; }
EOF
cat > "$test_dir/data.c" <<'EOF'
volatile int value = 1;
int entry(void) { return ++value; }
EOF
cat > "$test_dir/bss.c" <<'EOF'
volatile int value;
int entry(void) { return ++value; }
EOF

for kind in code data bss; do
    "$amiga_cc" -m68000 -noixemul -nostdlib -Wl,-e,_entry \
        "$test_dir/$kind.c" -o "$test_dir/$kind"
done

sh "$check" "$test_dir/code"
for kind in data bss; do
    # A good first input must not hide a bad subsequent binary.
    if sh "$check" "$test_dir/code" "$test_dir/$kind" \
            > "$test_dir/result" 2>&1; then
        echo "ERROR: accepted writable $kind section" >&2
        exit 1
    fi
    grep -F "writable section .$kind" "$test_dir/result"
done

printf 'not a binary\n' > "$test_dir/invalid"
for kind in missing invalid; do
    if sh "$check" "$test_dir/$kind" > "$test_dir/result" 2>&1; then
        echo "ERROR: accepted $kind binary" >&2
        exit 1
    fi
done
echo "ROM section checker tests passed"
