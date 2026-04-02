# ODFileSystem — Amiga optical filesystem
# SPDX-License-Identifier: BSD-2-Clause
#
# Makefile for host and Amiga cross-compilation builds

# ---- toolchain selection ----

# Amiga cross-compiler (m68k-amigaos)
CC      = m68k-amigaos-gcc
STRIP   = m68k-amigaos-strip

# NDK include path (override with: make NDK_PATH=/your/path)
NDK_PATH ?= $(shell realpath $$(dirname $$(which $(CC)))/../m68k-amigaos/ndk-include 2>/dev/null)

# Host compiler
HOSTCC ?= cc

# ---- common flags ----

INCLUDES = -I include -I backends

# ---- host build flags ----

HOSTCFLAGS  = -std=c11 -O2 -g \
              -Wall -Wextra -Wpedantic -Werror \
              -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
              -Wno-unused-parameter
HOSTLDFLAGS =

# ---- Amiga build flags (following xsysinfo conventions) ----

CFLAGS  = -O2 -m68000 -mtune=68020-60 -msoft-float -noixemul \
          -Wall -Wextra \
          -Wstrict-prototypes -Wmissing-prototypes \
          -DAMIGA
LDFLAGS = -noixemul
LIBS    = -lamiga -lgcc

# ---- build directories ----

HOST_BUILD  = build/host
AMIGA_BUILD = build/amiga

# ---- shared source lists ----

# Core library (shared between host and Amiga)
CORE_SRCS = \
    core/error.c \
    core/log.c \
    core/node.c \
    core/cache_block.c \
    core/charset.c \
    core/mount.c \
    backends/iso9660/iso9660.c

# Host-only sources
HOST_SRCS = platform/host/file_media.c

# Amiga-only sources (handler frontend, device I/O — stubs for now)
AMIGA_SRCS = platform/amiga/dos_glue.c

HOST_LIB_SRCS  = $(CORE_SRCS) $(HOST_SRCS)
HOST_LIB_OBJS  = $(patsubst %.c,$(HOST_BUILD)/%.o,$(HOST_LIB_SRCS))

AMIGA_LIB_SRCS = $(CORE_SRCS) $(AMIGA_SRCS)
AMIGA_LIB_OBJS = $(patsubst %.c,$(AMIGA_BUILD)/%.o,$(AMIGA_LIB_SRCS))

# ---- test binaries (host only) ----

TEST_SRCS = $(wildcard tests/unit/test_*.c)
TEST_BINS = $(patsubst tests/unit/%.c,$(HOST_BUILD)/tests/%,$(TEST_SRCS))

# ---- host tool binaries ----

TOOL_NAMES = imginfo imgls imgcat imgstat imgbench imgdump
TOOL_BINS  = $(patsubst %,$(HOST_BUILD)/tools/%,$(TOOL_NAMES))

# ---- handler target (Amiga) ----

HANDLER = $(AMIGA_BUILD)/ODFileSystem

# ==================================================================
# targets
# ==================================================================

.PHONY: all host amiga lib tests tools check clean

all: host

host: lib tests tools

amiga: $(HANDLER)
	@echo "  $(HANDLER) built successfully"
	@wc -c < "$(HANDLER)" | awk '{printf "  Handler size: %s bytes\n", $$1}'

# ---- host library ----

lib: $(HOST_BUILD)/libodfs.a

$(HOST_BUILD)/libodfs.a: $(HOST_LIB_OBJS)
	@mkdir -p $(@D)
	@echo "  AR    $@"
	@$(AR) rcs $@ $^

# ---- host object files ----

$(HOST_BUILD)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(INCLUDES) $(HOSTCFLAGS) -c -o $@ $<

# ---- host tests ----

tests: $(TEST_BINS)

$(HOST_BUILD)/tests/test_%: tests/unit/test_%.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(INCLUDES) -I tests/unit $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

check: tests
	@echo "=== Running unit tests ==="
	@failures=0; \
	for t in $(TEST_BINS); do \
	    echo "--- $$(basename $$t) ---"; \
	    $$t || failures=$$((failures + 1)); \
	    echo; \
	done; \
	if [ $$failures -ne 0 ]; then \
	    echo "$$failures test suite(s) failed"; \
	    exit 1; \
	fi; \
	echo "All test suites passed"

# ---- host tools ----

tools: $(TOOL_BINS)

$(HOST_BUILD)/tools/imginfo: tools/imginfo/imginfo.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgls: tools/imgls/imgls.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgcat: tools/imgcat/imgcat.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgstat: tools/imgstat/imgstat.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgbench: tools/imgbench/imgbench.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgdump: tools/imgdump/imgdump.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

# ---- Amiga library ----

$(AMIGA_BUILD)/libodfs.a: $(AMIGA_LIB_OBJS)
	@mkdir -p $(@D)
	@echo "  AR    $@ (amiga)"
	@m68k-amigaos-ar rcs $@ $^

# ---- Amiga object files ----

$(AMIGA_BUILD)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  CC    $<"
	@$(CC) $(INCLUDES) $(CFLAGS) -c -o $@ $<

# ---- Amiga handler ----

$(HANDLER): $(AMIGA_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  LINK  $@"
	@echo "  (handler frontend not yet implemented — producing stub)"
	@touch $@

# ---- clean ----

clean:
	@echo "  CLEAN"
	@rm -rf build
