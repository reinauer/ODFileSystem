# ODFileSystem — Amiga optical filesystem
# SPDX-License-Identifier: BSD-2-Clause
#
# Makefile for host and Amiga cross-compilation builds

# ---- toolchain selection ----

# Amiga cross-compiler (m68k-amigaos or m68k-aros-gcc)
CC      = m68k-amigaos-gcc
STRIP   = m68k-amigaos-strip

# NDK include path (override with: make NDK_PATH=/your/path)
NDK_PATH ?= $(shell realpath $$(dirname $$(which $(CC)))/../m68k-amigaos/ndk-include 2>/dev/null)

# AROS cross-compiler (override: make CC=m68k-aros-gcc AROS=1)
# When AROS=1, uses -static instead of -noixemul and defines __AROS__
AROS ?= 0

# Host compiler
HOSTCC ?= cc

# ---- common flags ----

AMIGA_DATE ?= $(shell date '+%-d.%-m.%Y')
ODFS_GIT_VERSION ?= $(shell desc=$$(git describe --tags --match "v*" --dirty --always 2>/dev/null || echo unknown); printf '%s\n' "$$desc" | grep -q '^v' && printf '%s' "$$desc" || printf 'early-0-g%s' "$$desc")

INCLUDES = -I include -I backends

# ---- optional 3rdparty submodules ----

GIT := $(shell git -C "$(CURDIR)" rev-parse --git-dir 1>/dev/null 2>&1 \
	&& command -v git)
ifneq ($(GIT),)
freshsubs := $(shell git submodule update --init 3rdparty/libcodesets \
	2>/dev/null)
endif

# ---- host build flags ----

HOSTCFLAGS  = -std=c11 -O2 -g \
              -Wall -Wextra -Wpedantic -Werror \
              -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
              -Wno-unused-parameter \
              -MMD -MP
HOSTLDFLAGS =

# ---- Amiga build options ----

# Serial debug output (override with: make SERIAL_DEBUG=1)
SERIAL_DEBUG ?= 0

# Packet trace instrumentation (override with: make PACKET_TRACE=1)
PACKET_TRACE ?= 0

# Release size limits (override when intentional growth is approved)
AMIGA_SIZE_LIMIT ?= 60000
ROM_SIZE_LIMIT   ?= 30000
SIZE_LIMIT_NAME  ?= AMIGA_SIZE_LIMIT
SIZE_LIMIT_DESC  ?= release Amiga handler

# Backend selection (override to disable: make FEATURE_UDF=0)
FEATURE_ISO9660      ?= 1
FEATURE_ROCK_RIDGE   ?= 1
FEATURE_JOLIET       ?= 1
FEATURE_MULTISESSION ?= 1
FEATURE_UDF          ?= 1
FEATURE_HFS          ?= 1
FEATURE_HFSPLUS      ?= 1
FEATURE_CDDA         ?= 1

# ---- Amiga build flags (following xsysinfo conventions) ----

FEATURE_DEFS = \
          -DODFS_AMIGA_DATE=\"$(AMIGA_DATE)\" \
          -DODFS_GIT_VERSION=\"$(ODFS_GIT_VERSION)\" \
          -DODFS_SERIAL_DEBUG=$(SERIAL_DEBUG) \
          -DODFS_PACKET_TRACE=$(PACKET_TRACE) \
          -DODFS_FEATURE_LOG=$(SERIAL_DEBUG) \
          -DODFS_FEATURE_ISO9660=$(FEATURE_ISO9660) \
          -DODFS_FEATURE_ROCK_RIDGE=$(FEATURE_ROCK_RIDGE) \
          -DODFS_FEATURE_JOLIET=$(FEATURE_JOLIET) \
          -DODFS_FEATURE_MULTISESSION=$(FEATURE_MULTISESSION) \
          -DODFS_FEATURE_UDF=$(FEATURE_UDF) \
          -DODFS_FEATURE_HFS=$(FEATURE_HFS) \
          -DODFS_FEATURE_HFSPLUS=$(FEATURE_HFSPLUS) \
          -DODFS_FEATURE_CDDA=$(FEATURE_CDDA)

ifeq ($(AROS),1)
CFLAGS  = -Os -m68000 -mtune=68020-60 -msoft-float -static -nostartfiles \
          -Wall -Wextra -Werror \
          -Wstrict-prototypes -Wmissing-prototypes \
          -Wno-array-bounds \
          -MMD -MP \
          -DAMIGA -D__AROS__ $(FEATURE_DEFS)
LDFLAGS = -static
LIBS    = -lamiga -lgcc
else
CFLAGS  = -Os -m68000 -mtune=68020-60 -msoft-float -noixemul -nostartfiles \
          -Wall -Wextra -Werror \
          -Wstrict-prototypes -Wmissing-prototypes \
          -Wno-array-bounds \
          -MMD -MP \
          -DAMIGA $(FEATURE_DEFS)
LDFLAGS = -noixemul
LIBS    = -lamiga -lgcc
endif

# ---- build directories ----

HOST_BUILD  = build/host
AMIGA_BUILD = build/amiga
ROM_BUILD   = build/amiga-rom
AMIGA_TEST_BUILD = build/amiga-test
ROM_TEST_BUILD   = build/amiga-rom-test

# ---- shared source lists ----

# Core library (shared between host and Amiga)
CORE_SRCS = \
    core/error.c \
    core/log.c \
    core/node.c \
    core/namefix.c \
    core/cache_block.c \
    core/charset.c \
    core/mount.c \
    core/session.c \
    backends/iso9660/iso9660.c \
    backends/rock_ridge/rock_ridge.c \
    backends/joliet/joliet.c \
    backends/udf/udf.c \
    backends/hfs/hfs.c \
    backends/hfsplus/hfsplus.c \
    backends/cdda/cdda.c

# Host-only sources
HOST_SRCS = platform/host/file_media.c

# Amiga handler sources
AMIGA_SRCS = platform/amiga/handler_main.c \
    platform/amiga/libc_stubs.c \
    platform/amiga/printf_local.c

# Amiga assembly
AMIGA_ASM_SRCS = platform/amiga/startup.S
AMIGA_ASM_OBJS = $(patsubst %.S,$(AMIGA_BUILD)/%.o,$(AMIGA_ASM_SRCS))

HOST_LIB_SRCS  = $(CORE_SRCS) $(HOST_SRCS)
HOST_LIB_OBJS  = $(patsubst %.c,$(HOST_BUILD)/%.o,$(HOST_LIB_SRCS))
HOST_LIB_DEPS  = $(HOST_LIB_OBJS:.o=.d)

AMIGA_LIB_SRCS = $(CORE_SRCS) $(AMIGA_SRCS)
AMIGA_LIB_OBJS = $(patsubst %.c,$(AMIGA_BUILD)/%.o,$(AMIGA_LIB_SRCS))
AMIGA_LIB_DEPS = $(AMIGA_LIB_OBJS:.o=.d)
AMIGA_ASM_DEPS = $(AMIGA_ASM_OBJS:.o=.d)

# ---- test binaries (host only) ----

TEST_SRCS = $(wildcard tests/unit/test_*.c)
TEST_BINS = $(patsubst tests/unit/%.c,$(HOST_BUILD)/tests/%,$(TEST_SRCS))
TEST_DEPS = $(patsubst tests/unit/%.c,$(HOST_BUILD)/tests/%.d,$(TEST_SRCS))

# ---- fuzz binaries (host only) ----

FUZZ_SRCS = $(wildcard tests/fuzz/fuzz_*.c)
FUZZ_BINS = $(patsubst tests/fuzz/%.c,$(HOST_BUILD)/tests/%,$(FUZZ_SRCS))
FUZZ_DEPS = $(patsubst tests/fuzz/%.c,$(HOST_BUILD)/tests/%.d,$(FUZZ_SRCS))

# ---- host tool binaries ----

TOOL_NAMES = imginfo imgls imgcat imgstat imgbench imgdump
TOOL_BINS  = $(patsubst %,$(HOST_BUILD)/tools/%,$(TOOL_NAMES))
TOOL_DEPS  = $(patsubst %,$(HOST_BUILD)/tools/%.d,$(TOOL_NAMES))

# ---- handler target (Amiga) ----

HANDLER      = $(AMIGA_BUILD)/ODFileSystem
TEST_HANDLER = $(AMIGA_TEST_BUILD)/ODFileSystem
ADF          = $(AMIGA_TEST_BUILD)/ODFileSystem.adf
ADF_VOLUME   = ODFileSystem
ADF_DOSDRIVER      = platform/amiga/dosdrivers/CD0
ADF_DOSDRIVER_ICON = platform/amiga/dosdrivers/CD0.info
XDFTOOL      ?= xdftool

# ==================================================================
# targets
# ==================================================================

.PHONY: all host amiga amiga-test adf rom rom-test lib tests tools fuzz check golden-check malformed-check fuzz-check integration-check clean size

all: host

host: lib tests tools

amiga: $(HANDLER)
	@echo "  $(HANDLER) built successfully"
	@size=$$(wc -c < "$(HANDLER)"); \
	echo "  Handler size: $$size bytes"; \
	if [ "$(ENFORCE_SIZE_LIMITS)" != "0" ] && [ "$$size" -gt "$(AMIGA_SIZE_LIMIT)" ]; then \
		echo "  ERROR: $(SIZE_LIMIT_DESC) exceeds $(AMIGA_SIZE_LIMIT) bytes"; \
		echo "  If this growth is intentional, rerun with $(SIZE_LIMIT_NAME)=<new-limit>"; \
		exit 1; \
	fi

amiga-test:
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(AMIGA_TEST_BUILD) \
		ENFORCE_SIZE_LIMITS=0 \
		SERIAL_DEBUG=1 \
		amiga

adf: amiga-test $(ADF_DOSDRIVER) $(ADF_DOSDRIVER_ICON) Makefile
	@mkdir -p $(dir $(ADF))
	@echo "  ADF   $(ADF)"
	@$(XDFTOOL) -f $(ADF) \
		create + \
		format "$(ADF_VOLUME)" + \
		makedir L + \
		write $(TEST_HANDLER) L + \
		write $(ADF_DOSDRIVER) + \
		write $(ADF_DOSDRIVER_ICON)
	@echo "  ADF image ready: $(ADF)"

# ROM profile: minimal build for burning into ROM
# ISO9660 + Rock Ridge + Joliet + Multisession, no debug, no UDF/HFS/CDDA
rom:
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(ROM_BUILD) \
		CPPFLAGS="$(CPPFLAGS) -DODFS_PROFILE_ROM" \
		AMIGA_SIZE_LIMIT=$(ROM_SIZE_LIMIT) \
		SIZE_LIMIT_NAME=ROM_SIZE_LIMIT \
		SIZE_LIMIT_DESC=ROM\ profile\ handler \
		SERIAL_DEBUG=0 \
		FEATURE_UDF=0 \
		FEATURE_HFS=0 \
		FEATURE_HFSPLUS=0 \
		FEATURE_CDDA=0 \
		amiga
	@echo "  ROM profile build complete"

rom-test:
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(ROM_TEST_BUILD) \
		CPPFLAGS="$(CPPFLAGS) -DODFS_PROFILE_ROM" \
		ENFORCE_SIZE_LIMITS=0 \
		SERIAL_DEBUG=1 \
		FEATURE_UDF=0 \
		FEATURE_HFS=0 \
		FEATURE_HFSPLUS=0 \
		FEATURE_CDDA=0 \
		amiga
	@echo "  ROM test profile build complete"

# Print size breakdown of Amiga library objects
size: $(AMIGA_BUILD)/libodfs.a
	@echo "=== Amiga object sizes ==="
	@m68k-amigaos-size $(AMIGA_BUILD)/libodfs.a

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
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -c -o $@ $<

# ---- host tests ----

tests: $(TEST_BINS)

$(HOST_BUILD)/tests/test_%: tests/unit/test_%.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) -I tests/unit $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

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

golden-check: tools
	@echo "=== Running golden image tests ==="
	@TOOLS="$(PWD)/$(HOST_BUILD)/tools" sh tests/golden/test_formats.sh
	@TOOLS="$(PWD)/$(HOST_BUILD)/tools" FETCH="$(PWD)/tests/golden/fetch_real_as_fixture.sh" sh tests/golden/test_as_real.sh

malformed-check: tools
	@echo "=== Running malformed image tests ==="
	@TOOLS="$(PWD)/$(HOST_BUILD)/tools" tests/malformed/test_malformed.sh

fuzz: $(FUZZ_BINS)

$(HOST_BUILD)/tests/fuzz_%: tests/fuzz/fuzz_%.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

fuzz-check: fuzz
	@echo "=== Running parser fuzz smoke tests ==="
	@FUZZ_BINS="$(PWD)/$(HOST_BUILD)/tests" tests/fuzz/run_fuzz.sh

integration-check: amiga-test
	@echo "=== Running AmiFUSE integration test ==="
	@ODFS_HANDLER="$(PWD)/$(TEST_HANDLER)" tests/integration/test_amifuse.sh

# ---- host tools ----

tools: $(TOOL_BINS)

$(HOST_BUILD)/tools/imginfo: tools/imginfo/imginfo.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgls: tools/imgls/imgls.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgcat: tools/imgcat/imgcat.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgstat: tools/imgstat/imgstat.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgbench: tools/imgbench/imgbench.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgdump: tools/imgdump/imgdump.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

# ---- Amiga library ----

$(AMIGA_BUILD)/libodfs.a: $(AMIGA_LIB_OBJS)
	@mkdir -p $(@D)
	@echo "  AR    $@ (amiga)"
	@m68k-amigaos-ar rcs $@ $^

# ---- Amiga object files ----

$(AMIGA_BUILD)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  CC    $<"
	@$(CC) $(CPPFLAGS) $(INCLUDES) $(CFLAGS) -c -o $@ $<

# ---- Amiga assembly ----

$(AMIGA_BUILD)/%.o: %.S
	@mkdir -p $(@D)
	@echo "  AS    $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# ---- Amiga handler ----

$(HANDLER): $(AMIGA_ASM_OBJS) $(AMIGA_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) -nostartfiles -o $@ $(AMIGA_ASM_OBJS) -L$(AMIGA_BUILD) -lodfs -nostdlib -Wl,-u,_exit -lgcc -lc -lgcc -lamiga -ramiga-dev
	@echo "  STRIP $@"
	@$(STRIP) $@


# ---- clean ----

clean:
	@echo "  CLEAN"
	@rm -rf build

-include $(HOST_LIB_DEPS) $(AMIGA_LIB_DEPS) $(AMIGA_ASM_DEPS) \
	$(TEST_DEPS) $(FUZZ_DEPS) $(TOOL_DEPS)
