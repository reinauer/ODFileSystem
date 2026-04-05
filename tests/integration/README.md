# AmiFUSE Integration Test

This integration test is intentionally local-only.

It exercises the AmigaDOS handler boundary through AmiFUSE by:

- launching `ODFileSystem` as an Amiga filesystem handler
- mounting a user-supplied AmiFUSE-compatible optical-disc image, or a temporary generated one
- checking that expected files and directories are visible
- reading a small sample file
- unmounting again cleanly

## Why this is local-only

The normal GitHub CI workflow does not run this test because it depends on:

- a working AmiFUSE installation
- FUSE mount support on the host
- a prepared AmiFUSE-compatible optical-disc image for `ODFileSystem`

GitHub-hosted runners are not a good fit for that combination.

## Required Environment

- `AMIFUSE_IMAGE` points to an AmiFUSE-compatible optical-disc image file
- `AMIGAOS32_ISO` can be used as a convenient local fallback image
- `ODFS_HANDLER` optionally overrides the handler path
- `AMIFUSE` optionally overrides the `amifuse` executable path
- `AMIFUSE_PARTITION` optionally selects the partition name/index
- `AMIFUSE_BLOCK_SIZE` optionally overrides AmiFUSE block size
- `AMIFUSE_EXPECT_FILE` optionally overrides the default probe file
- `AMIFUSE_EXPECT_CONTENT` optionally overrides the default text probe

## Default Probe Expectations

By default the script expects:

- a file named `SHORT.TXT`
- the file content `Short`

If neither `AMIFUSE_IMAGE` nor `AMIGAOS32_ISO` is set, the script generates a
small temporary ISO with that content and uses it for the smoke test.

If `AMIGAOS32_ISO` is used, the script instead probes:

- `CDVersion`
- `$VER: AmigaOS 3.2 CD-ROM Release 47.1`

If your local test image uses a different layout, override the defaults with env vars.

## Running

The intended local fixture is the AmigaOS 3.2 CD image if you have it available.
That image is not part of this repository and is not expected to be uploaded.

If you want a fully self-contained smoke test with no external image, just run:

```sh
make integration-check
```

This target builds and uses `build/amiga-test/ODFileSystem`, so serial logging
remains enabled during local handler testing.

To use the AmigaOS 3.2 CD image instead:

```sh
AMIGAOS32_ISO=/path/to/AmigaOS3.2CD.iso make integration-check
```

or:

```sh
AMIFUSE_IMAGE=/path/to/test.iso make integration-check
```
