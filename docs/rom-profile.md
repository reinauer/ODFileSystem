# ROM Profile

## Goals
- Small footprint
- Minimal external dependencies
- Deterministic init
- Early logging support if enabled
- No heavy runtime allocation assumptions

## ROM Profile Includes
- ISO 9660
- Rock Ridge
- Joliet
- Small block cache (16 entries)
- Basic serial logging
- Optional multisession

## ROM Profile Excludes (Initially)
- UDF
- HFS / HFS+
- CDDA
- Advanced charset conversion
- Trace logging
- Large metadata caches
- Stream/read-ahead cache

## Build
```
make rom
```

This writes ROM-profile objects and the handler to `build/amiga-rom/`, so it does not
reuse or clobber the normal Amiga build in `build/amiga/`.

## Size Budget
TBD — will be established once core backends are implemented.
