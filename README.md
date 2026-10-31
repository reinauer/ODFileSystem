# ODFileSystem

[![Coverity](https://img.shields.io/coverity/scan/33023.svg)](https://scan.coverity.com/projects/33023)

ODFileSystem is a read-only optical-disc filesystem for AmigaOS 3 and AmigaOS 4.
It lets Amiga applications browse and read CD-ROM, DVD, Blu-ray, and image-based
media, and access audio-CD tracks as WAV or AIFF files.

## Why ODFileSystem

ODFileSystem has the most complete optical-disc format support of any Amiga CD
filesystem we tested. It is the only one that reads UDF and HFS+, so DVDs,
Blu-ray discs, and modern Mac discs work alongside classic CD-ROMs, and the only
one that reads every format below. It also has native AmigaOS 3 and AmigaOS 4
builds, and it is open source and actively developed.

| Feature | ODFileSystem | CDFileSystem 47.26 | AllegroCDFS 3.9 | BabelCDFS 1.2 | CacheCDFS 43.4 / 106.1 | AmiCDFS 2.40 | AsimCDFS 3.10 |
| --- | :-: | :-: | :-: | :-: | :-: | :-: | :-: |
| ISO 9660 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Rock Ridge long names | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Joliet long names | ✓ | – | ✓ | – | – | – | – |
| High Sierra | ✓ | – | – | – | – | – | ✓ |
| UDF | ✓ | – | – | – | – | – | – |
| HFS | ✓ | – | – | – | – | ✓ | – |
| HFS+ | ✓ | – | – | – | – | – | – |
| Audio tracks as files | ✓ | – | ✓ | – | – | – | ✓ |

It is also fast where browsing and loading software are concerned. In
[CDBench](https://github.com/reinauer/CDBench), which publishes its
[results for all eight filesystems](https://github.com/reinauer/CDBench/blob/main/BENCHMARKS.md),
the 68020 build is the fastest at whole-directory traversal (12 105 entries/s,
28% ahead of the next best) and small-file open and read (416 µs, over 2.5 times
faster than the next best). Its sequential read speed is second to AllegroCDFS,
which is about 8% faster. On Discovery, the first pass over a mounted disc, it
is level with CDFileSystem at about 7 500 entries/s, behind BabelCDFS at 9 365.

See the [full comparison](docs/comparison.md) for every result, the test setup,
and the areas where other filesystems lead.

## Installation

Download the [release archive](https://github.com/reinauer/ODFileSystem/releases)
for your AmigaOS version.

### Choose the handler

- **AmigaOS 3:** copy `ODFileSystem` from the OS3 archive to `L:ODFileSystem`.
  On a 68020 or later CPU, you can use `ODFileSystem020` instead; copy it as
  `L:ODFileSystem` to use the same mount configuration.
- **AmigaOS 4:** copy `ODFileSystem-amigaos4` from the OS4 archive as
  `L:ODFileSystem`.

If you built from source, use `build/amiga/ODFileSystem`, or the `ODFileSystem`
file in your chosen build directory.

### Set up a DOSDriver

Copy the sample [CD0](platform/amiga/dosdrivers/CD0) and
[CD0.info](platform/amiga/dosdrivers/CD0.info) from this repository to
`DEVS:DOSDrivers/`.
Edit `FileSystem` in CD0 to read `L:ODFileSystem`, then set the `Device` and
`Unit` tooltypes on the CD0 icon to match your controller and drive.

The sample CD0 entry uses `Control = "AIFF UDF"`, selecting AIFF audio tracks
and preferring UDF on bridge discs. Edit or remove this line to choose the
[format defaults](#format-selection).

Only one active DOSDriver may use a given device name. If another CD
filesystem is already mounted as `CD0:`, replace or disable that entry before
mounting ODFileSystem, or rename the ODFileSystem DOSDriver to `OD0:`.
Double-click the DOSDriver to mount it, or restart to mount it from
`DEVS:DOSDrivers/`.

For a default audio-CD disk icon, copy `Env-Archive/Sys/def_cdda.info` from the
release archive to `ENVARC:Sys/def_cdda.info` and `ENV:Sys/def_cdda.info`.
The source copy is [def_cdda.info](platform/amiga/env-archive/def_cdda.info).

### AmigaOS 4 Kickstart installation

The OS4 archive also includes `CDFileSystem` for replacing
`Kickstart/CDFileSystem` in an OS4 Kickstart directory or `Kickstart.zip`.
Keep the Kicklayout entry as `MODULE Kickstart/CDFileSystem`.
Use the `ODFileSystem-amigaos4` file for a DOSDriver installation.

## Mount Configuration

As an alternative to a DOSDriver, add this entry to your Mountlist and adjust
`Device` and `Unit` for your hardware:

```text
CD0:
    FileSystem = L:ODFileSystem
    Stacksize = 16384
    Priority  = 5
    GlobVec   = -1
    DosType   = 0x43443031
    ForceLoad = 1
    Device    = scsi.device
    Unit      = 2
    Flags     = 0
    Surfaces  = 1
    BlocksPerTrack = 1
    BlockSize = 2048
    Reserved  = 0
    LowCyl    = 0
    HighCyl   = 0
    Buffers   = 20
    BufMemType = 0
    Mount     = 1
    Activate  = 1
#
```

`ForceLoad = 1` matters on systems whose controller ROM registers a CD
filesystem for DosType 'CD01' in FileSystem.resource (the A4091 does):
without it, Mount silently uses that ROM filesystem instead of
`L:ODFileSystem` and overrides the mountlist StackSize.

`Activate = 1` starts the filesystem at mount, so a disc in the drive gets its
Workbench icon without waiting for first access. The sample CD0 entry sets it
too; remove the line to save memory until the drive is used. Keep it when
running benchmarks: without it, Workbench's disc-insert work runs during the
first pass over the disc and halves the CDBench Discovery result.

If the drive is empty at mount, a disc inserted later is detected only if the
device reports `TDERR_DiskChanged` for an empty drive. `a4091.device` 42.39 does
not, so with that version the disc mounts on first access to `CD0:`.

Use the mount entry's `Control` string to select formats and adjust caching:

```text
Control = "LOWERCASE UDF"
```

Supported control flags:

- `LOWERCASE` to lowercase plain ISO names
- `NOROCKRIDGE` or `NORR` to disable Rock Ridge
- `NOJOLIET` or `NOJ` to disable Joliet
- `HFSFIRST` or `HF` to prefer HFS on hybrid discs
- `UDF` to prefer UDF on bridge discs
- `AIFF` to expose audio tracks as AIFF instead of WAV
- `FILEBUFFERS=n` or `FB=n` to set the block-cache size in sectors
  (default 128 on AmigaOS 3, 1024 on AmigaOS 4)
- `METACACHE=n` or `MC=n` to set the parsed-directory cache size in KiB
  (default 96 on AmigaOS 3, 1024 on AmigaOS 4; 0 turns it off)

See [mountlist.example](docs/mountlist.example)
for a fuller example with hardware notes, and
[CD0](platform/amiga/dosdrivers/CD0) for the
sample DOSDriver entry used with Workbench.

## Supported Formats

| Filesystem | Status | Notes |
| --- | --- | --- |
| ISO 9660 | Supported | Plain ISO 9660 names and directory traversal |
| High Sierra | Supported | Pre-ISO 9660 (1986) variant, detected automatically; not in the ROM profile |
| Rock Ridge | Supported | Preferred over plain ISO when present |
| Joliet | Supported | Preferred over plain ISO when Rock Ridge is absent |
| UDF | Supported | Bridge discs default to ISO-family content unless `Control = "UDF"` is set |
| HFS | Supported with limitations | Data fork only |
| HFS+ | Supported with limitations | Data fork only; resource forks are not exposed |
| CDDA | Supported | Virtual WAV files, or AIFF with `Control = "AIFF"` |

ODFileSystem selects the last session on multisession discs by default.
Rock Ridge and UDF symbolic links appear as Amiga soft links. Amiga protection
bits and comments stored in Rock Ridge `AS` entries are preserved alongside
standard Rock Ridge names, timestamps, links, and directory structure.

### Format selection

When several directory trees are available, ODFileSystem prefers Rock Ridge,
then Joliet, then plain ISO 9660. On bridge or hybrid discs it prefers the
ISO-family view over UDF or HFS.

To prefer UDF, set:

```text
Control = "UDF"
```

To prefer HFS, use `Control = "HFSFIRST"`. Options can be combined, for example
`Control = "UDF LOWERCASE"`.

### Name collisions

If multiple on-disc names become the same AmigaDOS name after name or character
set conversion, the first entry keeps the original name. Later entries receive
`~2`, `~3`, and so on, in on-disc order.

### HFS and HFS+ limitations

Only data forks are exposed. Resource forks and Finder metadata are not
available as separate files. Some classic Mac software distributions may
therefore be incomplete when viewed through ODFileSystem, even though their
main data files remain readable.

### Audio CDs

Audio tracks appear as virtual WAV files by default, or AIFF files when
`Control = "AIFF"` is set. On mixed-mode discs they appear in a `CDDA/`
directory; on pure audio discs they appear at the root.

When the drive returns CD-Text, disc and track metadata appears in
`CD-TEXT.txt`, and each track's title becomes its AmigaDOS file comment.

## Building from Source

With an m68k AmigaOS cross-compiler in `PATH`:

```sh
make amiga
```

This produces `build/amiga/ODFileSystem`. For AmigaOS 4, use a PowerPC AmigaOS
cross-compiler and a separate build directory:

```sh
make amiga CC=ppc-amigaos-gcc AMIGA_BUILD=build/amiga-os4
```

See [Building ODFileSystem](docs/building.md) for toolchain setup, debug builds,
and release archives, and [ROM Profile](docs/rom-profile.md) for ROM builds.

## Development Documentation

- [Filesystem comparison](docs/comparison.md)
- [Architecture](docs/architecture.md)
- [Testing](docs/testing.md)
- [Release publishing](docs/releasing.md)

## License

ODFileSystem is licensed under the BSD 2-Clause license. See [LICENSE](LICENSE)
for the full text.
