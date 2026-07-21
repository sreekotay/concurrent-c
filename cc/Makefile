CC ?= cc
CXX ?= c++
TCC_BIN ?=
TCC_EXT ?= 1
TCC_INC ?= ../third_party/tcc
TCC_LIB ?= ../third_party/tcc
CC_ENABLE_ASYNC ?= 1
CC_ENABLE_TLS ?= 0
CC_RUNTIME_V3 ?= 1
CC_V3_DIAGNOSTICS ?= 0
BEARSSL_DIR ?= ../third_party/bearssl
CFLAGS_COMMON := -Wall -Wextra -I../out/include -Isrc -Iinclude -DCC_RUNTIME_V3=$(CC_RUNTIME_V3) -DCC_V3_DIAGNOSTICS=$(CC_V3_DIAGNOSTICS)
# Comptime hook dylibs are built with unresolved references into the ccc
# binary's embedded runtime (macOS: -undefined dynamic_lookup). On ELF
# platforms those symbols only resolve at dlopen time if the executable
# exports them, so link with --export-dynamic there.
ifneq ($(shell uname -s),Darwin)
  LDFLAGS += -Wl,--export-dynamic
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
  CXXFLAGS := $(CFLAGS_COMMON) -O0 -g -std=c++17 -fno-exceptions -fno-rtti
else
  CFLAGS := $(CFLAGS_COMMON) -O2 -ffunction-sections -fdata-sections
  CXXFLAGS := $(CFLAGS_COMMON) -O2 -ffunction-sections -fdata-sections -std=c++17 -fno-exceptions -fno-rtti
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

# Compile runtime as a single translation unit for easy linking. concurrent_c.c pulls in channel/nursery/scheduler/fiber_sched/fiber/io/string/exec and async shards when CC_ENABLE_ASYNC.
# Note: src/tools/ is excluded - those are standalone utilities with their own main()
SRCS := $(filter-out runtime/io_async.c runtime/channel.c runtime/nursery.c runtime/scheduler.c runtime/fiber_sched.c runtime/fiber.c runtime/async_chan.c runtime/async_runtime.c runtime/async_backend_poll.c runtime/exec.c src/tools/% src/preprocess/grammar_stub.c,$(wildcard src/*.c src/*/*.c)) runtime/concurrent_c.c
# Keep our local bridge even when TCC_EXT=1; it adapts libtcc hooks into CC's AST handle.
OBJDIR ?= ../out/cc/obj
OUT_BINDIR ?= ../out/cc/bin
OBJS := $(SRCS:%.c=$(OBJDIR)/%.o)
RUNTIME_CORE_OBJ := $(OBJDIR)/runtime/concurrent_c.core.o
RUNTIME_XJB_OBJ := $(OBJDIR)/runtime/float_format_xjb.o
DEPS := $(OBJS:.o=.d) $(RUNTIME_CORE_OBJ:.o=.d) $(RUNTIME_XJB_OBJ:.o=.d)
REAL_BINARY := bin/.ccc-bin
CCC_SCRIPT  := bin/ccc
CCC_WRAPPER_TEMPLATE := scripts/ccc_wrapper.sh
WRAPPER := $(OUT_BINDIR)/ccc

LOWER_HEADERS_SRCS := src/tools/lower_headers.c \
	src/header/lower_header.c \
	src/ir/ir.c \
	src/ir/verifier.c \
	src/preprocess/preprocess.c \
	src/preprocess/template_scan.c \
	src/preprocess/comptime_prepare.c \
	src/preprocess/grammar_seam.c \
	src/preprocess/grammar_rules.c \
	src/preprocess/cpp_expand.c \
	src/preprocess/type_registry.c \
	src/preprocess/type_graph.c \
	src/preprocess/emit_plan.c \
	src/result_spec.c \
	runtime/arena_state.c \
	src/comptime/const_eval.c \
	src/comptime/executor.c \
	src/comptime/hook_compile.c \
	src/comptime/symbols.c \
	src/visitor/pass_type_syntax.c \
	src/visitor/pass_channel_syntax.c \
	src/visitor/pass_err_syntax.c \
	src/visitor/pass_result_unwrap.c \
	src/visitor/pass_unwrap_destroy.c \
	src/visitor/visitor_fileutil.c \
	src/util/io.c \
	src/util/path.c \
	src/util/result_fn_registry.c
LOWER_HEADERS_OBJS := $(LOWER_HEADERS_SRCS:%.c=$(OBJDIR)/%.o)
LOWER_HEADERS_BINARY := $(OUT_BINDIR)/lower_headers
# Stage-1 bootstrap variant: grammar engines stubbed (see BOOTSTRAP SHAPE)
LOWER_HEADERS1_SRCS := $(subst src/preprocess/grammar_rules.c,src/preprocess/grammar_stub.c,$(LOWER_HEADERS_SRCS))
LOWER_HEADERS1_OBJS := $(LOWER_HEADERS1_SRCS:%.c=$(OBJDIR)/%.o)
LOWER_HEADERS1_BINARY := $(OUT_BINDIR)/lower_headers_stage1
LOWER_HEADERS_INPUTS := $(shell find include -name '*.cch' -type f)

# Lowered headers output directory
OUT_INCLUDE := ../out/include
OUT_RUNTIME := ../out/runtime
LOWER_HEADERS_STAMP := $(OUT_INCLUDE)/.headers_lowered.stamp

all: $(CCC_SCRIPT) $(WRAPPER) $(LOWER_HEADERS_STAMP)

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
.PHONY: lower-headers
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

$(LOWER_HEADERS_BINARY): $(LOWER_HEADERS_OBJS) $(TCC_LIB_DEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LOWER_HEADERS_OBJS) $(LDLIBS)

$(OBJDIR)/runtime/concurrent_c.o: $(LOWER_HEADERS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DCC_ENABLE_XJB_FLOAT_FMT=1 -MMD -MP -MF $(RUNTIME_CORE_OBJ:.o=.d) -c $(OUT_RUNTIME)/concurrent_c.c -o $(RUNTIME_CORE_OBJ)
	$(CXX) $(CXXFLAGS) -DCC_ENABLE_XJB_FLOAT_FMT=1 -MMD -MP -MF $(RUNTIME_XJB_OBJ:.o=.d) -c runtime/float_format_xjb.cpp -o $(RUNTIME_XJB_OBJ)
	$(CC) -nostdlib -r -o $@ $(RUNTIME_CORE_OBJ) $(RUNTIME_XJB_OBJ)

$(OBJDIR)/%.o: %.c | $(TCC_PATCH_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

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

.PHONY: all clean

# Auto-generated dependency files for objects (includes runtime/concurrent_c.c -> runtime/*.c).
-include $(DEPS)
