# ODFileSystem — Amiga optical filesystem
# SPDX-License-Identifier: BSD-2-Clause
#
# Makefile for host and Amiga cross-compilation builds

# ---- toolchain selection ----

# Amiga cross-compiler.
# Override with CC=ppc-amigaos-gcc for an AmigaOS 4 PPC build.
ifeq ($(origin CC),default)
CC      = m68k-amigaos-gcc
endif

# AROS cross-compiler (override: make CC=m68k-aros-gcc AROS=1)
# When AROS=1, uses -static instead of -noixemul and defines __AROS__
AROS ?= 0

# Derive target tools from CC so CC=ppc-amigaos-gcc also selects the
# matching ppc-amigaos-ar/strip/size tools.
AMIGA_CC_TARGET   := $(shell $(CC) -dumpmachine 2>/dev/null)
AMIGA_TOOL_PREFIX ?= $(patsubst %-gcc,%,$(notdir $(CC)))
AMIGA_AR          ?= $(AMIGA_TOOL_PREFIX)-ar
AMIGA_SIZE        ?= $(AMIGA_TOOL_PREFIX)-size
AMIGA_OBJCOPY     ?= $(AMIGA_TOOL_PREFIX)-objcopy
STRIP             ?= $(AMIGA_TOOL_PREFIX)-strip

ifneq ($(filter ppc-amigaos,$(AMIGA_CC_TARGET)),)
AMIGA_TARGET ?= os4
else ifeq ($(AROS),1)
AMIGA_TARGET ?= aros
else
AMIGA_TARGET ?= os3
endif

ifeq ($(AMIGA_TARGET),os4)
AMIGA_OSDIR := os4
else
AMIGA_OSDIR := os3
endif

# NDK include path (override with: make NDK_PATH=/your/path)
ifeq ($(AMIGA_TARGET),os4)
NDK_PATH ?= $(shell realpath $$(dirname $$(which $(CC)))/../ppc-amigaos/SDK/include/include_h 2>/dev/null)
else
NDK_PATH ?= $(shell realpath $$(dirname $$(which $(CC)))/../m68k-amigaos/ndk-include 2>/dev/null)
endif

# Host compiler
HOSTCC ?= cc

# ---- common flags ----

AMIGA_DATE ?= $(shell date '+%-d.%-m.%Y')
ODFS_GIT_VERSION ?= $(shell desc=$$(git describe --tags --match "v*" --dirty --always 2>/dev/null || echo unknown); printf '%s\n' "$$desc" | grep -q '^v' && printf '%s' "$$desc" || printf 'early-0-g%s' "$$desc")

INCLUDES = -I include -I backends
AMIGA_PLATFORM_INCLUDES = -I platform/amiga \
                          -I platform/amiga/common \
                          -I platform/amiga/$(AMIGA_OSDIR)
AMIGA_INCLUDES = $(INCLUDES) $(AMIGA_PLATFORM_INCLUDES) \
                 $(if $(NDK_PATH),-I$(NDK_PATH))

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

# Optimization level. Speed is the release priority; the ROM profile
# overrides this with -Os to stay within its size budget.
AMIGA_OPT ?= -O2

# Serial debug output (override with: make SERIAL_DEBUG=1)
SERIAL_DEBUG ?= 0

# Packet trace instrumentation (override with: make PACKET_TRACE=1)
PACKET_TRACE ?= 0

# Release size limits (override when intentional growth is approved)
ifeq ($(AMIGA_TARGET),os4)
AMIGA_SIZE_LIMIT ?= 147456
else
AMIGA_SIZE_LIMIT ?= 98304
endif
ROM_SIZE_LIMIT   ?= 40960
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

ifeq ($(AMIGA_TARGET),aros)
AMIGA_CPUFLAGS ?= -m68000 -mtune=68020-60 -msoft-float
AMIGA_SYSFLAGS ?= -static
AMIGA_WARNFLAGS =
AMIGA_DEFS     = -DAMIGA -D__AROS__
LDFLAGS        = $(AMIGA_SYSFLAGS) $(LTO)
LIBS           = -lamiga -lgcc
HANDLER_LDFLAGS = -nostartfiles
HANDLER_LIBS   = -nostdlib -Wl,-u,_exit -lgcc -lc -lgcc -lamiga -ramiga-dev
else ifeq ($(AMIGA_TARGET),os4)
AMIGA_CRT      ?= newlib
AMIGA_CPUFLAGS ?= -mcpu=powerpc
# No unwind tables: the freestanding handler has no exception support,
# and the kickstart loader expects plain PT_LOAD program headers only.
AMIGA_SYSFLAGS ?= -mcrt=$(AMIGA_CRT) -fno-asynchronous-unwind-tables
AMIGA_WARNFLAGS =
AMIGA_DEFS     = -DAMIGA -D__USE_INLINE__ -D__USE_BASETYPE__
LDFLAGS        = $(AMIGA_SYSFLAGS) $(LTO)
# Keep OS4 library/interface ownership explicit in os4/sys_compat.c.
# Do not add -lauto to the handler link.
LIBS           = -lc -lgcc
# The handler must not run the newlib C runtime startup: it consumes
# the first process message, which is the handler's ACTION_STARTUP
# packet. os4/start.c provides the freestanding entry instead.
# -static keeps gcc from passing --eh-frame-hdr, so the binary carries
# only the plain PT_LOAD program headers the kickstart loader expects.
HANDLER_LDFLAGS = -nostartfiles -static -Wl,-u,_start
HANDLER_LIBS   = -nostdlib -lgcc
else
AMIGA_CPUFLAGS ?= -m68000 -mtune=68020-60 -msoft-float
AMIGA_SYSFLAGS ?= -noixemul
AMIGA_WARNFLAGS =
AMIGA_DEFS     = -DAMIGA
LDFLAGS        = $(AMIGA_SYSFLAGS) $(LTO)
LIBS           = -lamiga -lgcc
HANDLER_LDFLAGS = -nostartfiles
HANDLER_LIBS   = -nostdlib -Wl,-u,_exit -lgcc -lc -lgcc -lamiga -ramiga-dev
endif

CFLAGS = $(AMIGA_OPT) $(AMIGA_CPUFLAGS) $(AMIGA_SYSFLAGS) -nostartfiles \
         -Wall -Wextra -Werror \
         $(AMIGA_WARNFLAGS) \
         -Wstrict-prototypes -Wmissing-prototypes \
         -Wno-array-bounds \
         -MMD -MP \
         $(AMIGA_DEFS) $(FEATURE_DEFS) $(LTO)

# ---- build directories ----

HOST_BUILD  = build/host
AMIGA_BUILD = build/amiga
ROM_BUILD   = build/amiga-rom
AMIGA_TEST_BUILD = build/amiga-test
ROM_TEST_BUILD   = build/amiga-rom-test
AMIGA_020_BUILD  = build/amiga-020

# 68020 release variant: hardware mul/div and 020 addressing modes make
# the handler both faster and smaller than the 68000 build.
AMIGA_020_CPUFLAGS ?= -m68020 -msoft-float

# ---- shared source lists ----

# Core library (shared between host and Amiga)
CORE_SRCS = \
    core/error.c \
    core/log.c \
    core/node.c \
    core/namefix.c \
    core/cache_block.c \
    core/cache_meta.c \
    core/charset.c \
    core/ancestry.c \
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
    platform/amiga/printf_local.c \
    platform/amiga/$(AMIGA_OSDIR)/sys_compat.c
ifeq ($(AMIGA_TARGET),os4)
AMIGA_SRCS += platform/amiga/os4/main.c \
    platform/amiga/os4/start.c \
    platform/amiga/os4/freestanding.c \
    platform/amiga/os4/vector_port.c
else
AMIGA_SRCS += platform/amiga/libc_stubs.c
endif

# Freestanding libc replacements: stop the compiler from recognizing
# the copy loops and emitting calls to the functions being defined.
$(AMIGA_BUILD)/platform/amiga/os4/freestanding.o: CFLAGS += -fno-builtin

# Amiga assembly
ifeq ($(AMIGA_TARGET),os4)
AMIGA_ASM_SRCS =
else
AMIGA_ASM_SRCS = platform/amiga/startup.S
endif
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
HANDLER_020  = $(AMIGA_020_BUILD)/ODFileSystem
KICKSTART_MODULE =
AMIGA_ARTIFACTS = $(HANDLER)
ifeq ($(AMIGA_TARGET),os4)
KICKSTART_MODULE = $(AMIGA_BUILD)/CDFileSystem
AMIGA_ARTIFACTS += $(KICKSTART_MODULE)
endif
TEST_HANDLER = $(AMIGA_TEST_BUILD)/ODFileSystem
AMIGA_TEST_TOOL = $(AMIGA_TEST_BUILD)/test_handler
ADF          = $(AMIGA_TEST_BUILD)/ODFileSystem.adf
ADF_VOLUME   = ODFileSystem
ADF_DOSDRIVER      = platform/amiga/dosdrivers/CD0
ADF_DOSDRIVER_ICON = platform/amiga/dosdrivers/CD0.info
XDFTOOL      ?= xdftool
LHA          ?= lha
AMIGAOS3_PACKAGE_NAME ?= ODFileSystem
AMIGAOS3_PACKAGE     = $(AMIGA_BUILD)/$(AMIGAOS3_PACKAGE_NAME).lha
AMIGAOS3_PACKAGE_DIR = $(AMIGA_BUILD)/$(AMIGAOS3_PACKAGE_NAME)-lha
AMIGAOS3_README      = docs/ODFileSystem.readme
AMIGAOS4_PACKAGE_NAME ?= ODFileSystem-amigaos4
AMIGAOS4_PACKAGE     = $(AMIGA_BUILD)/$(AMIGAOS4_PACKAGE_NAME).lha
AMIGAOS4_PACKAGE_DIR = $(AMIGA_BUILD)/$(AMIGAOS4_PACKAGE_NAME)-lha
AMIGAOS4_README      = docs/ODFileSystem_OS4.readme

# ==================================================================
# targets
# ==================================================================

.PHONY: all host amiga amiga-test amiga-020 adf rom rom-test lib tests tools fuzz
.PHONY: check golden-check malformed-check fuzz-check integration-check
.PHONY: clean size amigaos3-lha amigaos4-lha

all: host

host: lib tests tools

amiga: $(AMIGA_ARTIFACTS)
	@echo "  $(HANDLER) built successfully"
	@size=$$(wc -c < "$(HANDLER)"); \
	echo "  Handler size: $$size bytes"; \
	if [ "$(ENFORCE_SIZE_LIMITS)" != "0" ] && [ "$$size" -gt "$(AMIGA_SIZE_LIMIT)" ]; then \
		echo "  ERROR: $(SIZE_LIMIT_DESC) exceeds $(AMIGA_SIZE_LIMIT) bytes"; \
		echo "  If this growth is intentional, rerun with $(SIZE_LIMIT_NAME)=<new-limit>"; \
		exit 1; \
	fi; \
	if [ -n "$(KICKSTART_MODULE)" ]; then \
		ksize=$$(wc -c < "$(KICKSTART_MODULE)"); \
		echo "  Kickstart module: $(KICKSTART_MODULE)"; \
		echo "  CDFileSystem size: $$ksize bytes"; \
	fi

amiga-test:
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(AMIGA_TEST_BUILD) \
		ENFORCE_SIZE_LIMITS=0 \
		SERIAL_DEBUG=1 \
		amiga

amiga-020:
	@if [ "$(AMIGA_TARGET)" != "os3" ]; then \
		echo "  ERROR: amiga-020 requires CC=m68k-amigaos-gcc"; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(AMIGA_020_BUILD) \
		AMIGA_CPUFLAGS="$(AMIGA_020_CPUFLAGS)" \
		SIZE_LIMIT_DESC="release Amiga 68020 handler" \
		amiga

adf: amiga-test amiga-020 $(AMIGA_TEST_TOOL) $(ADF_DOSDRIVER) $(ADF_DOSDRIVER_ICON) Makefile
	@mkdir -p $(dir $(ADF))
	@echo "  ADF   $(ADF)"
	@$(XDFTOOL) -f $(ADF) \
		create + \
		format "$(ADF_VOLUME)" + \
		makedir L + \
		makedir C + \
		write $(TEST_HANDLER) L + \
		write $(HANDLER_020) L/ODFileSystem020 + \
		write $(AMIGA_TEST_TOOL) C/test_handler + \
		write $(ADF_DOSDRIVER) + \
		write $(ADF_DOSDRIVER_ICON)
	@echo "  ADF image ready: $(ADF)"

amigaos3-lha:
	@if [ "$(AMIGA_TARGET)" != "os3" ]; then \
		echo "  ERROR: amigaos3-lha requires CC=m68k-amigaos-gcc"; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory AMIGA_BUILD=$(AMIGA_BUILD) amiga
	@$(MAKE) --no-print-directory AMIGA_020_BUILD=$(AMIGA_020_BUILD) \
		amiga-020
	@$(MAKE) --no-print-directory AMIGA_TEST_BUILD=$(AMIGA_TEST_BUILD) \
		amiga-test
	@$(MAKE) --no-print-directory ROM_BUILD=$(ROM_BUILD) rom
	@$(MAKE) --no-print-directory ROM_TEST_BUILD=$(ROM_TEST_BUILD) \
		rom-test
	@rm -rf "$(AMIGAOS3_PACKAGE_DIR)" "$(AMIGAOS3_PACKAGE)"
	@mkdir -p "$(AMIGAOS3_PACKAGE_DIR)"
	@cp "$(HANDLER)" "$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem"
	@cp "$(HANDLER_020)" "$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem020"
	@cp "$(TEST_HANDLER)" "$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem-test"
	@cp "$(ROM_BUILD)/ODFileSystem" \
		"$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem-rom"
	@cp "$(ROM_TEST_BUILD)/ODFileSystem" \
		"$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem-rom-test"
	@cp "$(AMIGAOS3_README)" "$(AMIGAOS3_PACKAGE_DIR)/README.md"
	@echo "  LHA   $(AMIGAOS3_PACKAGE)"
	@(cd "$(AMIGAOS3_PACKAGE_DIR)" && \
		$(LHA) -aq "$(CURDIR)/$(AMIGAOS3_PACKAGE)" \
		ODFileSystem ODFileSystem020 ODFileSystem-test \
		ODFileSystem-rom ODFileSystem-rom-test README.md)
	@echo "  LHA archive ready: $(AMIGAOS3_PACKAGE)"

amigaos4-lha:
	@if [ "$(AMIGA_TARGET)" != "os4" ]; then \
		echo "  ERROR: amigaos4-lha requires CC=ppc-amigaos-gcc"; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory AMIGA_BUILD=$(AMIGA_BUILD) amiga
	@$(MAKE) --no-print-directory AMIGA_TEST_BUILD=$(AMIGA_TEST_BUILD) \
		amiga-test
	@rm -rf "$(AMIGAOS4_PACKAGE_DIR)" "$(AMIGAOS4_PACKAGE)"
	@mkdir -p "$(AMIGAOS4_PACKAGE_DIR)"
	@cp "$(HANDLER)" "$(AMIGAOS4_PACKAGE_DIR)/ODFileSystem-amigaos4"
	@cp "$(KICKSTART_MODULE)" "$(AMIGAOS4_PACKAGE_DIR)/CDFileSystem"
	@cp "$(TEST_HANDLER)" \
		"$(AMIGAOS4_PACKAGE_DIR)/ODFileSystem-amigaos4-test"
	@cp "$(AMIGA_TEST_BUILD)/CDFileSystem" \
		"$(AMIGAOS4_PACKAGE_DIR)/CDFileSystem-test"
	@cp "$(AMIGAOS4_README)" "$(AMIGAOS4_PACKAGE_DIR)/README.md"
	@echo "  LHA   $(AMIGAOS4_PACKAGE)"
	@(cd "$(AMIGAOS4_PACKAGE_DIR)" && \
		$(LHA) -aq "$(CURDIR)/$(AMIGAOS4_PACKAGE)" \
		ODFileSystem-amigaos4 CDFileSystem \
		ODFileSystem-amigaos4-test CDFileSystem-test README.md)
	@echo "  LHA archive ready: $(AMIGAOS4_PACKAGE)"

# ROM profile: minimal build for burning into ROM
# ISO9660 + Rock Ridge + Joliet + Multisession, no debug, no UDF/HFS/CDDA
rom:
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(ROM_BUILD) \
		AMIGA_OPT=-Os \
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
		AMIGA_OPT=-Os \
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
	@$(AMIGA_SIZE) $(AMIGA_BUILD)/libodfs.a

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
	@$(AMIGA_AR) rcs $@ $^

# ---- Amiga object files ----

$(AMIGA_BUILD)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  CC    $<"
	@$(CC) $(CPPFLAGS) $(AMIGA_INCLUDES) $(CFLAGS) -c -o $@ $<

# ---- Amiga assembly ----

$(AMIGA_BUILD)/%.o: %.S
	@mkdir -p $(@D)
	@echo "  AS    $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# ---- Amiga test tools ----

$(AMIGA_TEST_BUILD)/tests/amiga/%.o: tests/amiga/%.c
	@mkdir -p $(@D)
	@echo "  CC    $<"
	@$(CC) $(CPPFLAGS) $(AMIGA_INCLUDES) $(CFLAGS) -c -o $@ $<

$(AMIGA_TEST_TOOL): $(AMIGA_TEST_BUILD)/tests/amiga/test_handler.o
	@mkdir -p $(@D)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) -o $@ $< $(LIBS)
	@echo "  STRIP $@"
	@$(STRIP) $@

# ---- Amiga handler ----

$(HANDLER): $(AMIGA_ASM_OBJS) $(AMIGA_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) $(HANDLER_LDFLAGS) -o $@ $(AMIGA_ASM_OBJS) -L$(AMIGA_BUILD) -lodfs $(HANDLER_LIBS)
	@echo "  STRIP $@"
	@$(STRIP) $@

ifeq ($(AMIGA_TARGET),os4)
$(KICKSTART_MODULE): $(AMIGA_BUILD)/platform/amiga/os4/start.o $(AMIGA_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  LINK  $@ (kickstart)"
	@$(CC) $(LDFLAGS) -nostartfiles -nostdlib -Wl,-r -o $@.unstripped $< -L$(AMIGA_BUILD) -lodfs -lgcc
	@echo "  STRIP $@"
	@$(AMIGA_OBJCOPY) --strip-unneeded $@.unstripped $@
	@rm -f $@.unstripped
endif


# ---- clean ----

clean:
	@echo "  CLEAN"
	@rm -rf build

-include $(HOST_LIB_DEPS) $(AMIGA_LIB_DEPS) $(AMIGA_ASM_DEPS) \
	$(TEST_DEPS) $(FUZZ_DEPS) $(TOOL_DEPS)
