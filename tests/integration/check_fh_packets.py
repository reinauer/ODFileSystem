#!/usr/bin/env python3
#
# SPDX-License-Identifier: BSD-2-Clause

from pathlib import Path
import sys
from typing import Optional

from amifuse.fuse_fs import FileHandleStruct, HandlerBridge
from amifuse.rdb_inspect import detect_iso
from amitools.vamos.libstructs.dos import FileInfoBlockStruct, FileLockStruct


ACTION_COPY_DIR_FH = 1030
ACTION_PARENT_FH = 1031
ACTION_EXAMINE_FH = 1034
ACTION_EXAMINE_OBJECT = 23
ACTION_SAME_LOCK = 40
ACTION_PARENT = 29
FILE_LOCK_SIZE = FileLockStruct.get_size()
LOCK_SAME = 0
LOCK_SAME_VOLUME = 1
REPO_ROOT = Path(__file__).resolve().parents[2]
PLAIN_IMAGE = REPO_ROOT / "tests/images/test_plain.iso"
JOLIET_IMAGE = REPO_ROOT / "tests/images/test_joliet.iso"


def fib_name(bridge: HandlerBridge, fib_addr: int) -> str:
    raw = bridge.mem.r_block(fib_addr + 8, 108)
    return raw[1 : 1 + raw[0]].decode("latin-1")


def send_and_wait(bridge: HandlerBridge, pkt_type: int, args):
    bridge.launcher.send_packet(bridge.state, pkt_type, args)
    replies = bridge._run_until_replies(
        max_iters=500, cycles=200_000, sleep_base=0.0
    )
    if not replies:
        return None, "no-reply"
    return replies[-1][2], replies[-1][3]


def lock_examined_word(bridge: HandlerBridge, lock_bptr: int) -> int:
    return bridge.mem.r32((lock_bptr << 2) + FILE_LOCK_SIZE + 4)


def name_matches(actual: str, requested: str, expected: Optional[str]) -> bool:
    if expected is not None:
        return actual == expected
    return actual.casefold() == requested.casefold()


def check_candidate(
    bridge: HandlerBridge,
    label: str,
    dir_path: str,
    leaf: str,
    expected_leaf: Optional[str],
    expected_parent: Optional[str],
) -> Optional[int]:
    locks = []
    fh_addr = 0
    fh_opened = False
    parent_lock = 0
    dup_lock = 0
    try:
        dir_lock, _, locks = bridge.locate_path(dir_path)
        if dir_lock == 0:
            return None

        _, name_bptr = bridge._alloc_bstr(leaf)
        fh_addr = bridge._alloc_fh()
        bridge.launcher.send_findinput(
            bridge.state, name_bptr, dir_lock, fh_addr
        )
        replies = bridge._run_until_replies(
            max_iters=500, cycles=200_000, sleep_base=0.0
        )
        if not replies:
            print(f"FAIL: FINDINPUT had no reply for {dir_path}/{leaf}")
            return 1
        if replies[-1][2] == 0:
            print(
                f"FAIL: FINDINPUT failed for {dir_path}/{leaf} "
                f"({replies[-1][3]})"
            )
            return 1
        fh_opened = True

        fh_ptr = FileHandleStruct(bridge.mem, fh_addr).args.val
        fib_mem = bridge.vh.alloc.alloc_struct(
            FileInfoBlockStruct, label="FIB"
        )
        fib = FileInfoBlockStruct(bridge.mem, fib_mem.addr)

        res1, res2 = send_and_wait(
            bridge, ACTION_EXAMINE_FH, [fh_ptr, fib_mem.addr >> 2]
        )
        if res1 != -1:
            print(
                f"FAIL: EXAMINE_FH failed for {dir_path}/{leaf} ({res2})"
            )
            return 1
        examined_leaf = fib_name(bridge, fib_mem.addr)
        if not name_matches(examined_leaf, leaf, expected_leaf):
            expected = expected_leaf if expected_leaf is not None else leaf
            print(
                f"FAIL: EXAMINE_FH name for {dir_path}/{leaf}: "
                f"expected {expected!r}, got {examined_leaf!r}"
            )
            return 1

        res1, res2 = send_and_wait(bridge, ACTION_PARENT_FH, [fh_ptr])
        if res1 in (None, 0):
            print(
                f"FAIL: PARENT_FH failed for {dir_path}/{leaf} ({res2})"
            )
            return 1
        parent_lock = res1

        res1, res2 = send_and_wait(
            bridge, ACTION_EXAMINE_OBJECT, [parent_lock, fib_mem.addr >> 2]
        )
        if res1 != -1:
            print(
                f"FAIL: EXAMINE_OBJECT on PARENT_FH lock failed for "
                f"{dir_path}/{leaf} ({res2})"
            )
            return 1
        examined_parent = fib_name(bridge, fib_mem.addr)
        requested_parent = dir_path.rstrip("/").rsplit("/", 1)[-1]
        if not name_matches(
            examined_parent, requested_parent, expected_parent
        ):
            expected = (
                expected_parent
                if expected_parent is not None
                else requested_parent
            )
            print(
                f"FAIL: PARENT_FH name for {dir_path}/{leaf}: "
                f"expected {expected!r}, got {examined_parent!r}"
            )
            return 1

        res1, res2 = send_and_wait(bridge, ACTION_COPY_DIR_FH, [fh_ptr])
        if res1 in (None, 0):
            print(
                f"FAIL: COPY_DIR_FH failed for {dir_path}/{leaf} ({res2})"
            )
            return 1
        dup_lock = res1

        res1, res2 = send_and_wait(
            bridge, ACTION_EXAMINE_OBJECT, [dup_lock, fib_mem.addr >> 2]
        )
        if res1 != -1:
            print(
                f"FAIL: EXAMINE_OBJECT on COPY_DIR_FH lock failed for "
                f"{dir_path}/{leaf} ({res2})"
            )
            return 1
        copied_name = fib_name(bridge, fib_mem.addr)
        if copied_name != examined_leaf or fib.dir_entry_type.val >= 0:
            print(
                f"FAIL: COPY_DIR_FH object for {dir_path}/{leaf}: "
                f"expected file {examined_leaf!r}, got {copied_name!r} "
                f"type {fib.dir_entry_type.val}"
            )
            return 1
        if lock_examined_word(bridge, dup_lock) != 0xFFFFFFFF:
            print(
                "FAIL: EXAMINE_OBJECT did not mark the lock examined for "
                f"{dir_path}/{leaf}"
            )
            return 1

        res1, res2 = send_and_wait(
            bridge, ACTION_SAME_LOCK, [dup_lock, dup_lock]
        )
        if res1 != -1 or res2 != LOCK_SAME:
            print(f"FAIL: SAME_LOCK same object returned ({res1}, {res2})")
            return 1

        res1, res2 = send_and_wait(
            bridge, ACTION_SAME_LOCK, [dup_lock, parent_lock]
        )
        if res1 != 0 or res2 != LOCK_SAME_VOLUME:
            print(
                "FAIL: SAME_LOCK same-volume objects returned "
                f"({res1}, {res2})"
            )
            return 1

        res1, res2 = send_and_wait(bridge, ACTION_PARENT, [dup_lock])
        if res1 in (None, 0):
            print(
                "FAIL: COPY_DIR_FH lock stopped working after "
                f"EXAMINE_OBJECT for {dir_path}/{leaf} ({res2})"
            )
            return 1
        bridge.free_lock(res1)

        print(
            f"file-handle packet check passed for {label}: "
            f"{dir_path}/{leaf}"
        )
        return 0
    finally:
        if dup_lock:
            bridge.free_lock(dup_lock)
        if parent_lock:
            bridge.free_lock(parent_lock)
        if fh_addr:
            if fh_opened:
                bridge.close_file(fh_addr)
            else:
                bridge._free_fh(fh_addr)
        for lock in reversed(locks):
            bridge.free_lock(lock)


def check_image(
    image: Path,
    handler: Path,
    label: str,
    candidates,
    *,
    required: bool,
) -> int:
    iso_info = detect_iso(image)
    if iso_info is None:
        status = "FAIL" if required else "SKIP"
        print(f"{status}: {label} is not an ISO image: {image}")
        return 1 if required else 0

    bridge = HandlerBridge(image, handler, debug=False, iso_info=iso_info)
    try:
        for candidate in candidates:
            result = check_candidate(bridge, label, *candidate)
            if result is not None:
                return result

        status = "FAIL" if required else "SKIP"
        print(f"{status}: no file-handle target found in {label}")
        return 1 if required else 0
    finally:
        bridge.close()


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_fh_packets.py IMAGE HANDLER", file=sys.stderr)
        return 2

    image = Path(sys.argv[1])
    handler = Path(sys.argv[2])
    failures = 0

    fixture_cases = [
        (
            "plain ISO",
            PLAIN_IMAGE,
            [("/deep/subdir", "nested.txt", "NESTED.TXT", "SUBDIR")],
        ),
        (
            "Joliet",
            JOLIET_IMAGE,
            [("/DEEP/SUBDIR", "NESTED.TXT", "nested.txt", "subdir")],
        ),
    ]
    for label, fixture, candidates in fixture_cases:
        failures |= check_image(
            fixture,
            handler,
            label,
            candidates,
            required=True,
        )

    fixture_paths = {fixture.resolve() for _, fixture, _ in fixture_cases}
    if image.resolve() not in fixture_paths:
        failures |= check_image(
            image,
            handler,
            "external image",
            [
                ("/deep/subdir", "nested.txt", None, None),
                ("/C", "Version", None, None),
            ],
            required=False,
        )

    return failures


if __name__ == "__main__":
    raise SystemExit(main())
