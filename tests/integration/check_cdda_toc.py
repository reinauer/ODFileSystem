#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the Amiga READ TOC transport/parser with 99 tracks and lead-out."""

from pathlib import Path
from struct import pack
import sys
from unittest.mock import patch

from amifuse.fuse_fs import HandlerBridge
from amifuse.rdb_inspect import detect_iso
from amifuse.scsi_device import IORequestStruct, SCSICmdStruct, ScsiDevice


ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "tests/images/test_plain.iso"
TRACK_COUNT = 99
TRACK_FRAMES = 300
LAST_TRACK_FRAMES = 525
AUDIO_FRAME_BYTES = 2352
WAV_HEADER_BYTES = 44


def check(handler: Path) -> None:
    # Format 0 descriptors supply only starting LBAs, not track lengths.
    # Make track 99 longer than its neighbours so its length must come
    # from the hundredth descriptor (lead-out), not the preceding spacing.
    starts = [i * TRACK_FRAMES for i in range(TRACK_COUNT)]
    leadout = starts[-1] + LAST_TRACK_FRAMES
    descriptors = b"".join(
        pack(">BBBBI", 0, 0x10, number, 0, start)
        for number, start in enumerate(starts, 1)
    )
    descriptors += pack(">BBBBI", 0, 0x10, 0xaa, 0, leadout)
    toc = pack(">HBB", len(descriptors) + 2, 1, TRACK_COUNT) + descriptors
    assert len(toc) == 804
    original_io = ScsiDevice.BeginIO
    toc_requests = []

    def audio_io(self, ctx, io_request):
        ior = IORequestStruct(ctx.mem, io_request)
        if ior.command.val == 28:  # HD_SCSICMD
            scsi = SCSICmdStruct(ctx.mem, ior.data.val)
            cdb = scsi.scsi_Command.val
            if ctx.mem.r8(cdb) == 0x43 and ctx.mem.r8(cdb + 2) == 0:
                allocation = ctx.mem.r16(cdb + 7)
                capacity = scsi.scsi_Length.val
                toc_requests.append((allocation, capacity))
                # Respect the caller's buffer even for a broken handler.
                # Check requests outside this hook: Vamos can swallow
                # exceptions raised while servicing an emulated device.
                response = toc[:min(allocation, capacity)]
                ctx.mem.w_block(scsi.scsi_Data.val, response)
                scsi.scsi_Actual.val = len(response)
                scsi.scsi_CmdActual.val = scsi.scsi_CmdLength.val
                scsi.scsi_Status.val = 0
                scsi.scsi_SenseActual.val = 0
                ior.actual.val = len(response)
                ior.error.val = 0
                ior.flags.val |= 1  # IOF_QUICK
                return
        return original_io(self, ctx, io_request)

    with patch.object(ScsiDevice, "BeginIO", audio_io):
        bridge = HandlerBridge(IMAGE, handler, debug=False, iso_info=detect_iso(IMAGE))
        try:
            # Read headers only: no synthetic audio payload is needed.
            for number, frames in ((98, TRACK_FRAMES), (99, leadout - starts[-1])):
                name = f"/Track{number:02d}.wav"
                header = bridge.read_file(name, WAV_HEADER_BYTES, 0)
                assert toc_requests, "handler never requested a format-0 TOC"
                for allocation, capacity in toc_requests:
                    assert allocation >= len(toc), \
                        f"READ TOC allocation {allocation} cannot hold the 804-byte TOC"
                    assert capacity >= allocation, \
                        "READ TOC data buffer is smaller than the CDB allocation"
                assert len(header) == WAV_HEADER_BYTES, f"{name}: missing WAV header"
                assert header[:4] == b"RIFF" and header[8:12] == b"WAVE", \
                    f"{name}: invalid WAV header"
                assert header[36:40] == b"data", f"{name}: missing data chunk"
                data_bytes = frames * AUDIO_FRAME_BYTES
                assert int.from_bytes(header[40:44], "little") == data_bytes, \
                    f"{name}: audio length does not match TOC LBAs"
                assert int.from_bytes(header[4:8], "little") == data_bytes + 36, \
                    f"{name}: incorrect RIFF size"
            packet, _ = bridge.launcher.send_packet(bridge.state, 5, [])  # ACTION_DIE
            replies = bridge._run_until_replies(sleep_base=0.0)
            assert any(pkt == packet and res1 == -1 and res2 == 0
                       for _, pkt, res1, res2 in replies), \
                "shutdown was not acknowledged"
            print("CDDA raw TOC passed: 804 bytes, 99 tracks, lead-out-derived length")
        finally:
            bridge.close()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: check_cdda_toc.py HANDLER")
    check(Path(sys.argv[1]))
