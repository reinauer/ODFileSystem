#!/bin/sh
# AmiFUSE integration smoke test for ODFileSystem
#
# SPDX-License-Identifier: BSD-2-Clause

set -eu

AMIFUSE="${AMIFUSE:-amifuse}"
ODFS_HANDLER="${ODFS_HANDLER:-build/amiga/ODFileSystem}"
IMAGE="${AMIFUSE_IMAGE:-${AMIGAOS32_ISO:-}}"
PARTITION="${AMIFUSE_PARTITION:-}"
BLOCK_SIZE="${AMIFUSE_BLOCK_SIZE:-}"
TMPDIR_ROOT="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "$TMPDIR_ROOT/odfs-amifuse.XXXXXX")"
MOUNTPOINT="${AMIFUSE_MOUNTPOINT:-$WORKDIR/mnt}"
AMIFUSE_PID=""
CREATED_MOUNTPOINT=0
GENERATED_ISO=0

if [ -n "${AMIFUSE_EXPECT_FILE:-}" ]; then
    EXPECT_FILE="${AMIFUSE_EXPECT_FILE}"
elif [ -n "${AMIGAOS32_ISO:-}" ] && [ "${IMAGE}" = "${AMIGAOS32_ISO}" ]; then
    EXPECT_FILE="CDVersion"
else
    EXPECT_FILE="SHORT.TXT"
fi

if [ -n "${AMIFUSE_EXPECT_CONTENT:-}" ]; then
    EXPECT_CONTENT="${AMIFUSE_EXPECT_CONTENT}"
elif [ -n "${AMIGAOS32_ISO:-}" ] && [ "${IMAGE}" = "${AMIGAOS32_ISO}" ]; then
    EXPECT_CONTENT='$VER: AmigaOS 3.2 CD-ROM Release 47.1'
else
    EXPECT_CONTENT="Short"
fi

cleanup() {
    if [ -n "$AMIFUSE_PID" ]; then
        umount "$MOUNTPOINT" >/dev/null 2>&1 || true
        kill "$AMIFUSE_PID" >/dev/null 2>&1 || true
        wait "$AMIFUSE_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORKDIR" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

create_temp_iso() {
    local srcdir="$WORKDIR/iso-src"
    local outiso="$WORKDIR/odfs-test.iso"

    mkdir -p "$srcdir/deep/subdir"
    printf 'Short' > "$srcdir/SHORT.TXT"
    printf 'Deep file' > "$srcdir/deep/subdir/nested.txt"

    if command -v mkisofs >/dev/null 2>&1; then
        mkisofs -quiet -o "$outiso" "$srcdir" >/dev/null 2>&1
    elif command -v hdiutil >/dev/null 2>&1; then
        hdiutil makehybrid -quiet -iso -joliet -o "$outiso" "$srcdir" >/dev/null
    else
        echo "SKIP: no ISO creation tool found (need mkisofs or hdiutil)"
        exit 0
    fi

    # AmiFUSE's synthetic raw-image geometry behaves more reliably with
    # a larger optical image than with the tiny ISO emitted here.
    truncate -s 64m "$outiso"

    IMAGE="$outiso"
    GENERATED_ISO=1
}

if ! command -v "$AMIFUSE" >/dev/null 2>&1; then
    echo "SKIP: amifuse not found"
    exit 0
fi

if [ -z "$IMAGE" ]; then
    create_temp_iso
fi

if [ ! -f "$IMAGE" ]; then
    echo "SKIP: image not found: $IMAGE"
    exit 0
fi

if [ ! -f "$ODFS_HANDLER" ]; then
    echo "SKIP: handler not found: $ODFS_HANDLER"
    exit 0
fi

mkdir -p "$MOUNTPOINT"
CREATED_MOUNTPOINT=1

set -- "$AMIFUSE" mount "$IMAGE" --driver "$ODFS_HANDLER" --mountpoint "$MOUNTPOINT"

if [ -n "$PARTITION" ]; then
    set -- "$@" --partition "$PARTITION"
fi

if [ -n "$BLOCK_SIZE" ]; then
    set -- "$@" --block-size "$BLOCK_SIZE"
fi

"$@" >/tmp/odfs-amifuse.log 2>&1 &
AMIFUSE_PID=$!

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if [ -e "$MOUNTPOINT/$EXPECT_FILE" ]; then
        ready=1
        break
    fi
    if ! kill -0 "$AMIFUSE_PID" >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

if [ "$ready" -ne 1 ]; then
    echo "FAIL: mount did not expose $EXPECT_FILE"
    echo "--- amifuse log ---"
    sed -n '1,200p' /tmp/odfs-amifuse.log || true
    exit 1
fi

test -d "$MOUNTPOINT"
test -f "$MOUNTPOINT/$EXPECT_FILE"

CONTENT="$(cat "$MOUNTPOINT/$EXPECT_FILE")"
if [ "$CONTENT" != "$EXPECT_CONTENT" ]; then
    echo "FAIL: unexpected content in $EXPECT_FILE"
    echo "expected: $EXPECT_CONTENT"
    echo "actual:   $CONTENT"
    exit 1
fi

ls "$MOUNTPOINT" >/dev/null

python3 tests/integration/check_assign_prefix.py "$IMAGE" "$ODFS_HANDLER"
python3 tests/integration/check_fh_packets.py "$IMAGE" "$ODFS_HANDLER"

echo "AmiFUSE integration test passed"
