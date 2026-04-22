#!/usr/bin/env python3
#
# SPDX-License-Identifier: BSD-2-Clause

from pathlib import Path
import sys

from amifuse.fuse_fs import HandlerBridge
from amifuse.rdb_inspect import detect_iso


def open_via_findinput(bridge: HandlerBridge, dir_path: str, name: str):
    dir_lock, _, locks = bridge.locate_path(dir_path)
    if dir_lock == 0:
        return None, "dir-lock"

    try:
        _, name_bptr = bridge._alloc_bstr(name)
        fh_addr = bridge._alloc_fh()
        bridge.launcher.send_findinput(bridge.state, name_bptr, dir_lock, fh_addr)
        replies = bridge._run_until_replies(
            max_iters=500, cycles=200_000, sleep_base=0.0
        )
        if not replies:
            return False, "no-reply"
        if replies[-1][2] == 0:
            return False, replies[-1][3]
        bridge.close_file(fh_addr)
        return True, 0
    finally:
        for lock in reversed(locks):
            bridge.free_lock(lock)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_assign_prefix.py IMAGE HANDLER", file=sys.stderr)
        return 2

    image = Path(sys.argv[1])
    handler = Path(sys.argv[2])
    iso_info = detect_iso(image)
    if iso_info is None:
        print("SKIP: assign-prefix check currently expects an ISO image")
        return 0

    bridge = HandlerBridge(image, handler, debug=False, iso_info=iso_info)
    try:
        candidates = [
            ("/deep/subdir", "nested.txt", "SUBDIR"),
            ("/Libs", "workbench.library", "LIBS"),
        ]

        found_case = False
        for dir_path, leaf, prefix in candidates:
            ok, info = open_via_findinput(bridge, dir_path, leaf)
            if ok is None:
                continue
            found_case = True
            if ok is not True:
                print(f"FAIL: control open {dir_path}/{leaf} failed ({info})")
                return 1

            ok, info = open_via_findinput(bridge, dir_path, f"{prefix}:{leaf}")
            if ok is not True:
                print(
                    "FAIL: assign-style prefix under directory lock did not "
                    f"resolve for {dir_path}/{prefix}:{leaf} ({info})"
                )
                return 1

        if found_case:
            print("assign-prefix checks passed")
            return 0

        print("SKIP: no assign-prefix regression target found in image")
        return 0
    finally:
        bridge.close()


if __name__ == "__main__":
    raise SystemExit(main())
