CC ?= cc
TCC_BIN ?=
TCC_EXT ?= 0
TCC_INC ?= ../third_party/tcc
TCC_LIB ?= ../third_party/tcc
CFLAGS_COMMON := -Wall -Wextra -Isrc -Iinclude -DCC_ENABLE_ASYNC
ifeq ($(BUILD),debug)
  CFLAGS := $(CFLAGS_COMMON) -O0 -g
else
  CFLAGS := $(CFLAGS_COMMON) -O2
endif
# Optional TCC extension (set TCC_EXT=1 and point TCC_INC/TCC_LIB at patched tcc)
ifeq ($(TCC_EXT),1)
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
SRCS := $(filter-out runtime/io_async.c runtime/channel.c runtime/nursery.c runtime/scheduler.c runtime/async_chan.c runtime/async_runtime.c runtime/async_backend_poll.c runtime/exec.c,$(wildcard src/*.c src/*/*.c)) runtime/concurrent_c.c
# When linking against patched TCC (TCC_EXT=1), skip our local bridge to avoid
# duplicate symbols—the real implementations come from libtcc.
ifeq ($(TCC_EXT),1)
  SRCS := $(filter-out src/parser/tcc_bridge.c,$(SRCS))
endif
OBJS := $(SRCS:.c=.o)
BINARY := bin/cc

all: $(BINARY)

.PHONY: test-build-stub
test-build-stub: $(BINARY)
	@echo "Running build stub test..."
	@sh ../examples/build_stub/test-build-stub.sh

$(BINARY): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

clean:
	rm -f $(OBJS) $(BINARY)

.PHONY: all clean

