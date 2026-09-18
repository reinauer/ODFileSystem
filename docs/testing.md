# Testing Strategy

## Principles
- Most logic must be testable on host systems
- Every parser/backend must have direct tests
- Every bug fixed becomes a regression test
- Every supported format has golden images

## Test Categories

### Unit Tests (`tests/unit/`)
Test small deterministic components: endian helpers, path parsing, timestamps, directory record parsing, cache logic, logging, charset conversion.

### Golden Image Tests (`tests/golden/`)
For each image, define expected: detected backend(s), volume name, root listing, file metadata, file hashes, timestamps.

The golden suite also includes an optional real-world Amiga `AS` check. It
downloads the small `Arabian Nights` archive from Archive.org on demand,
verifies its MD5, extracts the data track, and skips cleanly if the network or
required extraction tools are unavailable.

### Malformed Image Tests (`tests/malformed/`)
Intentionally broken images to ensure: no crash, no infinite loop, graceful error paths, bounded resource usage.

### Differential Tests
Compare behavior against NetBSD, OpenBSD, Linux, CDVDFS. Differences documented as bug, intentional policy difference, or unsupported legacy quirk.

### Integration Tests (`tests/integration/`)
Test the AmigaDOS-facing handler: mount, list, read, examine, locks, path handling.

### Fuzz Targets (`tests/fuzz/`)
Host-side fuzz targets for each parser backend.

## Running Tests

```
make check    # build and run all unit tests
make golden-check  # run golden image tests with the host tools
make malformed-check  # run malformed-image smoke tests
make fuzz-check  # run parser fuzz smoke tests
make amiga      # release Amiga handler build, serial logging disabled
make amiga-test # test Amiga handler build, serial logging enabled
make adf        # build build/amiga-test/ODFileSystem.adf for emulator testing
make rom        # release ROM-profile build, serial logging disabled
make rom-test   # test ROM-profile build, serial logging enabled
make integration-check  # local-only AmiFUSE test using the amiga-test handler
AMIGAOS32_ISO=/path/to/AmigaOS3.2CD.iso make integration-check  # local-only AmiFUSE handler test
```

`make adf` also places `C/test_handler` on the test disk. It can be used on a
real Amiga to exercise lock/filehandle parent semantics, for example:

```
test_handler CD0:some/deep/directory REL=///sibling
test_handler CD0:CDDA/Track01.wav FILE=CD0:CDDA/Track01.wav
```

## CI Requirements
Per push: build, unit tests, golden image tests, malformed-image tests, parser fuzz smoke tests, Amiga handler build, ROM-profile build, static checks, warnings-as-errors.

CI and release packaging reject nonempty writable allocated sections in the
m68k 68000, 68020, and ROM-profile release handlers. The checker uses
`m68k-amigaos-objdump` (override with `OBJDUMP`) and rejects missing or invalid
binaries too. Amiga code hunks lack a `READONLY` flag, so `CODE` sections are
accepted; writable data and BSS are rejected. Debug handlers and OS4 binaries
are outside this check.

After building the release handlers, run the check locally with:

```
sh tests/amiga/test_rom_sections.sh
sh tools/check_rom_sections.sh \
    build/amiga/ODFileSystem \
    build/amiga-020/ODFileSystem \
    build/amiga-rom/ODFileSystem
```
