# ROM Profile

## Goals
- Small footprint
- Minimal external dependencies
- Deterministic init
- Quiet release build by default
- No heavy runtime allocation assumptions

## ROM Profile Includes
- ISO 9660
- Rock Ridge
- Joliet
- Small block cache (16 entries)
- Optional multisession

## ROM Profile Excludes (Initially)
- UDF
- HFS / HFS+
- CDDA
- Serial logging in the release build
- Packet trace logging
- Large metadata caches
- Stream/read-ahead cache

## Build
```
make rom
make rom-test
```

`make rom` writes the release ROM-profile handler to `build/amiga-rom/` with
serial output disabled.

`make rom-test` writes the test ROM-profile handler to
`build/amiga-rom-test/` with serial output enabled.

These targets do not reuse or clobber the normal Amiga build in `build/amiga/`.

## Size Budget
The ROM-profile handler should stay small enough for ROM-oriented use, but the
project does not currently enforce a hard size limit in CI. Track meaningful
growth in commit messages and release notes.
