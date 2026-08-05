CC ?= cc
TCC_BIN ?=
TCC_EXT ?= 1
TCC_INC ?= ../third_party/tcc
TCC_LIB ?= ../third_party/tcc
CC_ENABLE_ASYNC ?= 1
CC_ENABLE_TLS ?= 0
CC_RUNTIME_V3 ?= 1
CC_V3_DIAGNOSTICS ?= 0
BEARSSL_DIR ?= ../third_party/bearssl

# Host TinyCC self-build of ccc (Linux). Darwin cannot: host tcc emits ELF
# while the in-tree libtcc.a is Mach-O. Detect by basename or */tcc path.
CC_IS_TCC := 0
ifneq ($(filter tcc,$(notdir $(CC))),)
  CC_IS_TCC := 1
endif
ifeq ($(CC_IS_TCC),0)
  ifneq ($(filter %/tcc,$(CC)),)
    CC_IS_TCC := 1
  endif
endif
ifeq ($(CC_IS_TCC),1)
  ifeq ($(shell uname -s),Darwin)
    $(error Host CC=tcc cannot self-build ccc on Darwin (TCC emits ELF; libtcc.a is Mach-O). Use Linux ILP32: CCC_HOST_CC=tcc ./scripts/smoke_i386.sh)
  endif
endif

CFLAGS_COMMON := -Wall -Wextra -I../out/include -Isrc -Iinclude -DCC_RUNTIME_V3=$(CC_RUNTIME_V3) -DCC_V3_DIAGNOSTICS=$(CC_V3_DIAGNOSTICS)
# Comptime hook dylibs are built with unresolved references into the ccc
# binary's embedded runtime (macOS: -undefined dynamic_lookup). On ELF
# platforms those symbols only resolve at dlopen time if the executable
# exports them, so link with --export-dynamic there.
ifneq ($(shell uname -s),Darwin)
  LDFLAGS += -Wl,--export-dynamic
  # ILP32: plain readdir() returns EOVERFLOW on filesystems with 64-bit
  # inodes (Docker volumes, many modern ext4 layouts). Harmless on LP64.
  CFLAGS_COMMON += -D_FILE_OFFSET_BITS=64
endif
ifeq ($(CC_ENABLE_ASYNC),1)
  CFLAGS_COMMON += -DCC_ENABLE_ASYNC
endif
ifeq ($(CC_ENABLE_TLS),1)
  CFLAGS_COMMON += -DCC_ENABLE_TLS -I$(BEARSSL_DIR)/inc
  LDLIBS += $(BEARSSL_DIR)/build/libbearssl.a
endif
ifeq ($(BUILD),debug)
  CFLAGS := $(CFLAGS_COMMON) -O0 -g
else
  CFLAGS := $(CFLAGS_COMMON) -O2 -ffunction-sections -fdata-sections
endif
# UFCS requires the patched TCC extension (AST hooks). Enforce TCC_EXT=1.
ifneq ($(TCC_EXT),1)
  $(error TCC_EXT must be 1 (patched TCC extension required for UFCS); set TCC_EXT=1 and ensure TCC_INC/TCC_LIB point to the patched tcc)
endif

# TCC extension (set TCC_EXT=1 and point TCC_INC/TCC_LIB at patched tcc)
ifeq ($(TCC_EXT),1)
  # Normalize paths (allow invoking from repo root with TCC_INC=third_party/tcc).
  TCC_INC_ABS := $(shell cd $(TCC_INC) 2>/dev/null && pwd)
  ifeq ($(TCC_INC_ABS),)
    TCC_INC_ABS := $(shell cd ../$(TCC_INC) 2>/dev/null && pwd)
  endif
  ifneq ($(TCC_INC_ABS),)
    override TCC_INC := $(TCC_INC_ABS)
  endif
  CFLAGS += -DCC_TCC_EXT_AVAILABLE -DCONFIG_CC_EXT -I$(TCC_INC)
  # Absolute dir holding libtcc1.a, so the in-process constexpr seam
  # (comptime/const_eval.c) can point TCC at its runtime support lib for
  # TCC_OUTPUT_MEMORY relocation regardless of cwd.
  CFLAGS += -DCC_TCC_LIB_DIR='"$(TCC_INC)"'
  # If TCC_LIB points to a directory, link -Ldir -ltcc; if it points to an
  # archive, link that archive directly.
  ifneq ($(suffix $(TCC_LIB)),.a)
    LDFLAGS += -L$(TCC_LIB)
    LDLIBS  += -ltcc
    # Track libtcc.a as dependency - relink if TCC is rebuilt
    TCC_LIB_DEP := $(TCC_LIB)/libtcc.a
  else
    LDLIBS  += $(TCC_LIB)
    TCC_LIB_DEP := $(TCC_LIB)
  endif
endif

# TinyCC host: C11 mode, -B for libtcc1.a, neutralize unimplemented
# _Thread_local (ccc frontend TLS is process-scoped under TINYC — ccc is not
# multi-threaded across those sites).
ifeq ($(CC_IS_TCC),1)
  CFLAGS += -std=c11 -D_Thread_local= -D__thread=
  # liblfds has no TCC port (needs GCC __atomic_* / OS PAL).
  CFLAGS += -DCC_NO_LIBLFDS
  ifneq ($(TCC_INC_ABS),)
    CFLAGS += -B$(TCC_INC_ABS)
  else
    CFLAGS += -B$(TCC_INC)
  endif
  # Match typical gcc/clang link sets for pthread/dl users in the runtime TU.
  LDLIBS += -lm -ldl -lpthread
endif

# Compile runtime as a single translation unit for easy linking. concurrent_c.c pulls in channel/nursery/scheduler/fiber_sched/fiber/io/string/exec and async shards when CC_ENABLE_ASYNC.
# Note: src/tools/ is excluded - those are standalone utilities with their own main()
SRCS := $(filter-out runtime/io_async.c runtime/channel.c runtime/nursery.c runtime/scheduler.c runtime/fiber_sched.c runtime/fiber.c runtime/async_chan.c runtime/async_runtime.c runtime/async_backend_poll.c runtime/exec.c src/tools/% src/preprocess/grammar_stub.c,$(wildcard src/*.c src/*/*.c)) runtime/concurrent_c.c
# Keep our local bridge even when TCC_EXT=1; it adapts libtcc hooks into CC's AST handle.
ifeq ($(CC_IS_TCC),1)
  OBJDIR ?= ../out/cc-tcc/obj
  OUT_BINDIR ?= ../out/cc-tcc/bin
else
  OBJDIR ?= ../out/cc/obj
  OUT_BINDIR ?= ../out/cc/bin
endif
OBJS := $(SRCS:%.c=$(OBJDIR)/%.o)
RUNTIME_CORE_OBJ := $(OBJDIR)/runtime/concurrent_c.core.o
RUNTIME_FLOAT_OBJ := $(OBJDIR)/runtime/float_format_zmij.o
DEPS := $(OBJS:.o=.d) $(RUNTIME_CORE_OBJ:.o=.d) $(RUNTIME_FLOAT_OBJ:.o=.d)
REAL_BINARY := bin/.ccc-bin
CCC_SCRIPT  := bin/ccc
CCC_WRAPPER_TEMPLATE := scripts/ccc_wrapper.sh
WRAPPER := $(OUT_BINDIR)/ccc
SHADOW_LOWER_TEMPLATE := scripts/shadow_lower.sh
SHADOW_LOWER_SCRIPT := bin/shadow_lower
SHADOW_LOWER_WRAPPER := $(OUT_BINDIR)/shadow_lower
# Native shadow_lower (libtcc-linked) + host-built runtime dylib for --exe.
SHADOW_LOWER_CCS := ../cc/shadow/shadow_lower.ccs
SHADOW_BUILD_CCH := ../cc/shadow/shadow_build.cch
SHADOW_TCC_C := ../cc/shadow/shadow_tcc_compile.c
SHADOW_TCC_OBJ := $(OBJDIR)/shadow_tcc_compile.o
SHADOW_COMPTIME_C := ../cc/shadow/shadow_comptime.c
SHADOW_COMPTIME_OBJ := $(OBJDIR)/shadow_comptime.o
OUT_LIBDIR := $(dir $(OUT_BINDIR))lib
ifeq ($(shell uname -s),Darwin)
  RUNTIME_DYLIB := $(OUT_LIBDIR)/libcc_runtime.dylib
  RUNTIME_DYLIB_LINK := -dynamiclib
else
  RUNTIME_DYLIB := $(OUT_LIBDIR)/libcc_runtime.so
  RUNTIME_DYLIB_LINK := -shared
endif

# The runtime object is linked into RUNTIME_DYLIB as well as the binary, and
# ELF rejects a thread-local relocation in a shared object built from non-PIC
# code (`R_X86_64_TPOFF32 ... recompile with -fPIC`).  Mach-O does not, which
# is why this only shows up on Linux.
ifneq ($(shell uname -s),Darwin)
  RUNTIME_PIC := -fPIC
endif

LOWER_HEADERS_SRCS := src/tools/lower_headers.c \
	src/header/lower_header.c \
	src/ir/ir.c \
	src/ir/verifier.c \
	src/preprocess/preprocess.c \
	src/preprocess/script_entry.c \
	src/preprocess/script_oneliner.c \
	src/preprocess/template_scan.c \
	src/preprocess/comptime_prepare.c \
	src/preprocess/grammar_seam.c \
	src/preprocess/grammar_rules.c \
	src/preprocess/grammar_cli.c \
	src/preprocess/cpp_expand.c \
	src/preprocess/type_registry.c \
	src/preprocess/type_graph.c \
	src/preprocess/variant_lower.c \
	src/preprocess/emit_plan.c \
	src/preprocess/strswitch_comptime.c \
	src/parser/symsig.c \
	src/result_spec.c \
	runtime/arena_state.c \
	src/comptime/const_eval.c \
	src/comptime/executor.c \
	src/comptime/hook_compile.c \
	src/build/host_cc_profile.c \
	src/comptime/symbols.c \
	src/visitor/pass_type_syntax.c \
	src/visitor/pass_channel_syntax.c \
	src/visitor/errhandler_lookup.c \
	src/visitor/pass_err_syntax.c \
	src/visitor/pass_result_unwrap.c \
	src/visitor/pass_unwrap_destroy.c \
	src/visitor/visitor_fileutil.c \
	src/util/cache_evict.c \
	src/util/io.c \
	src/util/path.c \
	src/util/result_fn_registry.c
LOWER_HEADERS_OBJS := $(LOWER_HEADERS_SRCS:%.c=$(OBJDIR)/%.o)
# Same engine surface as lower_headers (minus its main), for prepare/exec/splice.
# Omit arena_state.o: it is the FFC_IMPL TU and also unity-included by
# concurrent_c.o. Archiving it here and linking concurrent_c.o on the same
# line is an ODR hard-error under GNU ld -fno-common (Linux/gcc). shadow_lower
# always links concurrent_c.o for those symbols instead.
SHADOW_COMPTIME_ENGINE_OBJS := $(filter-out \
	$(OBJDIR)/src/tools/lower_headers.o \
	$(OBJDIR)/runtime/arena_state.o,$(LOWER_HEADERS_OBJS))
SHADOW_COMPTIME_LIB := $(OBJDIR)/libshadow_comptime.a
LOWER_HEADERS_BINARY := $(OUT_BINDIR)/lower_headers
# Stage-1 bootstrap variant: grammar engines stubbed (see BOOTSTRAP SHAPE)
# Stage-1: stub grammar_rules (no engines) and drop grammar_cli.o (defined there).
LOWER_HEADERS1_SRCS := $(filter-out src/preprocess/grammar_cli.c,$(subst src/preprocess/grammar_rules.c,src/preprocess/grammar_stub.c,$(LOWER_HEADERS_SRCS)))
LOWER_HEADERS1_OBJS := $(LOWER_HEADERS1_SRCS:%.c=$(OBJDIR)/%.o)
LOWER_HEADERS1_BINARY := $(OUT_BINDIR)/lower_headers_stage1
LOWER_HEADERS_INPUTS := $(shell find include -name '*.cch' -type f)

# Lowered headers output directory
OUT_INCLUDE := ../out/include
OUT_RUNTIME := ../out/runtime
LOWER_HEADERS_STAMP := $(OUT_INCLUDE)/.headers_lowered.stamp

all: $(CCC_SCRIPT) $(WRAPPER) $(SHADOW_LOWER_WRAPPER) $(RUNTIME_DYLIB) $(LOWER_HEADERS_STAMP)

# Patched-TCC guard: a fresh clone (or a changed patch set) gets the CC
# hooks applied into the submodule working tree BEFORE any CC object that
# includes tcc.h compiles (order-only prereq on the %.o rule), and libtcc
# rebuilt before anything links against it. Idempotent (the apply script
# reverse-checks). NOTE: placed AFTER `all` — an earlier revision sat
# before it and became make's default goal, silently building nothing.
TCC_PATCH_STAMP := $(TCC_INC)/.cc_patches_applied
# The stamp keys on the patch set AND the patched tree files: a submodule
# checkout/reset rewrites the tree (fresh mtimes) without touching the patch,
# and without these prereqs make would happily link a half-patched libtcc
# against CC objects built for the patched layout (audit finding).  The apply
# script is idempotent and touches the stamp last, so a completed apply is
# newer than every prereq and the rule stays quiescent.
$(TCC_PATCH_STAMP): $(wildcard ../third_party/tcc-patches/000*.patch) \
                    $(wildcard $(TCC_INC)/tcc.h $(TCC_INC)/tccgen.c \
                               $(TCC_INC)/tccpp.c $(TCC_INC)/libtcc.c \
                               $(TCC_INC)/cc_ast_record.h)
	@cd .. && bash scripts/apply_tcc_patches.sh
	@cd $(TCC_INC) && ([ -f config.mak ] || ./configure) && \
	  (grep -q CONFIG_cc_ext config.mak || echo 'CONFIG_cc_ext=yes' >> config.mak)
	@touch $@
ifneq ($(suffix $(TCC_LIB)),.a)
$(TCC_LIB)/libtcc.a: $(TCC_PATCH_STAMP)
	@cd $(TCC_LIB) && $(MAKE) libtcc.a libtcc1.a
endif

# Lower .cch headers to .h files (must run before examples that use generated C)
.PHONY: clean-cache lower-headers
lower-headers: $(LOWER_HEADERS_STAMP)

# The stamp uses the STAGE-1 tool (grammar stubbed): stdlib headers contain
# no @grammar, and stage-1 is buildable from raw .cch includes alone — which
# makes the whole graph acyclic and truly clean-buildable.  (Previously the
# stamp needed the FULL tool while several of its objects latently needed
# the stamp's output; clean trees only built by ordering luck.)
$(LOWER_HEADERS_STAMP): $(LOWER_HEADERS1_BINARY) $(LOWER_HEADERS_INPUTS) $(wildcard runtime/*.c runtime/*.h)
	@mkdir -p $(OUT_INCLUDE) $(OUT_RUNTIME)
	@$(LOWER_HEADERS1_BINARY) include $(OUT_INCLUDE) --rewrite-includes runtime $(OUT_RUNTIME)
	@touch $@

# CC-authored compiler internals: src/preprocess/emit/*.cch are ENGINE
# sources written in user CC (@string templates, CCString, arenas), lowered
# by the SAME shared passes as every user header and compiled into ccc.
# Engine source is user CC — a lowering gap is a bug, not a boundary.
#
# BOOTSTRAP SHAPE (two-stage; nothing generated is checked in): the full
# lower_headers links grammar_rules.c, whose emitter text is itself the
# CC being lowered — so a STAGE-1 lower_headers links grammar_stub.c
# instead (five seam symbols; @grammar hard-errors, which is also the
# guard that engine sources never declare grammars).  Stage 1 lowers
# src/preprocess/emit/*.cch into $(ENGINE_GEN_DIR); the real
# grammar_rules.o compiles against that; the full tool and ccc follow.
# The cost is one extra tool link, paid once per install/update.
ENGINE_CCH_DIR := src/preprocess/emit
ENGINE_CCH := $(wildcard $(ENGINE_CCH_DIR)/*.cch)
ENGINE_GEN_DIR := ../out/cc/genh
ENGINE_GEN_HDRS := $(patsubst $(ENGINE_CCH_DIR)/%.cch,$(ENGINE_GEN_DIR)/%.h,$(ENGINE_CCH))

$(LOWER_HEADERS1_BINARY): $(LOWER_HEADERS1_OBJS) $(TCC_LIB_DEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LOWER_HEADERS1_OBJS) $(LDLIBS)

$(ENGINE_GEN_DIR)/%.h: $(ENGINE_CCH_DIR)/%.cch $(LOWER_HEADERS1_BINARY)
	@mkdir -p $(ENGINE_GEN_DIR)
	@$(LOWER_HEADERS1_BINARY) $(ENGINE_CCH_DIR) $(ENGINE_GEN_DIR) > /dev/null

.PHONY: engine-headers
engine-headers: $(ENGINE_GEN_HDRS)

# The full tool must agree with stage 1 on the engine headers (it only
# wouldn't if an engine .cch tripped the stage-1 stub, which hard-errors).
.PHONY: engine-headers-verify
engine-headers-verify: $(ENGINE_GEN_HDRS) $(LOWER_HEADERS_BINARY)
	@rm -rf $(ENGINE_GEN_DIR).verify && mkdir -p $(ENGINE_GEN_DIR).verify
	@$(LOWER_HEADERS_BINARY) $(ENGINE_CCH_DIR) $(ENGINE_GEN_DIR).verify > /dev/null
	@diff -r $(ENGINE_GEN_DIR) $(ENGINE_GEN_DIR).verify && echo "engine headers: stage-1 == full tool"

$(OBJDIR)/src/preprocess/grammar_rules.o: $(ENGINE_GEN_HDRS)
$(OBJDIR)/src/preprocess/grammar_rules.o: CFLAGS += -I$(ENGINE_GEN_DIR)
# The engine headers #include <ccc/*.h> from $(OUT_INCLUDE), which only the
# lower-headers stamp produces — without this prereq a CLEAN tree fails
# ("ccc/cc_arena.h: No such file") and incremental trees build by ordering
# luck only.  Still acyclic: stage-1 → stamp → grammar_rules.o → full tool.
$(OBJDIR)/src/preprocess/grammar_rules.o: $(LOWER_HEADERS_STAMP)

.PHONY: test-build-stub
test-build-stub: $(WRAPPER)
	@echo "Running build stub test..."
	@sh ../examples/build_stub/test-build-stub.sh

$(REAL_BINARY): $(OBJS) $(TCC_LIB_DEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# cc/bin/ccc is now a shell wrapper that auto-lowers .cch headers on demand
# before dispatching to the real compiled binary (cc/bin/.ccc-bin).  This
# ensures downstream builds (real_projects/*, tests, perf scripts) that call
# cc/bin/ccc directly never see a stale out/include/*.h after a .cch edit.
$(CCC_SCRIPT): $(REAL_BINARY) $(CCC_WRAPPER_TEMPLATE) $(LOWER_HEADERS_BINARY)
	@mkdir -p $(dir $@)
	@cp $(CCC_WRAPPER_TEMPLATE) $@
	@chmod +x $@

# Legacy out/cc/bin/ccc path: keep it as a thin delegator to cc/bin/ccc so
# the smart-wrapper logic is inherited transitively.  Several perf scripts
# and real_projects Makefiles still hard-code this path.
$(WRAPPER): $(CCC_SCRIPT)
	@mkdir -p $(dir $@)
	@printf '%s\n' '#!/bin/sh' > $@
	@printf '%s\n' 'set -e' >> $@
	@printf '%s\n' 'SELF_DIR="$$(cd "$$(dirname "$$0")" && pwd)"' >> $@
	@printf '%s\n' 'exec "$$SELF_DIR/../../../cc/bin/ccc" "$$@"' >> $@
	@chmod +x $@

# Runtime shared lib for shadow --exe (libtcc cannot link host clang .o on Darwin).
# Leaf soname only — run with DYLD_LIBRARY_PATH / LD_LIBRARY_PATH = this dir.
# Host TCC needs CFLAGS (-B) so -shared finds libtcc1.a.
$(RUNTIME_DYLIB): $(OBJDIR)/runtime/concurrent_c.o
	@mkdir -p $(dir $@)
ifeq ($(shell uname -s),Darwin)
	$(CC) $(CFLAGS) $(RUNTIME_DYLIB_LINK) -install_name libcc_runtime.dylib -o $@ $< -lpthread -lm
else
	$(CC) $(CFLAGS) $(RUNTIME_DYLIB_LINK) -o $@ $< -lpthread -lm
endif

$(SHADOW_TCC_OBJ): $(SHADOW_TCC_C) $(TCC_LIB_DEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I../cc/shadow -c $(SHADOW_TCC_C) -o $@

$(SHADOW_COMPTIME_OBJ): $(SHADOW_COMPTIME_C) $(TCC_LIB_DEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I../cc/shadow -Isrc -c $(SHADOW_COMPTIME_C) -o $@

$(SHADOW_COMPTIME_LIB): $(SHADOW_COMPTIME_OBJ) $(SHADOW_COMPTIME_ENGINE_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	ar rcs $@ $^

# shadow_lower self-build:
#   SHADOW_LOWER_SOURCE=bootstrap (default) — host-cc last-good snapshot
#   SHADOW_LOWER_SOURCE=ccs — re-lower .ccs via existing native shadow_lower
# Promote a new seed: scripts/snapshot_shadow_lower.sh && promote_shadow_bootstrap.sh
BOOTSTRAP_ROOT := bootstrap/shadow_lower
BOOTSTRAP_VER := $(shell cat $(BOOTSTRAP_ROOT)/last-good 2>/dev/null)
BOOTSTRAP_DIR := $(BOOTSTRAP_ROOT)/$(BOOTSTRAP_VER)
SHADOW_LOWER_SOURCE ?= bootstrap
# Prefer a previously built native binary when re-lowering from .ccs.
SHADOW_LOWER_SEED_BIN := $(firstword \
	$(wildcard $(OUT_BINDIR)/shadow_lower) \
	$(wildcard bin/shadow_lower))

SHADOW_LOWER_CCS_DEPS := $(SHADOW_LOWER_CCS) $(SHADOW_BUILD_CCH) \
	$(wildcard ../cc/shadow/pp_*.cch) \
	$(wildcard ../cc/shadow/shadow_*.cch) \
	$(wildcard ../cc/shadow/shadow_*.h)
SHADOW_LOWER_BOOT_DEPS := $(BOOTSTRAP_DIR)/shadow_lower.c \
	$(wildcard $(BOOTSTRAP_DIR)/include/*.h)

ifeq ($(SHADOW_LOWER_SOURCE),ccs)
$(SHADOW_LOWER_WRAPPER): $(SHADOW_LOWER_CCS_DEPS) $(SHADOW_TCC_OBJ) $(SHADOW_COMPTIME_LIB) \
	$(TCC_LIB_DEP) $(RUNTIME_DYLIB) $(OBJDIR)/runtime/concurrent_c.o
	@mkdir -p $(dir $@) $(OBJDIR)
	@test -n "$(SHADOW_LOWER_SEED_BIN)" -a -x "$(SHADOW_LOWER_SEED_BIN)" || \
		(echo "SHADOW_LOWER_SOURCE=ccs needs an existing shadow_lower; run: make SHADOW_LOWER_SOURCE=bootstrap"; exit 1)
	# Native self-emit, then host-cc link (same objs as bootstrap).
	# concurrent_c.o supplies arena_state/FFC (omitted from libshadow_comptime.a).
	$(SHADOW_LOWER_SEED_BIN) $(SHADOW_LOWER_CCS) -o $(OBJDIR)/shadow_lower.emit.c --no-cache \
		--cc-flags="-I../cc/shadow -I$(TCC_INC) -DSHADOW_HAVE_LIBTCC=1"
	@if grep -qE '^de#line' $(OBJDIR)/shadow_lower.emit.c; then \
		echo "error: emit contains de#line (injected-token lead bug)"; exit 1; \
	fi
	$(CC) $(CFLAGS) -o $@ $(OBJDIR)/shadow_lower.emit.c \
		-I../out/include/cc/shadow -I../out/include -I../cc/include \
		-I../cc/shadow -I$(TCC_INC) -DSHADOW_HAVE_LIBTCC=1 \
		$(abspath $(SHADOW_TCC_OBJ)) \
		$(abspath $(OBJDIR)/runtime/concurrent_c.o) \
		$(abspath $(SHADOW_COMPTIME_LIB)) \
		-L$(TCC_LIB) -ltcc $(LDFLAGS) $(LDLIBS)
	@mkdir -p $(dir $(SHADOW_LOWER_SCRIPT))
	@cp $@ $(SHADOW_LOWER_SCRIPT)
	@chmod +x $(SHADOW_LOWER_SCRIPT)
else
$(SHADOW_LOWER_WRAPPER): $(SHADOW_LOWER_BOOT_DEPS) $(SHADOW_TCC_OBJ) $(SHADOW_COMPTIME_LIB) \
	$(TCC_LIB_DEP) $(OBJDIR)/runtime/concurrent_c.o $(LOWER_HEADERS_STAMP)
	@test -n "$(BOOTSTRAP_VER)" || (echo "missing $(BOOTSTRAP_ROOT)/last-good"; exit 1)
	@test -f "$(BOOTSTRAP_DIR)/shadow_lower.c" || \
		(echo "missing $(BOOTSTRAP_DIR)/shadow_lower.c; run snapshot+promote"; exit 1)
	@mkdir -p $(dir $@)
	# Seed snapshot headers at angle-include paths some emits still spell
	# (<cc/shadow/*.h>, legacy <examples/serdes/c/*.h>). v4+ rewrites those
	# to "foo.h" + -Iinclude/; seeding keeps older snapshots host-cc clean.
	@mkdir -p ../out/include/cc/shadow ../out/include/examples/serdes/c
	@if ls $(BOOTSTRAP_DIR)/include/*.h >/dev/null 2>&1; then \
		cp -f $(BOOTSTRAP_DIR)/include/*.h ../out/include/cc/shadow/; \
		cp -f $(BOOTSTRAP_DIR)/include/*.h ../out/include/examples/serdes/c/; \
	fi
	# arena_state/FFC live only in concurrent_c.o (see SHADOW_COMPTIME_ENGINE_OBJS).
	$(CC) $(CFLAGS) -o $@ $(BOOTSTRAP_DIR)/shadow_lower.c \
		-I$(BOOTSTRAP_DIR)/include -Iinclude -I../out/include \
		-I../cc/shadow -I$(TCC_INC) -DSHADOW_HAVE_LIBTCC=1 \
		$(abspath $(SHADOW_TCC_OBJ)) \
		$(abspath $(OBJDIR)/runtime/concurrent_c.o) \
		$(abspath $(SHADOW_COMPTIME_LIB)) \
		$(LDFLAGS) $(LDLIBS)
	@mkdir -p $(dir $(SHADOW_LOWER_SCRIPT))
	@cp $@ $(SHADOW_LOWER_SCRIPT)
	@chmod +x $(SHADOW_LOWER_SCRIPT)
	@echo "built $@ from bootstrap $(BOOTSTRAP_VER)"
endif

.PHONY: shadow_lower-from-bootstrap shadow_lower-from-ccs
shadow_lower-from-bootstrap:
	$(MAKE) SHADOW_LOWER_SOURCE=bootstrap $(SHADOW_LOWER_WRAPPER)
shadow_lower-from-ccs:
	$(MAKE) SHADOW_LOWER_SOURCE=ccs $(SHADOW_LOWER_WRAPPER)

# Dev fallback wrapper (ccc run) — not installed as the primary CLI.
$(SHADOW_LOWER_SCRIPT).sh: $(SHADOW_LOWER_TEMPLATE)
	@mkdir -p $(dir $@)
	@cp $(SHADOW_LOWER_TEMPLATE) $@
	@chmod +x $@

$(LOWER_HEADERS_BINARY): $(LOWER_HEADERS_OBJS) $(TCC_LIB_DEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LOWER_HEADERS_OBJS) $(LDLIBS)

$(OBJDIR)/runtime/concurrent_c.o: $(LOWER_HEADERS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(RUNTIME_PIC) -MMD -MP -MF $(RUNTIME_CORE_OBJ:.o=.d) -c $(OUT_RUNTIME)/concurrent_c.c -o $(RUNTIME_CORE_OBJ)
	$(CC) $(CFLAGS) $(RUNTIME_PIC) -MMD -MP -MF $(RUNTIME_FLOAT_OBJ:.o=.d) -c runtime/float_format_zmij.c -o $(RUNTIME_FLOAT_OBJ)
	$(CC) -nostdlib -r -o $@ $(RUNTIME_CORE_OBJ) $(RUNTIME_FLOAT_OBJ)

$(OBJDIR)/%.o: %.c | $(TCC_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

clean-cache:
	rm -rf $(HOME)/.cache/concurrent-c/incexp $(HOME)/.cache/concurrent-c/comptime-hooks
	rm -rf $(CURDIR)/../out/ccc-cache

clean:
	rm -f $(WRAPPER) $(REAL_BINARY) $(LOWER_HEADERS_BINARY)
	rm -rf $(OBJDIR)
	rm -rf $(OUT_INCLUDE)
	rm -rf $(OUT_RUNTIME)
	# Clean legacy in-tree objects from earlier layouts.
	@rm -f $(SRCS:.c=.o)
	@rm -f bin/cc ../out/cc/bin/cc

EMIT_TPL_PRELUDE := src/comptime/emit_tpl_prelude.inc.h
EMIT_TPL_CORE := include/ccc/cc_emit_tpl_core.inc.cch
GEN_EMIT_TPL := ../tools/gen_emit_tpl_prelude.sh

$(EMIT_TPL_PRELUDE): $(GEN_EMIT_TPL) $(EMIT_TPL_CORE)
	$(GEN_EMIT_TPL)

$(OBJDIR)/src/comptime/executor.o: $(EMIT_TPL_PRELUDE)
$(OBJDIR)/src/comptime/hook_compile.o: $(EMIT_TPL_PRELUDE)

.PHONY: all clean

# Auto-generated dependency files for objects (includes runtime/concurrent_c.c -> runtime/*.c).
-include $(DEPS)
