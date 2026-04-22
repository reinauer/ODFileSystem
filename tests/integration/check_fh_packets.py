#!/usr/bin/env python3
#
# SPDX-License-Identifier: BSD-2-Clause

from pathlib import Path
import sys

from amifuse.fuse_fs import FileHandleStruct, HandlerBridge
from amifuse.rdb_inspect import detect_iso
from amitools.vamos.libstructs.dos import FileInfoBlockStruct, FileLockStruct


ACTION_COPY_DIR_FH = 1030
ACTION_PARENT_FH = 1031
ACTION_EXAMINE_FH = 1034
ACTION_EXAMINE_OBJECT = 23
ACTION_PARENT = 29
FILE_LOCK_SIZE = FileLockStruct.get_size()


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


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_fh_packets.py IMAGE HANDLER", file=sys.stderr)
        return 2

    image = Path(sys.argv[1])
    handler = Path(sys.argv[2])
    iso_info = detect_iso(image)
    if iso_info is None:
        print("SKIP: file-handle packet check currently expects an ISO image")
        return 0

    bridge = HandlerBridge(image, handler, debug=False, iso_info=iso_info)
    try:
        candidates = [
            ("/deep/subdir", "nested.txt", "subdir"),
            ("/C", "Version", "C"),
        ]

        for dir_path, leaf, parent_name in candidates:
            dir_lock, _, locks = bridge.locate_path(dir_path)
            if dir_lock == 0:
                continue

            fh_addr = 0
            parent_lock = 0
            dup_lock = 0
            try:
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
                if fib_name(bridge, fib_mem.addr) != leaf:
                    print(
                        f"FAIL: EXAMINE_FH returned wrong name for "
                        f"{dir_path}/{leaf}"
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
                if res1 != -1 or fib_name(bridge, fib_mem.addr) != parent_name:
                    print(
                        f"FAIL: PARENT_FH returned wrong lock for "
                        f"{dir_path}/{leaf}"
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
                if fib_name(bridge, fib_mem.addr) != leaf or fib.dir_entry_type.val >= 0:
                    print(
                        f"FAIL: COPY_DIR_FH returned wrong object for "
                        f"{dir_path}/{leaf}"
                    )
                    return 1
                if lock_examined_word(bridge, dup_lock) != 0xFFFFFFFF:
                    print(
                        "FAIL: EXAMINE_OBJECT did not mark the lock examined for "
                        f"{dir_path}/{leaf}"
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

                print(f"file-handle packet check passed for {dir_path}/{leaf}")
                return 0
            finally:
                if dup_lock:
                    bridge.free_lock(dup_lock)
                if parent_lock:
                    bridge.free_lock(parent_lock)
                if fh_addr:
                    bridge.close_file(fh_addr)
                for lock in reversed(locks):
                    bridge.free_lock(lock)

        print("SKIP: no file-handle regression target found in image")
        return 0
    finally:
        bridge.close()


if __name__ == "__main__":
    raise SystemExit(main())
