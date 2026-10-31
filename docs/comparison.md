# Comparing Amiga CD Filesystems

This page compares ODFileSystem with the seven other AmigaOS CD filesystems
measured by [CDBench](https://github.com/reinauer/CDBench), which publishes the
same performance results in its
[BENCHMARKS.md](https://github.com/reinauer/CDBench/blob/main/BENCHMARKS.md).
All results below were collected on 2026-10-30 on one emulated system. The feature results are
what each handler actually did with a test disc, not what its documentation
says.

ODFileSystem results are from 1.0 beta1, which is the v0.9.0-2 tree with the HFS
and HFS+ directory fixes applied, built with GCC 13.4.

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
| ODFileSystem 1.0 beta1 (68020) | 188 232 | 7 529 | **12 105** |
| CDFileSystem 47.26 | 186 612 | 7 560 | 7 613 |
| BabelCDFS 1.2 | 186 525 | **9 365** | 9 446 |
| ODFileSystem 1.0 beta1 (68000) | 177 901 | 6 613 | 12 042 |
| CacheCDFS 43.4 | 171 111 | 5 862 | 5 314 |
| CacheCDFS 106.1 | 171 105 | 5 882 | 5 319 |
| AmiCDFS 2.40 | 111 286 | 5 890 | 5 926 |
| AsimCDFS 3.10 | 8 231 | 3 724 | 3 840 |

### Latency in µs (lower is better)

| Filesystem | Random same offset | Random stride | Random seeded | Deep lock | Open from lock | Small open+read |
| --- | --: | --: | --: | --: | --: | --: |
| AllegroCDFS 3.9 | 156 | 283 | **156** | 547 | 349 | 1 832 |
| ODFileSystem 1.0 beta1 (68020) | 68 | 274 | 214 | 225 | **211** | **416** |
| CDFileSystem 47.26 | **56** | 683 | 605 | **146** | 213 | 1 103 |
| BabelCDFS 1.2 | 170 | 315 | 172 | 613 | 429 | 2 823 |
| ODFileSystem 1.0 beta1 (68000) | 81 | 323 | 253 | 225 | 213 | 470 |
| CacheCDFS 43.4 | 59 | 373 | 265 | 993 | 1 076 | 3 274 |
| CacheCDFS 106.1 | 58 | 373 | 265 | 992 | 1 075 | 3 255 |
| AmiCDFS 2.40 | 69 | **256** | 185 | 681 | 439 | 2 733 |
| AsimCDFS 3.10 | 273 | 419 | 410 | 288 | 529 | 1 731 |

ODFileSystem leads on whole-directory traversal and small-file access, which
dominate browsing and loading software from a disc. AllegroCDFS is about 8%
faster at sequential reads, BabelCDFS is fastest at Discovery, the first
traversal of a mounted disc, and CDFileSystem has the fastest deep path lookup.

AsimCDFS mounted with 2 buffers instead of the 192 in its mount entry, so its
sequential result is unlikely to be its best.

### Changes since v0.6.0

Matching builds of the v0.6.0 release and 1.0 beta1, measured the same way:

| Test | v0.6.0 (68020) | 1.0 beta1 (68020) | Change | v0.6.0 (68000) | 1.0 beta1 (68000) | Change |
| --- | --: | --: | --: | --: | --: | --: |
| Sequential read, KiB/s | 188 185 | 188 232 | 0.0% | 179 172 | 177 901 | −0.7% |
| Discovery, entries/s | 7 328 | 7 529 | +2.7% | 5 688 | 6 613 | +16.3% |
| ExAll traversal, entries/s | 11 600 | 12 105 | +4.4% | 11 585 | 12 042 | +3.9% |
| Random same offset, µs | 68 | 68 | 0% | 81 | 81 | 0% |
| Random stride, µs | 275 | 274 | −0.4% | 326 | 323 | −0.9% |
| Random seeded, µs | 215 | 214 | −0.5% | 255 | 253 | −0.8% |
| Deep lock, µs | 319 | 225 | −29.5% | 320 | 225 | −29.7% |
| Open from lock, µs | 290 | 211 | −27.2% | 290 | 213 | −26.6% |
| Small open+read, µs | 644 | 416 | −35.4% | 699 | 470 | −32.8% |

Most of the improvement in lock and open times comes from register-argument
builds, added after v0.9.0. The v0.9.0 68020 handler measures 324 µs for deep
lock and 501 µs for small open+read.

## Test setup

- WinUAE emulating an A4000 with a 68040, Kickstart 3.2.3, and AmigaOS 3.2.
- `a4091.device` 42.39, unit 2, with 2048-byte sectors and an emulated drive, so
  that results reflect the filesystem more than drive mechanics.
- The 55 MiB `CDBENCH_FIXTURE` ISO image from `make fixture-iso` in CDBench.
- CDBench run from the Shell after Workbench had loaded, with a 16 KiB stack.
- Each handler used its CDBench mount entry, with `Activate = 1` so that the disc
  was mounted before CDBench started. Without it, Workbench's disc-insert work
  runs during the timed walk, and ODFileSystem's Discovery result falls from
  7 529 to about 3 900 entries/s. ODFileSystem used `Buffers = 5` and
  `Control = "AIFF UDF"`.

The format tests walked each disc and read every file, using the images in
[tests/images](../tests/images) plus single-format versions of the UDF and HFS
discs, a High Sierra disc, and a two-track audio CD. On real drives, seek and
transfer times will narrow the performance differences.
