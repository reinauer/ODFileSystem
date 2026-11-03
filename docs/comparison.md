# Comparing Amiga CD Filesystems

This page compares ODFileSystem with the seven other AmigaOS CD filesystems
measured by [CDBench](https://github.com/reinauer/CDBench), which publishes the
same performance results in its
[BENCHMARKS.md](https://github.com/reinauer/CDBench/blob/main/BENCHMARKS.md).
All results below were collected on one emulated system. The feature results are
what each handler actually did with a test disc, not what its documentation
says.

ODFileSystem results are from 1.0 beta2, which is the v0.9.0-8 tree with the
AmigaOS 3 memory and string functions added, built with GCC 16.2. Results for
the other filesystems were collected on 2026-10-30, and for ODFileSystem on
2026-11-02.

## Features

| Feature | ODFileSystem | CDFileSystem 47.26 | AllegroCDFS 3.9 | BabelCDFS 1.2 | CacheCDFS 43.4 | CacheCDFS 106.1 | AmiCDFS 2.40 | AsimCDFS 3.10 |
| --- | :-: | :-: | :-: | :-: | :-: | :-: | :-: | :-: |
| ISO 9660 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Rock Ridge long names | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Rock Ridge soft links | ✓ | – | – | – | – | – | ✓ | – |
| Joliet long names | ✓ | – | ✓ | – | – | – | – | – |
| High Sierra | ✓ | – | – | – | – | – | – | ✓ |
| UDF | ✓ | – | – | – | – | – | – | – |
| HFS | ✓ | – | – | – | – | – | ✓ | – |
| HFS+ | ✓ | – | – | – | – | – | – | – |
| Audio tracks as files | WAV, AIFF | – | raw | – | – | – | – | raw, AIFF, more |
| Native AmigaOS 4 build | ✓ | – | – | – | – | – | – | – |
| Latest version | 2026 | 2020 | 2012 | 1993 | 1995 | 1995 | 1997 | 1998 |

✓ means the handler listed the test disc with the expected names and read its
files correctly. – means it did not, using its CDBench mount entry. A handler
without Joliet support may still read the 8.3 ISO names on a disc that has
both.

Some handlers have features this table does not credit:

- AmiCDFS documents access to HFS resource forks, which ODFileSystem does not
  expose.
- AsimCDFS also offers mono audio tracks, Photo CD support, and CD³² and CDTV
  emulation.
- BabelCDFS contains a High Sierra identifier, but did not mount the High Sierra
  test disc, which ODFileSystem and AsimCDFS both read.

Multisession support is not compared. The tests use flat image files, which have
no multisession table of contents, so every handler saw only the first session.

## Performance

Median of three runs, each after a fresh boot. Repeat runs of a handler differed
by at most 0.1% in throughput and 2 µs in latency. **Bold** marks the best result
in a column.

### Throughput and directory traversal (higher is better)

| Filesystem | Sequential read KiB/s | Discovery entries/s | ExAll traversal entries/s |
| --- | --: | --: | --: |
| AllegroCDFS 3.9 | **203 539** | 7 163 | 7 429 |
| ODFileSystem 1.0 beta2 (68020) | 188 260 | 7 679 | **12 214** |
| CDFileSystem 47.26 | 186 612 | 7 560 | 7 613 |
| BabelCDFS 1.2 | 186 525 | **9 365** | 9 446 |
| ODFileSystem 1.0 beta2 (68000) | 177 959 | 7 362 | 12 193 |
| CacheCDFS 43.4 | 171 111 | 5 862 | 5 314 |
| CacheCDFS 106.1 | 171 105 | 5 882 | 5 319 |
| AmiCDFS 2.40 | 111 286 | 5 890 | 5 926 |
| AsimCDFS 3.10 | 8 231 | 3 724 | 3 840 |

### Latency in µs (lower is better)

| Filesystem | Random same offset | Random stride | Random seeded | Deep lock | Open from lock | Small open+read |
| --- | --: | --: | --: | --: | --: | --: |
| AllegroCDFS 3.9 | 156 | 283 | **156** | 547 | 349 | 1 832 |
| ODFileSystem 1.0 beta2 (68020) | 57 | **196** | 191 | 207 | **193** | **371** |
| CDFileSystem 47.26 | **56** | 683 | 605 | **146** | 213 | 1 103 |
| BabelCDFS 1.2 | 170 | 315 | 172 | 613 | 429 | 2 823 |
| ODFileSystem 1.0 beta2 (68000) | 64 | 217 | 212 | 212 | 197 | 406 |
| CacheCDFS 43.4 | 59 | 373 | 265 | 993 | 1 076 | 3 274 |
| CacheCDFS 106.1 | 58 | 373 | 265 | 992 | 1 075 | 3 255 |
| AmiCDFS 2.40 | 69 | 256 | 185 | 681 | 439 | 2 733 |
| AsimCDFS 3.10 | 273 | 419 | 410 | 288 | 529 | 1 731 |

ODFileSystem leads on whole-directory traversal, small-file access and random
reads at changing offsets, which dominate browsing and loading software from a
disc. AllegroCDFS is about 8% faster at sequential reads and has the fastest
seeded random reads, BabelCDFS is fastest at Discovery, the first traversal of
a mounted disc, and CDFileSystem has the fastest deep path lookup.

AsimCDFS mounted with 2 buffers instead of the 192 in its mount entry, so its
sequential result is unlikely to be its best.

### Changes since v0.6.0

Matching builds of the v0.6.0 release and 1.0 beta2, measured the same way:

| Test | v0.6.0 (68020) | 1.0 beta2 (68020) | Change | v0.6.0 (68000) | 1.0 beta2 (68000) | Change |
| --- | --: | --: | --: | --: | --: | --: |
| Sequential read, KiB/s | 188 185 | 188 260 | 0.0% | 179 172 | 177 959 | −0.7% |
| Discovery, entries/s | 7 328 | 7 679 | +4.8% | 5 688 | 7 362 | +29.4% |
| ExAll traversal, entries/s | 11 600 | 12 214 | +5.3% | 11 585 | 12 193 | +5.2% |
| Random same offset, µs | 68 | 57 | −16.2% | 81 | 64 | −21.0% |
| Random stride, µs | 275 | 196 | −28.7% | 326 | 217 | −33.4% |
| Random seeded, µs | 215 | 191 | −11.2% | 255 | 212 | −16.9% |
| Deep lock, µs | 319 | 207 | −35.1% | 320 | 212 | −33.8% |
| Open from lock, µs | 290 | 193 | −33.4% | 290 | 197 | −32.1% |
| Small open+read, µs | 644 | 371 | −42.4% | 699 | 406 | −41.9% |

Most of the improvement in lock and open times comes from register-argument
builds, added after v0.9.0. The v0.9.0 68020 handler measures 324 µs for deep
lock and 501 µs for small open+read. Most of the improvement in random reads
comes from the AmigaOS 3 handler's own memory functions, added after 1.0 beta1:
they copy sectors with `CopyMem` and copy long words between differently
aligned buffers on a 68020 or later. 1.0 beta1, built with GCC 13.4, measured
274 µs for random stride reads and 416 µs for small open+read on the 68020.

## Test setup

- WinUAE emulating an A4000 with a 68040, Kickstart 3.2.3, and AmigaOS 3.2.
- `a4091.device` 42.39, unit 2, with 2048-byte sectors and an emulated drive, so
  that results reflect the filesystem more than drive mechanics.
- The 55 MiB `CDBENCH_FIXTURE` ISO image from `make fixture-iso` in CDBench.
- CDBench run from the Shell after Workbench had loaded, with a 16 KiB stack.
- Each handler used its CDBench mount entry, with `Activate = 1` so that the disc
  was mounted before CDBench started. Without it, Workbench's disc-insert work
  runs during the timed walk, and the Discovery result of ODFileSystem 1.0
  beta1 fell from 7 529 to about 3 900 entries/s. ODFileSystem used `Buffers = 5` and
  `Control = "AIFF UDF"`.

The format tests walked each disc and read every file, using the images in
[tests/images](../tests/images) plus single-format versions of the UDF and HFS
discs, a High Sierra disc, and a two-track audio CD. On real drives, seek and
transfer times will narrow the performance differences.
