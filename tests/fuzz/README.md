# Parser Fuzz Smoke Targets

These fuzz targets are lightweight host-side smoke harnesses for parser entry
points. They are not sanitizer-driven fuzzers by themselves; instead, they
accept arbitrary files and corpus directories and try to mount and minimally
exercise directory traversal, lookup, path resolution, and file reads without
crashing.

Available targets:

- `fuzz_auto`
- `fuzz_iso9660`
- `fuzz_joliet`
- `fuzz_udf`
- `fuzz_hfs`
- `fuzz_hfsplus`

Run them all with:

```sh
make fuzz-check
```
