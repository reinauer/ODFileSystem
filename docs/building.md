# Building ODFileSystem

The Amiga handler is built with the `amiga` make target. The Makefile selects
the Amiga frontend from the compiler target:

- `m68k-amigaos-gcc` builds the AmigaOS 3/classic handler
- `ppc-amigaos-gcc` builds the native AmigaOS 4 handler

## AmigaOS 3

With an m68k AmigaOS cross-compiler in `PATH`, run:

```sh
make amiga
```

This uses `m68k-amigaos-gcc` by default and writes:

```text
build/amiga/ODFileSystem
```

If the compiler is not in `PATH`, pass it explicitly:

```sh
make amiga CC=/path/to/m68k-amigaos-gcc
```

If the matching `ar`, `size`, and `strip` tools are also outside `PATH`, pass
those too:

```sh
make amiga \
  CC=/path/to/m68k-amigaos-gcc \
  AMIGA_AR=/path/to/m68k-amigaos-ar \
  AMIGA_SIZE=/path/to/m68k-amigaos-size \
  STRIP=/path/to/m68k-amigaos-strip
```

To build the OS3 release archive, use:

```sh
make amigaos3-lha
```

This creates `build/amiga/ODFileSystem.lha` containing `ODFileSystem`,
`ODFileSystem020`, `ODFileSystem-test`, `ODFileSystem-rom`,
`ODFileSystem-rom-test`, `Env-Archive/Sys/def_cdda.info`, and `README.md`.
The test ADF is built and released separately.

## AmigaOS 4

With the AmigaOS 4 PPC toolchain in `PATH`, run:

```sh
make amiga CC=ppc-amigaos-gcc
```

This selects `AMIGA_TARGET=os4` automatically and builds the native OS4
filesystem handler. To keep OS3 and OS4 outputs side by side, use a separate
build directory:

```sh
make amiga \
  CC=ppc-amigaos-gcc \
  AMIGA_BUILD=build/amiga-os4
```

The handler is then written to:

```text
build/amiga-os4/ODFileSystem
```

For OS4 builds the Makefile also writes a Kickstart-module form:

```text
build/amiga-os4/CDFileSystem
```

These two files are intentionally different:

- `ODFileSystem` is the normal disk-loadable filesystem handler. Copy it to
  `L:ODFileSystem` and use it with a DOSDriver or mountlist.
- `CDFileSystem` is a relocatable Kickstart resident module. Use it when
  replacing `Kickstart/CDFileSystem` in an OS4 Kickstart directory or
  `Kickstart.zip`; the Kicklayout entry remains `MODULE Kickstart/CDFileSystem`.

If the OS4 toolchain is installed outside `PATH`, pass the full tool paths:

```sh
make amiga \
  CC=/opt/amiga-ppc/bin/ppc-amigaos-gcc \
  AMIGA_AR=/opt/amiga-ppc/bin/ppc-amigaos-ar \
  AMIGA_OBJCOPY=/opt/amiga-ppc/bin/ppc-amigaos-objcopy \
  AMIGA_SIZE=/opt/amiga-ppc/bin/ppc-amigaos-size \
  STRIP=/opt/amiga-ppc/bin/ppc-amigaos-strip \
  AMIGA_BUILD=build/amiga-os4
```

The Makefile normally derives the NDK include path from the selected compiler.
If your SDK is elsewhere, add `NDK_PATH=/path/to/include_h`.

## Debug and Size Limits

Release builds have serial logging disabled. For a test build with serial
output enabled, use `make amiga-test` with the same toolchain selection:

```sh
make amiga-test
make amiga-test CC=ppc-amigaos-gcc AMIGA_TEST_BUILD=build/amiga-os4-test
```

The OS4 test build likewise produces both `ODFileSystem` and `CDFileSystem`
under the selected test build directory.

To build the OS4 release archive, use:

```sh
make amigaos4-lha \
  CC=ppc-amigaos-gcc \
  AMIGA_BUILD=build/amiga-os4 \
  AMIGA_TEST_BUILD=build/amiga-os4-test
```

This creates `build/amiga-os4/ODFileSystem-amigaos4.lha` containing
`ODFileSystem-amigaos4`, `CDFileSystem`, `ODFileSystem-amigaos4-test`,
`CDFileSystem-test`, `Env-Archive/Sys/def_cdda.info`, and `README.md`.

Release builds enforce a default size limit of `98304` bytes for OS3 and
`155648` bytes for OS4. If intentional growth needs a higher ceiling, override
it with `AMIGA_SIZE_LIMIT=<bytes>`. During local bring-up, the limit can be
disabled with `ENFORCE_SIZE_LIMITS=0`.

See [ROM Profile](rom-profile.md) for ROM builds and
[Testing Strategy](testing.md) for running the test suites.
