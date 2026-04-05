# Malformed Image Tests

This directory contains malformed-image coverage for the host tools.

The committed corpus is intentionally small and simple:

- `not-an-image.bin` is arbitrary non-image input
- `short-sector.bin` is shorter than one 2048-byte sector
- `pvd-like-garbage.bin` includes a `CD001` marker in the wrong context

The test runner also generates temporary malformed cases from valid images at
runtime, including truncated images and images with deliberately corrupted
volume-descriptor bytes.
