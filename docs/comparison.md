# Comparing Amiga CD Filesystems

This page compares ODFileSystem with the seven other AmigaOS CD filesystems
measured by [CDBench](https://github.com/reinauer/CDBench), which publishes the
same performance results in its
[BENCHMARKS.md](https://github.com/reinauer/CDBench/blob/main/BENCHMARKS.md).
All results below were collected on one emulated system. The feature results are
what each handler actually did with a test disc, not what its documentation
says.

ODFileSystem results are from 1.0 beta3, which is the v0.9.0-30 tree, built
with GCC 16.2. Results for the other filesystems were collected on 2026-10-30,
and for ODFileSystem on 2026-11-02.

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
| ODFileSystem 1.0 beta3 (68020) | **205 197** | **9 770** | **13 064** |
| ODFileSystem 1.0 beta3 (68000) | 205 174 | 9 601 | 12 976 |
| AllegroCDFS 3.9 | 203 539 | 7 163 | 7 429 |
| CDFileSystem 47.26 | 186 612 | 7 560 | 7 613 |
| BabelCDFS 1.2 | 186 525 | 9 365 | 9 446 |
| CacheCDFS 43.4 | 171 111 | 5 862 | 5 314 |
| CacheCDFS 106.1 | 171 105 | 5 882 | 5 319 |
| AmiCDFS 2.40 | 111 286 | 5 890 | 5 926 |
| AsimCDFS 3.10 | 8 231 | 3 724 | 3 840 |

### Latency in µs (lower is better)

| Filesystem | Random same offset | Random stride | Random seeded | Deep lock | Open from lock | Small open+read |
| --- | --: | --: | --: | --: | --: | --: |
| ODFileSystem 1.0 beta3 (68020) | **53** | **161** | **153** | **137** | **159** | **319** |
| ODFileSystem 1.0 beta3 (68000) | 60 | 162 | 154 | 139 | 160 | 339 |
| AllegroCDFS 3.9 | 156 | 283 | 156 | 547 | 349 | 1 832 |
| CDFileSystem 47.26 | 56 | 683 | 605 | 146 | 213 | 1 103 |
| BabelCDFS 1.2 | 170 | 315 | 172 | 613 | 429 | 2 823 |
| CacheCDFS 43.4 | 59 | 373 | 265 | 993 | 1 076 | 3 274 |
| CacheCDFS 106.1 | 58 | 373 | 265 | 992 | 1 075 | 3 255 |
| AmiCDFS 2.40 | 69 | 256 | 185 | 681 | 439 | 2 733 |
| AsimCDFS 3.10 | 273 | 419 | 410 | 288 | 529 | 1 731 |

The 68020 build of ODFileSystem is the fastest of the eight filesystems in every
CDBench metric: sequential reads, Discovery and whole-directory traversal,
random reads, path lookups, and small-file access. Its narrowest leads are over
AllegroCDFS in sequential reads (under 1%) and seeded random reads (3 µs), and
over CDFileSystem in random reads from the same offset (3 µs) and deep path
lookups (9 µs).

The 68000 build is ahead of every other filesystem in every metric but one:
random reads from the same offset, where CDFileSystem and both CacheCDFS
versions are faster.

AsimCDFS mounted with 2 buffers instead of the 192 in its mount entry, so its
sequential result is unlikely to be its best.

### Changes since v0.6.0

Matching builds of the v0.6.0 release and 1.0 beta3, measured the same way:

| Test | v0.6.0 (68020) | 1.0 beta3 (68020) | Change | v0.6.0 (68000) | 1.0 beta3 (68000) | Change |
| --- | --: | --: | --: | --: | --: | --: |
| Sequential read, KiB/s | 188 185 | 205 197 | +9.0% | 179 172 | 205 174 | +14.5% |
| Discovery, entries/s | 7 328 | 9 770 | +33.3% | 5 688 | 9 601 | +68.8% |
| ExAll traversal, entries/s | 11 600 | 13 064 | +12.6% | 11 585 | 12 976 | +12.0% |
| Random same offset, µs | 68 | 53 | −22.1% | 81 | 60 | −25.9% |
| Random stride, µs | 275 | 161 | −41.5% | 326 | 162 | −50.3% |
| Random seeded, µs | 215 | 153 | −28.8% | 255 | 154 | −39.6% |
| Deep lock, µs | 319 | 137 | −57.1% | 320 | 139 | −56.6% |
| Open from lock, µs | 290 | 159 | −45.2% | 290 | 160 | −44.8% |
| Small open+read, µs | 644 | 319 | −50.5% | 699 | 339 | −51.5% |

Up to 1.0 beta2, most of the improvement in lock and open times came from
register-argument builds, added after v0.9.0. The v0.9.0 68020 handler measures
324 µs for deep lock and 501 µs for small open+read. Most of the improvement in
random reads came from the AmigaOS 3 handler's own memory functions, added after
1.0 beta1:
they copy sectors with `CopyMem` and copy long words between differently
aligned buffers on a 68020 or later. 1.0 beta1, built with GCC 13.4, measured
274 µs for random stride reads and 416 µs for small open+read on the 68020.

1.0 beta3 builds on 1.0 beta2. File data goes from the drive straight into the
program's buffer with SCSI READ(10) commands, and a Seek starts reading the
sectors it lands on, so on the 68020 sequential reads rose from 188 260 to
205 197 KiB/s and seeded random reads fell from 191 to 153 µs. Directory scans convert and
compare names in fewer instructions, raising Discovery from 7 679 to
9 770 entries/s. A path is resolved in one walk through the cached
directories instead of one lookup per name, cutting deep lock from 207 to
137 µs.

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
