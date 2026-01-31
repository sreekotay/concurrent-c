CC ?= cc
TCC_BIN ?=
TCC_EXT ?= 1
TCC_INC ?= ../third_party/tcc
TCC_LIB ?= ../third_party/tcc
CC_ENABLE_ASYNC ?= 1
CC_ENABLE_TLS ?= 0
BEARSSL_DIR ?= ../third_party/bearssl
CFLAGS_COMMON := -Wall -Wextra -Isrc -Iinclude
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
  # If TCC_LIB points to a directory, link -Ldir -ltcc; if it points to an
  # archive, link that archive directly.
  ifneq ($(suffix $(TCC_LIB)),.a)
    LDFLAGS += -L$(TCC_LIB)
    LDLIBS  += -ltcc
  else
    LDLIBS  += $(TCC_LIB)
  endif
endif

# Compile runtime as a single translation unit for easy linking. concurrent_c.c pulls in channel/nursery/scheduler/fiber_sched/fiber/io/string/exec and async shards when CC_ENABLE_ASYNC.
SRCS := $(filter-out runtime/io_async.c runtime/channel.c runtime/nursery.c runtime/scheduler.c runtime/fiber_sched.c runtime/fiber.c runtime/async_chan.c runtime/async_runtime.c runtime/async_backend_poll.c runtime/exec.c src/cccn/%,$(wildcard src/*.c src/*/*.c)) runtime/concurrent_c.c
# Keep our local bridge even when TCC_EXT=1; it adapts libtcc hooks into CC's AST handle.
OBJDIR ?= ../out/cc/obj
OUT_BINDIR ?= ../out/cc/bin
OBJS := $(SRCS:%.c=$(OBJDIR)/%.o)
DEPS := $(OBJS:.o=.d)
REAL_BINARY := $(OUT_BINDIR)/ccc
WRAPPER := bin/ccc

# CCCN (CCC Next) refactor
CCCN_SRCS := $(wildcard src/cccn/**/*.c) src/build/build_helpers.c src/util/*.c src/driver.c src/preprocess/preprocess.c src/comptime/symbols.c src/parser/tcc_bridge.c src/parser/cc_ext_parser.c src/visitor/pass.c src/visitor/visitor_fileutil.c src/visitor/visitor_pipeline.c src/visitor/pass_strip_markers.c src/visitor/pass_ufcs.c src/visitor/pass_closure_calls.c src/visitor/pass_autoblock.c src/visitor/pass_await_normalize.c src/visitor/async_ast.c src/visitor/checker.c src/visitor/pass_arena_ast.c src/visitor/pass_nursery_spawn_ast.c src/visitor/pass_closure_literal_ast.c src/visitor/pass_defer_syntax.c src/visitor/pass_channel_syntax.c src/visitor/pass_type_syntax.c src/visitor/pass_match_syntax.c src/visitor/pass_with_deadline_syntax.c src/visitor/edit_buffer.c src/visitor/walk.c src/visitor/visitor.c src/visitor/ufcs.c src/preprocess/type_registry.c src/visitor/visit_codegen.c src/parser/parse.c
CCCN_OBJS := $(CCCN_SRCS:%.c=$(OBJDIR)/%.o)
CCCN_BINARY := $(OUT_BINDIR)/cccn
CCCN_WRAPPER := bin/cccn

all: $(WRAPPER) $(CCCN_WRAPPER)

.PHONY: test-build-stub
test-build-stub: $(WRAPPER)
	@echo "Running build stub test..."
	@sh ../examples/build_stub/test-build-stub.sh

$(REAL_BINARY): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(WRAPPER): $(REAL_BINARY)
	@mkdir -p $(dir $@)
	@printf '%s\n' '#!/bin/sh' > $@
	@printf '%s\n' 'set -e' >> $@
	@printf '%s\n' 'SELF_DIR="$$(cd "$$(dirname "$$0")" && pwd)"' >> $@
	@printf '%s\n' 'exec "$$SELF_DIR/../../out/cc/bin/ccc" "$$@"' >> $@
	@chmod +x $@

$(CCCN_BINARY): $(CCCN_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CCCN_OBJS) $(LDLIBS)

$(CCCN_WRAPPER): $(CCCN_BINARY)
	@mkdir -p $(dir $@)
	@printf '%s\n' '#!/bin/sh' > $@
	@printf '%s\n' 'set -e' >> $@
	@printf '%s\n' 'SELF_DIR="$$(cd "$$(dirname "$$0")" && pwd)"' >> $@
	@printf '%s\n' 'exec "$$SELF_DIR/../../out/cc/bin/cccn" "$$@"' >> $@
	@chmod +x $@

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

clean:
	rm -f $(WRAPPER) $(REAL_BINARY)
	rm -rf $(OBJDIR)
	# Clean legacy in-tree objects from earlier layouts.
	@rm -f $(SRCS:.c=.o)
	@rm -f bin/cc ../out/cc/bin/cc

.PHONY: all clean

# Auto-generated dependency files for objects (includes runtime/concurrent_c.c -> runtime/*.c).
-include $(DEPS)
