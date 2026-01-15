CC_DIR := cc
BUILD ?= debug
BEARSSL_DIR := third_party/bearssl

.PHONY: all cc clean fmt lint example example-c smoke test tools
.PHONY: tcc-patch-apply tcc-patch-regen tcc-update-check
.PHONY: deps bearssl bearssl-clean deps-update

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
	@$(CC_DIR)/bin/ccc build run --out-dir out examples/hello.ccs

example-c:
	@mkdir -p out
	@cc examples/hello_c.c -o out/hello_c && ./out/hello_c

smoke: cc
	@$(CC_DIR)/bin/ccc build test --out-dir out

# Build tools (host C).
tools:
	@mkdir -p tools
	@cc -O2 -Wall -Wextra tools/cc_test.c -o tools/cc_test

# Prefer using ccc itself for tests (the runner drives ./cc/bin/ccc).
test: cc tools
	@./tools/cc_test

# ---- Dependencies -----------------------------------------------------------

# Build all third-party dependencies
deps: bearssl

# Build BearSSL static library
bearssl:
	@echo "Building BearSSL..."
	@$(MAKE) -C $(BEARSSL_DIR) -j4
	@echo "BearSSL built: $(BEARSSL_DIR)/build/libbearssl.a"

bearssl-clean:
	@$(MAKE) -C $(BEARSSL_DIR) clean

# Update all dependencies to latest versions
deps-update:
	@echo "Updating submodules to latest..."
	@git submodule update --remote --merge
	@echo "Submodules updated. Rebuild with: make deps"

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

