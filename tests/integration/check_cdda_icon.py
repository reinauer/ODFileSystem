#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise OS3 icon loading with an incoming request queued during Open().

AmiFUSE supplies a synthetic audio TOC and the packaged default icon. Its DOS
implementation does not emulate synchronous packet replies, so explicitly
check that the incoming request cannot be consumed from the DOS reply port.
Run each case in a fresh interpreter, as with the other AmiFUSE checks.
"""

from pathlib import Path
import sys
from unittest.mock import patch

from amifuse.amiga_structs import DeviceNodeStruct
from amifuse.fuse_fs import HandlerBridge
from amifuse.rdb_inspect import detect_iso
from amifuse.scsi_device import IORequestStruct, SCSICmdStruct, ScsiDevice
from amifuse.startup_runner import HandlerLauncher
from amitools.vamos.lib.DosLibrary import DosLibrary
from amitools.vamos.lib.dos.FileHandle import FileHandle


ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "tests/images/test_plain.iso"
ICON_PATH = ROOT / "platform/amiga/env-archive/def_cdda.info"
ICON = ICON_PATH.read_bytes()
ACTION_IS_FILESYSTEM = 1027
ACTION_INHIBIT = 31


def check(handler: Path, source: str) -> None:
    original_io = ScsiDevice.BeginIO
    original_open = DosLibrary.Open
    original_launch = HandlerLauncher.launch_with_startup
    original_poll = HandlerLauncher.poll_replies
    pending = set()
    answered = set()
    lookups = []
    collisions = []
    launch = {}

    def launch_handler(self, *args, **kwargs):
        state = original_launch(self, *args, **kwargs)
        launch.update(launcher=self, state=state, process_port=state.port_addr)
        return state

    def audio_io(self, ctx, io_request):
        ior = IORequestStruct(ctx.mem, io_request)
        if ior.command.val == 28:
            scsi = SCSICmdStruct(ctx.mem, ior.data.val)
            cdb = scsi.scsi_Command.val
            if ctx.mem.r8(cdb) == 0x43 and ctx.mem.r8(cdb + 2) == 0:
                # One audio track starting at LBA 0, lead-out at LBA 750.
                toc = bytes.fromhex(
                    "00120101 0010010000000000 0010aa00000002ee"
                )
                assert scsi.scsi_Length.val >= len(toc)
                ctx.mem.w_block(scsi.scsi_Data.val, toc)
                scsi.scsi_Actual.val = len(toc)
                scsi.scsi_CmdActual.val = scsi.scsi_CmdLength.val
                scsi.scsi_Status.val = 0
                scsi.scsi_SenseActual.val = 0
                ior.actual.val = len(toc)
                ior.error.val = 0
                ior.flags.val |= 1  # IOF_QUICK
                return
        return original_io(self, ctx, io_request)

    def open_icon(self, ctx, name_ptr, mode):
        name = ctx.mem.r_cstr(name_ptr)
        if name not in ("ENV:Sys/def_cdda.info", "ENVARC:Sys/def_cdda.info"):
            return original_open(self, ctx, name_ptr, mode)
        lookups.append(name)
        launcher = launch["launcher"]
        state = launch["state"]
        dn = DeviceNodeStruct(ctx.mem, launcher.boot["dn_addr"])
        port = dn.dn_Task.val
        pkt, _ = launcher._build_std_packet(
            port, state.reply_port_addr, ACTION_IS_FILESYSTEM, []
        )
        pending.add(pkt)
        # Real DOS expects only its own reply here. A queued filesystem
        # request on pr_MsgPort is the AN_AsyncPkt failure condition.
        if launcher.exec_impl.port_mgr.has_msg(launch["process_port"]):
            collisions.append(name)
        if not name.startswith(source + ":"):
            self.setioerr(ctx, 205)  # ERROR_OBJECT_NOT_FOUND
            return 0
        fh = FileHandle(ICON_PATH.open("rb"), name, str(ICON_PATH))
        self.file_mgr._register_file(fh)
        return fh.b_addr

    def poll(self, *args, **kwargs):
        replies = original_poll(self, *args, **kwargs)
        for _, pkt, res1, res2 in replies:
            if pkt in pending and res1 == -1 and res2 == 0:
                answered.add(pkt)
        return replies

    with patch.object(HandlerLauncher, "launch_with_startup", launch_handler), \
         patch.object(HandlerLauncher, "poll_replies", poll), \
         patch.object(ScsiDevice, "BeginIO", audio_io), \
         patch.object(DosLibrary, "Open", open_icon):
        bridge = HandlerBridge(IMAGE, handler, debug=False, iso_info=detect_iso(IMAGE))
        try:
            for mounted in range(2):
                data = bridge.read_file("/Disk.info", len(ICON) + 32, 0)
                assert data == (b"" if source == "missing" else ICON), \
                    "Disk.info differs from the configured default icon"
                assert bridge.read_file("/Track01.wav", 4, 0) == b"RIFF"
                assert lookups, "audio mount never attempted the icon lookup"
                assert not collisions, f"filesystem request on DOS reply port: {collisions}"
                assert pending == answered, "request queued during icon load was lost"
                if mounted == 0:
                    pending.clear()
                    answered.clear()
                    # Repeat loading after an inhibit/uninhibit cycle.
                    for state in (-1, 0):
                        bridge.launcher.send_packet(bridge.state, ACTION_INHIBIT, [state])
                        replies = bridge._run_until_replies(sleep_base=0.0)
                        assert replies and replies[0][2] == -1, "inhibit cycle failed"
            expected = ["ENV:Sys/def_cdda.info"]
            if source != "ENV":
                expected.append("ENVARC:Sys/def_cdda.info")
            assert lookups == expected * 2, f"unexpected lookup order: {lookups}"
            # AmiFUSE's legacy port registry retains freed port addresses;
            # observe the actual Exec deletion calls instead of that cache.
            messages = bridge.vh.slm.exec_impl.msg_func
            with patch.object(messages, "delete_msg_port",
                              wraps=messages.delete_msg_port) as delete:
                bridge.launcher.send_packet(bridge.state, 5, [])  # ACTION_DIE
                replies = bridge._run_until_replies(sleep_base=0.0)
                assert replies and replies[0][2] == -1, "shutdown was not acknowledged"
                freed = [call.args[0].addr for call in delete.call_args_list]
                assert freed.count(bridge.state.port_addr) == 1, "request port leaked"
                assert launch["process_port"] not in freed, "process port was deleted"
            print(f"CDDA icon packet isolation passed: {source}, startup and remount")
        finally:
            bridge.close()


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[2] not in ("ENV", "ENVARC", "missing"):
        sys.exit("usage: check_cdda_icon.py HANDLER ENV|ENVARC|missing")
    check(Path(sys.argv[1]), sys.argv[2])
