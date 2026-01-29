# Makefile for pigz - builds both original (pthread) and CC versions

CC = gcc
CFLAGS = -O3 -Wall -Wextra -pthread -I.
LDFLAGS = -lz -lpthread

# Zopfli sources
ZOPFLI_SRCS = zopfli/src/zopfli/blocksplitter.c \
              zopfli/src/zopfli/cache.c \
              zopfli/src/zopfli/deflate.c \
              zopfli/src/zopfli/gzip_container.c \
              zopfli/src/zopfli/hash.c \
              zopfli/src/zopfli/katajainen.c \
              zopfli/src/zopfli/lz77.c \
              zopfli/src/zopfli/squeeze.c \
              zopfli/src/zopfli/tree.c \
              zopfli/src/zopfli/util.c \
              zopfli/src/zopfli/zlib_container.c \
              zopfli/src/zopfli/zopfli_lib.c

# CC compiler
CCC = ../../cc/bin/ccc

# Original pigz (pthread-based)
pigz: pigz.c yarn.c try.c yarn.h try.h $(ZOPFLI_SRCS)
	$(CC) $(CFLAGS) -o $@ pigz.c yarn.c try.c $(ZOPFLI_SRCS) $(LDFLAGS)

# CC version
pigz_cc: pigz_cc.ccs zlib_wrapper.c
	$(CCC) -o $@ pigz_cc.ccs zlib_wrapper.c --ld-flags "-lz"

# Download source files if not present
setup:
	./setup.sh

# Test both versions produce identical output
test: pigz pigz_cc
	@echo "=== Generating test data ==="
	dd if=/dev/urandom of=test_input.bin bs=1M count=10 2>/dev/null
	@echo ""
	@echo "=== Testing original pigz ==="
	./pigz -k -c test_input.bin > test_orig.gz
	@echo "Original compressed size: $$(wc -c < test_orig.gz) bytes"
	@echo ""
	@echo "=== Testing CC pigz ==="
	./pigz_cc -k -c test_input.bin > test_cc.gz
	@echo "CC compressed size: $$(wc -c < test_cc.gz) bytes"
	@echo ""
	@echo "=== Verifying decompression ==="
	gunzip -c test_orig.gz > test_orig_decomp.bin
	gunzip -c test_cc.gz > test_cc_decomp.bin
	cmp test_input.bin test_orig_decomp.bin && echo "Original: OK"
	cmp test_input.bin test_cc_decomp.bin && echo "CC: OK"
	@rm -f test_input.bin test_orig.gz test_cc.gz test_orig_decomp.bin test_cc_decomp.bin

# Benchmark comparison
benchmark: pigz pigz_cc
	@echo "=== Generating benchmark data (100MB) ==="
	dd if=/dev/urandom of=bench_data.bin bs=1M count=100 2>/dev/null
	@echo ""
	@echo "=== Benchmarking original pigz ==="
	time ./pigz -k -p 4 bench_data.bin
	mv bench_data.bin.gz bench_orig.gz
	@echo ""
	@echo "=== Benchmarking CC pigz ==="
	time ./pigz_cc -k -p 4 bench_data.bin
	mv bench_data.bin.gz bench_cc.gz
	@echo ""
	@echo "=== Results ==="
	@echo "Original: $$(wc -c < bench_orig.gz) bytes"
	@echo "CC:       $$(wc -c < bench_cc.gz) bytes"
	@rm -f bench_data.bin bench_orig.gz bench_cc.gz

clean:
	rm -f pigz pigz_cc *.o *.gz *.bin

.PHONY: setup test benchmark clean
