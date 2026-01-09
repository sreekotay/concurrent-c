CC_DIR := cc
BUILD ?= debug

.PHONY: all cc clean fmt lint example example-c smoke
.PHONY: tcc-patch-apply tcc-patch-regen tcc-update-check

all: cc

cc:
	$(MAKE) -C $(CC_DIR) BUILD=$(BUILD)

clean:
	$(MAKE) -C $(CC_DIR) clean

fmt:
	@./scripts/format.sh

lint:
	@./scripts/lint.sh

# Build and run the UFCS hello example through our compiler.
example: cc
	@mkdir -p out
	@$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c examples/hello.cc out/hello.c
	@cc -Icc/include -Icc -I. out/hello.c cc/runtime/concurrent_c.o -o out/hello && ./out/hello

example-c:
	@mkdir -p out
	@cc examples/hello_c.c -o out/hello_c && ./out/hello_c

smoke: cc
	@mkdir -p out
	@$(MAKE) example
	@if [ "$$TCC_EXT" = "1" ]; then \
		$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/ufcs_smoke.cc out/ufcs_smoke.c; \
		cc -Icc/include -Icc -I. out/ufcs_smoke.c cc/runtime/concurrent_c.o -o out/ufcs_smoke && ./out/ufcs_smoke; \
		$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/ufcs_multiline.cc out/ufcs_multiline.c; \
		cc -Icc/include -Icc -I. out/ufcs_multiline.c cc/runtime/concurrent_c.o -o out/ufcs_multiline && ./out/ufcs_multiline; \
		$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/ufcs_nested_multiline.cc out/ufcs_nested_multiline.c; \
		cc -Icc/include -Icc -I. out/ufcs_nested_multiline.c cc/runtime/concurrent_c.o -o out/ufcs_nested_multiline && ./out/ufcs_nested_multiline; \
	else \
		echo "[smoke] skipping UFCS tests (set TCC_EXT=1 to enable)"; \
	fi
	@$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/ufcs_double_write.cc out/ufcs_double_write.c
	@cc -Icc/include -Icc -I. out/ufcs_double_write.c cc/runtime/concurrent_c.o -o out/ufcs_double_write && ./out/ufcs_double_write
	@$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/vec_smoke.c out/vec_smoke.c
	@cc -Icc/include -Icc -I. out/vec_smoke.c cc/runtime/concurrent_c.o -o out/vec_smoke && ./out/vec_smoke
	@$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/io_smoke.c out/io_smoke.c
	@cc -Icc/include -Icc -I. out/io_smoke.c cc/runtime/concurrent_c.o -o out/io_smoke && ./out/io_smoke
	@$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/map_smoke.c out/map_smoke.c
	@cc -Icc/include -Icc -I. out/map_smoke.c cc/runtime/concurrent_c.o -o out/map_smoke && ./out/map_smoke
	@if [ "$$CC_ENABLE_ASYNC" = "1" ]; then \
		$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/channel_select_smoke.c out/channel_select_smoke.c; \
		cc -Icc/include -Icc -I. out/channel_select_smoke.c cc/runtime/concurrent_c.o -o out/channel_select_smoke -lpthread && ./out/channel_select_smoke; \
	else \
		echo "[smoke] skipping channel_select_smoke (set CC_ENABLE_ASYNC=1 to enable)"; \
	fi
	@$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/channel_send_take_reject.c out/channel_send_take_reject.c
	@cc -Icc/include -Icc -I. out/channel_send_take_reject.c cc/runtime/concurrent_c.o -o out/channel_send_take_reject && ./out/channel_send_take_reject
	@echo "[sourcemap] expect compile error mapped to CC source"
	@$(CC_DIR)/bin/cc --emit-c-only --no-runtime --keep-c tests/sourcemap_fail.cc out/sourcemap_fail.c
	@set -e; if cc -Icc/include -Icc -I. -Werror=implicit-function-declaration out/sourcemap_fail.c cc/runtime/concurrent_c.o -o out/sourcemap_fail 2> out/sourcemap_fail.err; then \
		echo "expected sourcemap_fail to fail compilation" >&2; exit 1; \
	fi; \
	grep -q "tests/sourcemap_fail.cc:6" out/sourcemap_fail.err && echo "[sourcemap] mapped OK"

# ---- TCC upgrade convenience ------------------------------------------------

# Apply our local hooks patch(es) into the `third_party/tcc` working tree.
tcc-patch-apply:
	@./scripts/apply_tcc_patches.sh

# Regenerate `third_party/tcc-patches/0001-cc-ext-hooks.patch` from the current
# `third_party/tcc` working tree diff (HEAD -> working tree).
tcc-patch-regen:
	@./scripts/regen_tcc_patches.sh

# “One button” check when upgrading TCC:
# - apply patch (idempotent)
# - build TCC
# - build + run CC smoke suite against the patched libtcc.a
tcc-update-check:
	@$(MAKE) tcc-patch-apply
	@$(MAKE) -C third_party/tcc -j4
	@$(MAKE) -B smoke TCC_EXT=1 TCC_INC=third_party/tcc TCC_LIB=../third_party/tcc/libtcc.a

