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
`make rom` enforces a default release size limit of `30000` bytes.

If intentional growth needs a higher ceiling, override it with:

```sh
make rom ROM_SIZE_LIMIT=<bytes>
```

The normal release handler similarly enforces `AMIGA_SIZE_LIMIT=60000`.
