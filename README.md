# ODFileSystem

ODFileSystem is a read-only optical-disc filesystem driver for Amiga systems. It is implemented as an AmigaDOS handler frontend with clean backend plugins for the various optical-disc formats it supports, allowing it to mount and browse CD-ROM, DVD, Blu-ray, and image-based media.

The project also includes host-side tools and tests so most parser and cache logic can be developed and validated outside an Amiga environment.

## Purpose

ODFileSystem is the most modern and complete optical-disc filesystem for the Amiga. The handler deals with AmigaDOS packets, locks, and file handles, while the core library and backend parsers detect media formats, choose the appropriate view of hybrid discs, enumerate directories, and read file data.

ODFileSystem can:

- mount optical media through a standard AmigaDOS handler
- support common CD/DVD filesystem formats and hybrid discs
- present a consistent read-only file and directory model to Amiga software
- allow most logic to be tested on a host machine using image files

## Supported Filesystems And Features

### Filesystems

ODFileSystem currently includes backends for:

- ISO 9660
- Rock Ridge
- Joliet
- UDF
- HFS
- HFS+
- CDDA virtual audio tracks

### Features

- Read-only AmigaDOS handler for optical media
- Multisession support, with the last session selected by default
- Hybrid-disc precedence rules
- Deterministic duplicate-name handling after normalization or charset conversion
- Rock Ridge, Joliet, UDF, HFS, and HFS+ probing
- Path lookup, directory enumeration, and file reads
- Block-cache based media access
- Host-side tools for inspecting image files

### Feature Matrix

| Filesystem | Status | Notes |
| --- | --- | --- |
| ISO 9660 | Supported | Plain ISO 9660 names and directory traversal |
| Rock Ridge | Supported | Preferred over plain ISO when present |
| Joliet | Supported | Preferred over plain ISO when Rock Ridge is absent |
| UDF | Supported | Bridge discs default to ISO-family content unless forced |
| HFS | Supported with limitations | Data fork only |
| HFS+ | Supported with limitations | Data fork only; resource forks are not exposed |
| CDDA | Supported | Exposed as virtual WAV files |

### Format Selection Rules

For ISO-family hybrids, the default precedence is:

1. Rock Ridge
2. Joliet
3. Plain ISO 9660

For bridge and hybrid discs:

- ISO-family content is preferred over UDF by default
- ISO-family content is preferred over HFS by default
- UDF can be preferred explicitly
- HFS can be preferred explicitly
- A specific session or backend can be forced by mount options

### Name Collision Policy

If multiple on-disc names normalize to the same visible AmigaDOS name, the first
entry keeps the unsuffixed name and later entries are renamed deterministically
in on-disc order using `~2`, `~3`, and so on.

### HFS And HFS+ Limitations

- Only data forks are exposed. Resource forks and Finder metadata are not
  presented as separate files or alternate streams.
- Because of those limits, some classic Mac software distributions may be
  incomplete when viewed through ODFileSystem even though their main data files
  remain readable.

### CDDA

Audio tracks are exposed as virtual WAV files. On mixed-mode discs they appear in a `CDDA/` directory, and on pure audio discs they appear at the root.

## Sample Mountlist

Build the handler with:

```sh
make amiga
```

This builds the release handler with serial logging disabled. For a test build
with serial output enabled, use `make amiga-test`.

Then copy `build/amiga/ODFileSystem` to `L:ODFileSystem` and add a mount entry such as:

```text
CD0:
    Handler   = L:ODFileSystem
    Stacksize = 8192
    Priority  = 5
    GlobVec   = -1
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

Optional handler control flags can be supplied through the mount entry control string, for example:

```text
Control = "LOWERCASE UDF FILEBUFFERS=128"
```

Supported control flags:

- `LOWERCASE` to lowercase plain ISO names
- `NOROCKRIDGE` or `NORR` to disable Rock Ridge
- `NOJOLIET` or `NOJ` to disable Joliet
- `HFSFIRST` or `HF` to prefer HFS on hybrid discs
- `UDF` to prefer UDF on bridge discs
- `FILEBUFFERS` or `FB` to set the block-cache size

See [docs/mountlist.example](/Users/stepan/git/xcdfs/docs/mountlist.example) for a fuller example with hardware notes.

## Unit Tests

Unit tests are host-side tests under `tests/unit/`. Run them with:

```sh
make check
```

This builds the host library and the unit-test binaries, then runs all suites. In this workspace, `make check` completes successfully.

The repository also contains image-based golden tests in `tests/golden/`, but the unit-test entry point is `make check`.

## License

ODFileSystem is licensed under the BSD 2-Clause license. See [LICENSE](/Users/stepan/git/xcdfs/LICENSE) for the full text.
