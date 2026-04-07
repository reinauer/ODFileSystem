#!/usr/bin/env python3
"""
Extract a plain 2048-byte-per-sector data track from a simple CUE sheet.

Supported:
- TRACK nn MODE1/2352
- TRACK nn MODE2/2352
- TRACK nn MODE1/2048

This is intended for local fixture preparation, not as a full general-purpose
 BIN/CUE converter.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Track:
    num: int
    mode: str
    file_name: str
    index01_frames: int | None


def msf_to_frames(msf: str) -> int:
    mm, ss, ff = [int(part) for part in msf.split(":")]
    return mm * 60 * 75 + ss * 75 + ff


def parse_cue(path: Path) -> list[Track]:
    tracks: list[Track] = []
    current_file: str | None = None

    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line:
            continue

        m = re.match(r'FILE\s+"([^"]+)"\s+(\S+)', line, re.I)
        if m:
            current_file = m.group(1)
            continue

        m = re.match(r'TRACK\s+(\d+)\s+(\S+)', line, re.I)
        if m:
            tracks.append(
                Track(
                    num=int(m.group(1)),
                    mode=m.group(2).upper(),
                    file_name=current_file or "",
                    index01_frames=None,
                )
            )
            continue

        m = re.match(r'INDEX\s+01\s+(\d\d:\d\d:\d\d)', line, re.I)
        if m and tracks:
            tracks[-1].index01_frames = msf_to_frames(m.group(1))

    return tracks


def sector_layout(mode: str) -> tuple[int, int]:
    if mode == "MODE1/2048":
        return 2048, 0
    if mode == "MODE1/2352":
        return 2352, 16
    if mode == "MODE2/2352":
        return 2352, 24
    raise ValueError(f"unsupported track mode: {mode}")


def extract_track(cue_path: Path, track_num: int, out_path: Path) -> None:
    tracks = parse_cue(cue_path)
    if not tracks:
        raise ValueError(f"no tracks found in {cue_path}")

    track = next((t for t in tracks if t.num == track_num), None)
    if track is None:
        raise ValueError(f"track {track_num} not found in {cue_path}")
    if track.index01_frames is None:
        raise ValueError(f"track {track_num} has no INDEX 01 in {cue_path}")

    raw_sector_size, data_offset = sector_layout(track.mode)
    bin_path = cue_path.parent / track.file_name
    start = track.index01_frames * raw_sector_size
    file_size = bin_path.stat().st_size

    if track.file_name:
        same_file_tracks = [t for t in tracks if t.file_name == track.file_name]
        same_index = same_file_tracks.index(track)
        if same_index + 1 < len(same_file_tracks):
            next_track = same_file_tracks[same_index + 1]
            if next_track.index01_frames is None:
                raise ValueError(f"next track after {track_num} has no INDEX 01")
            end = next_track.index01_frames * raw_sector_size
        else:
            end = file_size
    else:
        end = file_size

    if end < start:
        raise ValueError("computed negative track span")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with bin_path.open("rb") as src, out_path.open("wb") as dst:
        src.seek(start)
        remaining = end - start
        while remaining >= raw_sector_size:
            sector = src.read(raw_sector_size)
            if len(sector) != raw_sector_size:
                break
            dst.write(sector[data_offset:data_offset + 2048])
            remaining -= raw_sector_size


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cue", type=Path, help="input .cue file")
    parser.add_argument("output", type=Path, help="output ISO/data-track file")
    parser.add_argument(
        "--track",
        type=int,
        default=1,
        help="track number to extract (default: %(default)s)",
    )
    args = parser.parse_args()

    extract_track(args.cue, args.track, args.output)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
