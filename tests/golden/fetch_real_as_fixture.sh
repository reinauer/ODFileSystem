#!/bin/sh
# Fetch and prepare the small real-world AS test fixture.
#
# Outputs the prepared data-track ISO path on stdout.
#
# Exit codes:
#   0 = fixture ready
#   2 = skipped (missing tools/network/extraction failed)

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)

ARCHIVE_ITEM="noaen-tosec-iso-commodore-amiga-cd32"
ARCHIVE_REL="Commodore/Amiga CD32/Games/[BIN]/Arabian Nights (1993)(Buzz)(M4).7z"
ARCHIVE_MD5="09d4723a76c3f380b3b6e375c6b9e1fe"
ARCHIVE_URL="https://archive.org/download/noaen-tosec-iso-commodore-amiga-cd32/Commodore/Amiga%20CD32/Games/%5BBIN%5D/Arabian%20Nights%20%281993%29%28Buzz%29%28M4%29.7z"
DATA_MD5="d8034c80bb2294d712511cced7401097"
PREPARED_IMAGE="${ODFS_REAL_AS_IMAGE:-}"

CACHE_DIR="${ODFS_REAL_AS_CACHE:-${TMPDIR:-/tmp}/odfs-real-as}"
ARCHIVE_FILE="$CACHE_DIR/arabian_nights.7z"
EXTRACT_DIR="$CACHE_DIR/arabian_nights"
ISO_FILE="$CACHE_DIR/arabian_nights.iso"
CUE_FILE="$EXTRACT_DIR/Arabian Nights (1993)(Buzz)(M4).cue"

have_tool() {
    command -v "$1" >/dev/null 2>&1
}

calc_md5() {
    if have_tool md5sum; then
        md5sum "$1" | awk '{print $1}'
    elif have_tool md5; then
        md5 -q "$1"
    else
        return 1
    fi
}

prepare_from_archive() {
    SEVEN_Z=$(command -v 7z || true)

    mkdir -p "$CACHE_DIR"

    if [ -f "$ARCHIVE_FILE" ] && [ "$(calc_md5 "$ARCHIVE_FILE")" = "$ARCHIVE_MD5" ]; then
        :
    else
        rm -f "$ARCHIVE_FILE"
        if ! have_tool curl; then
            return 2
        fi
        if ! /usr/bin/curl -L --fail --silent --show-error \
            -o "$ARCHIVE_FILE" "$ARCHIVE_URL"; then
            rm -f "$ARCHIVE_FILE"
            return 2
        fi
        if [ "$(calc_md5 "$ARCHIVE_FILE")" != "$ARCHIVE_MD5" ]; then
            echo "fixture md5 mismatch for downloaded archive" >&2
            rm -f "$ARCHIVE_FILE"
            return 2
        fi
    fi

    rm -rf "$EXTRACT_DIR"
    mkdir -p "$EXTRACT_DIR"
    if [ -z "$SEVEN_Z" ]; then
        return 2
    fi
    if ! have_tool python3; then
        return 2
    fi
    if ! "$SEVEN_Z" x -y "$ARCHIVE_FILE" "-o$EXTRACT_DIR" >/dev/null; then
        rm -rf "$EXTRACT_DIR"
        return 2
    fi
    if [ ! -f "$CUE_FILE" ]; then
        echo "fixture extract missing cue file" >&2
        return 2
    fi

    if ! python3 "$ROOT_DIR/tools/extract_data_track.py" "$CUE_FILE" "$ISO_FILE" >/dev/null; then
        rm -f "$ISO_FILE"
        return 2
    fi
    if [ "$(calc_md5 "$ISO_FILE")" != "$DATA_MD5" ]; then
        echo "fixture md5 mismatch for extracted ISO" >&2
        rm -f "$ISO_FILE"
        return 2
    fi
    return 0
}

if ! calc_md5 /dev/null >/dev/null 2>&1; then
    exit 2
fi

if [ -f "$ISO_FILE" ] && [ "$(calc_md5 "$ISO_FILE")" = "$DATA_MD5" ]; then
    printf '%s\n' "$ISO_FILE"
    exit 0
fi

if [ -n "$PREPARED_IMAGE" ] && [ -f "$PREPARED_IMAGE" ] &&
   [ "$(calc_md5 "$PREPARED_IMAGE")" = "$DATA_MD5" ]; then
    printf '%s\n' "$PREPARED_IMAGE"
    exit 0
fi

if prepare_from_archive; then
    printf '%s\n' "$ISO_FILE"
    exit 0
fi

exit 2
