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

#### Real-world Amiga Rock Ridge metadata

The golden suite includes an optional check for Amiga protection bits and
comments stored in Rock Ridge `AS` entries. Reference images:

- [Arabian Nights (1993)(Buzz)(M4)](https://archive.org/download/noaen-tosec-iso-commodore-amiga-cd32/Commodore/Amiga%20CD32/Games/%5BBIN%5D/Arabian%20Nights%20%281993%29%28Buzz%29%28M4%29.7z)
- [Benefactor (1994)(Psygnosis)](https://archive.org/download/noaen-tosec-iso-commodore-amiga-cd32/Commodore/Amiga%20CD32/Games/%5BIMG%5D/Benefactor%20%281994%29%28Psygnosis%29.7z)

The automated real-image golden test downloads only the smaller
`Arabian Nights` archive on demand, verifies the Archive.org MD5 before reuse,
extracts track 1 to a plain 2048-byte data image, and skips cleanly if download
or extraction tooling is unavailable. If a prepared data-track image already
exists locally, set `ODFS_REAL_AS_IMAGE=/path/to/arabian_nights.iso` to reuse it
without redownloading. The larger `Benefactor` image is kept as a manual
reference input.

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

## Performance changes

Compare release handlers with the same compiler, flags, CDBench binary,
fixture, mount settings, emulator build and CPU configuration. Use a fresh
boot for each run and alternate the baseline/candidate order. Keep diagnostic
instrumentation out of scored builds. Emulator runs measure filesystem
overhead with image-backed media; they do not measure optical-drive speed or
establish physical-hardware compatibility.

For AROS read-ahead changes, score all 16 applicable data-disc workloads from
`CDBench BENCH: ITER 1024 CSV`. Check the byte and operation counts as well as
the timings. Keep the three audio/mixed-disc skips explicit; a data-disc run
does not validate CDDA. Repeat load-contaminated runs using a criterion chosen
before examining their scores, and retain the excluded results.

AmigaOS 3 performance is a release gate even when the optimization targets
AROS. Test both the 68000 and 68020 release builds. Compare every metric with
the baseline's run-to-run range and the timer's reporting resolution; do not
assume a blanket percentage is normal noise. Investigate a repeatable slowdown
outside that envelope. Keep AmigaOS 4 as a separate comparison and native AROS
m68k/ARM/PPC execution as separate portability checks.

If a target's final release binary is byte-for-byte identical to its tested
baseline with fixed version/date strings, record both hashes. That proves
this patch cannot change its executable behavior; timing differences between
those identical artifacts measure environmental variation. Run a final guest
artifact check and keep this identity result distinct from a measured speedup.
