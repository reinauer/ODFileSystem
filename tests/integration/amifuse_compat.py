# SPDX-License-Identifier: BSD-2-Clause
"""Packets to the handler, for the AmiFUSE versions the checks run on."""

ACTION_FINDINPUT = 1005


def do_packet(bridge, action, args, **limits):
    """Send a packet and run the handler until it has answered.

    Returns (res1, res2), or None if no answer came. limits are for the
    wait of an AmiFUSE up to 1.0.
    """
    launcher = bridge.launcher
    if not hasattr(launcher, "send_packet"):
        # With amitools 0.10 the bridge sends packets through DosPktHandler.
        return bridge._packet(action, args, "check")
    packet, _ = launcher.send_packet(bridge.state, action, args)
    for _, pkt, res1, res2 in bridge._run_until_replies(sleep_base=0.0, **limits):
        if pkt == packet:
            return res1, res2
    return None


def find_input(bridge, dir_lock, name_bptr, fh_addr, **limits):
    """ACTION_FINDINPUT for a name under a lock, into the handle at fh_addr."""
    return do_packet(
        bridge, ACTION_FINDINPUT, [fh_addr >> 2, dir_lock, name_bptr], **limits
    )
