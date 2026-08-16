# Makefile for pigz - builds both original (pthread) and CC versions
#
# Zopfli (level 11 in upstream pigz.c and pigz_cc): discovery is automatic unless
# you set both ZOPFLI_CFLAGS and ZOPFLI_LDFLAGS (non-empty). Order: pkg-config
# libzopfli, pkg-config zopfli, Homebrew paths, /usr/local, system /usr/include.

CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -pthread
PIGZ_CFLAGS ?= -Ipigz_c
PIGZ_LDFLAGS ?= -lz -lpthread

# User/environment may preset both; otherwise we fill them in below.
ZOPFLI_CFLAGS ?=
ZOPFLI_LDFLAGS ?=

ifeq ($(and $(strip $(ZOPFLI_CFLAGS)),$(strip $(ZOPFLI_LDFLAGS))),)
  ifeq ($(shell pkg-config libzopfli --exists 2>/dev/null && echo 1),1)
    override ZOPFLI_CFLAGS := $(shell pkg-config --cflags libzopfli)
    override ZOPFLI_LDFLAGS := $(shell pkg-config --libs libzopfli)
  else ifeq ($(shell pkg-config zopfli --exists 2>/dev/null && echo 1),1)
    override ZOPFLI_CFLAGS := $(shell pkg-config --cflags zopfli)
    override ZOPFLI_LDFLAGS := $(shell pkg-config --libs zopfli)
  else ifneq ($(wildcard /opt/homebrew/include/zopfli.h),)
    override ZOPFLI_CFLAGS := -I/opt/homebrew/include
    override ZOPFLI_LDFLAGS := -L/opt/homebrew/lib -lzopfli
  else ifneq ($(wildcard /usr/local/include/zopfli.h),)
    override ZOPFLI_CFLAGS := -I/usr/local/include
    override ZOPFLI_LDFLAGS := -L/usr/local/lib -lzopfli
  else ifneq ($(wildcard /usr/include/zopfli.h),)
    override ZOPFLI_CFLAGS :=
    override ZOPFLI_LDFLAGS := -lzopfli
  else ifneq ($(wildcard /usr/include/zopfli/zopfli.h),)
    # Debian/Ubuntu libzopfli-dev installs headers under include/zopfli/.
    override ZOPFLI_CFLAGS := -I/usr/include/zopfli
    override ZOPFLI_LDFLAGS := -lzopfli
  else
    $(error Zopfli not found (need zopfli.h + libzopfli). Install: brew install zopfli (macOS), apt install libzopfli-dev (Debian/Ubuntu), or set both ZOPFLI_CFLAGS and ZOPFLI_LDFLAGS for your prefix.)
  endif
endif

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SECTION_LDFLAGS ?= -Wl,-dead_strip
else
SECTION_LDFLAGS ?= -Wl,--gc-sections
endif

# Output directory for binaries
OUT_DIR = out

# Original pigz sources (downloaded by setup.sh)
PIGZ_C_DIR = pigz_c
PIGZ_SRCS = $(PIGZ_C_DIR)/pigz.c $(PIGZ_C_DIR)/yarn.c $(PIGZ_C_DIR)/try.c

# CC compiler (wrapper preferred; falls back to out/ layout)
CCC ?= $(shell test -x ../../cc/bin/ccc && echo ../../cc/bin/ccc || echo ../../out/cc/bin/ccc)

# Ensure output directory exists
$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Original pigz (pthread-based; Zopfli for -11 per upstream pigz.c)
$(OUT_DIR)/pigz: $(PIGZ_SRCS) $(PIGZ_C_DIR)/yarn.h $(PIGZ_C_DIR)/try.h | $(OUT_DIR)
	$(CC) $(CFLAGS) $(PIGZ_CFLAGS) $(ZOPFLI_CFLAGS) -ffunction-sections -fdata-sections $(SECTION_LDFLAGS) -o $@ $(PIGZ_SRCS) $(PIGZ_LDFLAGS) $(ZOPFLI_LDFLAGS)

# CC version
$(OUT_DIR)/pigz_cc: pigz_cc/pigz_cc.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_cc/pigz_cc.ccs --ld-flags "-lz $(ZOPFLI_LDFLAGS)" --cc-flags "$(ZOPFLI_CFLAGS)" --release

# Idiomatic CC version (with T!E result types)
$(OUT_DIR)/pigz_idiomatic: pigz_idiomatic.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_idiomatic.ccs --ld-flags "-lz" --release

# Batch @parallel for (no nursery / channel); seekable inputs only
$(OUT_DIR)/pigz_parallel: pigz_parallel.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_parallel.ccs --ld-flags "-lz" --release

# Pthread CC version (OS threads)
$(OUT_DIR)/pigz_pthread: pigz_pthread.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_pthread.ccs --ld-flags "-lz" --release

# Legacy hybrid-named variant, ported to main task-channel APIs
$(OUT_DIR)/pigz_hybrid: pigz_hybrid.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_hybrid.ccs --ld-flags "-lz" --release

# Profiling version (fiber tasks + per-block timing)
$(OUT_DIR)/pigz_profile: pigz_profile.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_profile.ccs --ld-flags "-lz" --release

# Profiling version (pthread tasks + per-block timing)
$(OUT_DIR)/pigz_pthread_profile: pigz_pthread_profile.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_pthread_profile.ccs --ld-flags "-lz" --release

# Unordered variant: no head-of-line blocking (output is corrupt, benchmarking only)
$(OUT_DIR)/pigz_unordered: pigz_unordered.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_unordered.ccs --ld-flags "-lz" --release

# Convenience targets
pigz: $(OUT_DIR)/pigz
pigz_cc: $(OUT_DIR)/pigz_cc
pigz_idiomatic: $(OUT_DIR)/pigz_idiomatic
pigz_parallel: $(OUT_DIR)/pigz_parallel
pigz_hybrid: $(OUT_DIR)/pigz_hybrid
pigz_pthread: $(OUT_DIR)/pigz_pthread
pigz_profile: $(OUT_DIR)/pigz_profile
pigz_pthread_profile: $(OUT_DIR)/pigz_pthread_profile
pigz_unordered: $(OUT_DIR)/pigz_unordered

all: pigz pigz_cc pigz_idiomatic pigz_parallel pigz_hybrid pigz_pthread

# Download source files if not present
setup:
	./setup.sh

# Test both versions produce identical output
test: pigz pigz_cc pigz_idiomatic pigz_hybrid
	@echo "=== Generating test data ==="
	dd if=/dev/urandom of=test_input.bin bs=1M count=10 2>/dev/null
	@echo ""
	@echo "=== Testing original pigz ==="
	$(OUT_DIR)/pigz -k -c test_input.bin > test_orig.gz
	@echo "Original compressed size: $$(wc -c < test_orig.gz) bytes"
	@echo ""
	@echo "=== Testing CC pigz ==="
	$(OUT_DIR)/pigz_cc -k -c test_input.bin > test_cc.gz
	@echo "CC compressed size: $$(wc -c < test_cc.gz) bytes"
	@echo ""
	@echo "=== Testing idiomatic pigz demos ==="
	$(OUT_DIR)/pigz_idiomatic test_input.bin
	mv test_input.bin.gz test_idiomatic.gz
	$(OUT_DIR)/pigz_hybrid test_input.bin
	mv test_input.bin.gz test_hybrid.gz
	@echo ""
	@echo "=== Verifying decompression ==="
	gunzip -c test_orig.gz > test_orig_decomp.bin
	gunzip -c test_cc.gz > test_cc_decomp.bin
	gunzip -c test_idiomatic.gz > test_idiomatic_decomp.bin
	gunzip -c test_hybrid.gz > test_hybrid_decomp.bin
	cmp test_input.bin test_orig_decomp.bin && echo "Original: OK"
	cmp test_input.bin test_cc_decomp.bin && echo "CC: OK"
	cmp test_input.bin test_idiomatic_decomp.bin && echo "Idiomatic: OK"
	cmp test_input.bin test_hybrid_decomp.bin && echo "Hybrid: OK"
	@rm -f test_input.bin test_orig.gz test_cc.gz test_idiomatic.gz test_hybrid.gz test_orig_decomp.bin test_cc_decomp.bin test_idiomatic_decomp.bin test_hybrid_decomp.bin

clean:
	rm -rf $(OUT_DIR) *.o *.gz *.bin

smoke: pigz_cc
	@./smoke.sh

.PHONY: setup test clean all pigz pigz_cc pigz_idiomatic pigz_parallel pigz_hybrid smoke
