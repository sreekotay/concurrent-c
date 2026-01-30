# Makefile for pigz - builds both original (pthread) and CC versions

CC = gcc
CFLAGS = -O3 -Wall -Wextra -pthread -Ipigz_c -DNOZOPFLI
LDFLAGS = -lz -lpthread

# Output directory for binaries
OUT_DIR = out

# Original pigz sources (downloaded by setup.sh)
PIGZ_C_DIR = pigz_c
PIGZ_SRCS = $(PIGZ_C_DIR)/pigz.c $(PIGZ_C_DIR)/yarn.c $(PIGZ_C_DIR)/try.c

# CC compiler
CCC = ../../out/cc/bin/ccc

# Ensure output directory exists
$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Original pigz (pthread-based)
$(OUT_DIR)/pigz: $(PIGZ_SRCS) $(PIGZ_C_DIR)/yarn.h $(PIGZ_C_DIR)/try.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,-dead_strip -o $@ $(PIGZ_SRCS) $(LDFLAGS)

# CC version
$(OUT_DIR)/pigz_cc: pigz_cc/pigz_cc.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_cc/pigz_cc.ccs --ld-flags "-lz"

# Idiomatic CC version (with T!E result types)
$(OUT_DIR)/pigz_idiomatic: pigz_idiomatic.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ pigz_idiomatic.ccs --ld-flags "-lz"

# Convenience targets
pigz: $(OUT_DIR)/pigz
pigz_cc: $(OUT_DIR)/pigz_cc
pigz_idiomatic: $(OUT_DIR)/pigz_idiomatic

all: pigz pigz_cc pigz_idiomatic

# Download source files if not present
setup:
	./setup.sh

# Test both versions produce identical output
test: pigz pigz_cc
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
	@echo "=== Verifying decompression ==="
	gunzip -c test_orig.gz > test_orig_decomp.bin
	gunzip -c test_cc.gz > test_cc_decomp.bin
	cmp test_input.bin test_orig_decomp.bin && echo "Original: OK"
	cmp test_input.bin test_cc_decomp.bin && echo "CC: OK"
	@rm -f test_input.bin test_orig.gz test_cc.gz test_orig_decomp.bin test_cc_decomp.bin

clean:
	rm -rf $(OUT_DIR) *.o *.gz *.bin

smoke: pigz_cc
	@./smoke.sh

.PHONY: setup test clean all pigz pigz_cc pigz_idiomatic smoke
