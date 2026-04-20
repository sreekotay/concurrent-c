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
SRCS := $(filter-out runtime/io_async.c runtime/channel.c runtime/nursery.c runtime/scheduler.c runtime/fiber_sched.c runtime/fiber.c runtime/async_chan.c runtime/async_runtime.c runtime/async_backend_poll.c runtime/exec.c src/tools/%,$(wildcard src/*.c src/*/*.c)) runtime/concurrent_c.c
# Keep our local bridge even when TCC_EXT=1; it adapts libtcc hooks into CC's AST handle.
OBJDIR ?= ../out/cc/obj
OUT_BINDIR ?= ../out/cc/bin
OBJS := $(SRCS:%.c=$(OBJDIR)/%.o)
RUNTIME_CORE_OBJ := $(OBJDIR)/runtime/concurrent_c.core.o
RUNTIME_XJB_OBJ := $(OBJDIR)/runtime/float_format_xjb.o
DEPS := $(OBJS:.o=.d) $(RUNTIME_CORE_OBJ:.o=.d) $(RUNTIME_XJB_OBJ:.o=.d)
REAL_BINARY := bin/ccc
WRAPPER := $(OUT_BINDIR)/ccc

LOWER_HEADERS_SRCS := src/tools/lower_headers.c \
	src/header/lower_header.c \
	src/preprocess/preprocess.c \
	src/preprocess/type_registry.c \
	src/result_spec.c \
	src/comptime/symbols.c \
	src/visitor/pass_type_syntax.c \
	src/visitor/pass_err_syntax.c \
	src/visitor/pass_result_unwrap.c \
	src/visitor/pass_unwrap_destroy.c \
	src/util/io.c \
	src/util/path.c \
	src/util/result_fn_registry.c
LOWER_HEADERS_OBJS := $(LOWER_HEADERS_SRCS:%.c=$(OBJDIR)/%.o)
LOWER_HEADERS_BINARY := $(OUT_BINDIR)/lower_headers
LOWER_HEADERS_INPUTS := $(shell find include -name '*.cch' -type f)

# Lowered headers output directory
OUT_INCLUDE := ../out/include
OUT_RUNTIME := ../out/runtime
LOWER_HEADERS_STAMP := $(OUT_INCLUDE)/.headers_lowered.stamp

all: $(WRAPPER) $(LOWER_HEADERS_STAMP)

# Lower .cch headers to .h files (must run before examples that use generated C)
.PHONY: lower-headers
lower-headers: $(LOWER_HEADERS_STAMP)

$(LOWER_HEADERS_STAMP): $(LOWER_HEADERS_BINARY) $(LOWER_HEADERS_INPUTS) $(wildcard runtime/*.c runtime/*.h)
	@mkdir -p $(OUT_INCLUDE) $(OUT_RUNTIME)
	@$(LOWER_HEADERS_BINARY) include $(OUT_INCLUDE) --rewrite-includes runtime $(OUT_RUNTIME)
	@touch $@

.PHONY: test-build-stub
test-build-stub: $(WRAPPER)
	@echo "Running build stub test..."
	@sh ../examples/build_stub/test-build-stub.sh

$(REAL_BINARY): $(OBJS) $(TCC_LIB_DEP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(WRAPPER): $(REAL_BINARY)
	@mkdir -p $(dir $@)
	@printf '%s\n' '#!/bin/sh' > $@
	@printf '%s\n' 'set -e' >> $@
	@printf '%s\n' 'SELF_DIR="$$(cd "$$(dirname "$$0")" && pwd)"' >> $@
	@printf '%s\n' 'exec "$$SELF_DIR/../../../cc/bin/ccc" "$$@"' >> $@
	@chmod +x $@

$(LOWER_HEADERS_BINARY): $(LOWER_HEADERS_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LOWER_HEADERS_OBJS) $(LDLIBS)

$(OBJDIR)/runtime/concurrent_c.o: $(LOWER_HEADERS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DCC_ENABLE_XJB_FLOAT_FMT=1 -MMD -MP -MF $(RUNTIME_CORE_OBJ:.o=.d) -c $(OUT_RUNTIME)/concurrent_c.c -o $(RUNTIME_CORE_OBJ)
	$(CXX) $(CXXFLAGS) -DCC_ENABLE_XJB_FLOAT_FMT=1 -MMD -MP -MF $(RUNTIME_XJB_OBJ:.o=.d) -c runtime/float_format_xjb.cpp -o $(RUNTIME_XJB_OBJ)
	$(CC) -nostdlib -r -o $@ $(RUNTIME_CORE_OBJ) $(RUNTIME_XJB_OBJ)

$(OBJDIR)/%.o: %.c
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

.PHONY: all clean

# Auto-generated dependency files for objects (includes runtime/concurrent_c.c -> runtime/*.c).
-include $(DEPS)
