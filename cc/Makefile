CC ?= cc
TCC_BIN ?=
TCC_EXT ?= 1
TCC_INC ?= ../third_party/tcc
TCC_LIB ?= ../third_party/tcc
CC_ENABLE_ASYNC ?= 0
CFLAGS_COMMON := -Wall -Wextra -Isrc -Iinclude
ifeq ($(CC_ENABLE_ASYNC),1)
  CFLAGS_COMMON += -DCC_ENABLE_ASYNC
endif
ifeq ($(BUILD),debug)
  CFLAGS := $(CFLAGS_COMMON) -O0 -g
else
  CFLAGS := $(CFLAGS_COMMON) -O2
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
  CFLAGS += -DCC_TCC_EXT_AVAILABLE -I$(TCC_INC)
  # If TCC_LIB points to a directory, link -Ldir -ltcc; if it points to an
  # archive, link that archive directly.
  ifneq ($(suffix $(TCC_LIB)),.a)
    LDFLAGS += -L$(TCC_LIB)
    LDLIBS  += -ltcc
  else
    LDLIBS  += $(TCC_LIB)
  endif
endif

# Compile runtime as a single translation unit for easy linking. concurrent_c.c pulls in channel/nursery/scheduler/io/string/exec and async shards when CC_ENABLE_ASYNC.
# Note: src/visitor/checker.c is a stale, unported experiment against an old AST and is excluded from the build.
SRCS := $(filter-out src/visitor/checker.c runtime/io_async.c runtime/channel.c runtime/nursery.c runtime/scheduler.c runtime/async_chan.c runtime/async_runtime.c runtime/async_backend_poll.c runtime/exec.c,$(wildcard src/*.c src/*/*.c)) runtime/concurrent_c.c
# Keep our local bridge even when TCC_EXT=1; it adapts libtcc hooks into CC's AST handle.
OBJDIR ?= ../out/cc/obj
OUT_BINDIR ?= ../out/cc/bin
OBJS := $(SRCS:%.c=$(OBJDIR)/%.o)
REAL_BINARY := $(OUT_BINDIR)/ccc
WRAPPER := bin/ccc

all: $(WRAPPER)

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

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(WRAPPER) $(REAL_BINARY)
	rm -rf $(OBJDIR)
	# Clean legacy in-tree objects from earlier layouts.
	@rm -f $(SRCS:.c=.o)
	@rm -f bin/cc ../out/cc/bin/cc

.PHONY: all clean

