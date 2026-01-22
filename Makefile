CC_DIR := cc
BUILD ?= debug
BEARSSL_DIR := third_party/bearssl
CURL_DIR := third_party/curl
CURL_BUILD := $(CURL_DIR)/build

.PHONY: all cc clean fmt lint example example-c smoke test tools
.PHONY: tcc-patch-apply tcc-patch-regen tcc-update-check
.PHONY: deps bearssl bearssl-clean curl curl-clean deps-update
.PHONY: examples-check stress-check perf-check full-check

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

# Verify all examples compile (CI smoke test for example rot).
examples-check: cc
	@echo "=== Checking examples compile ==="
	@failed=0; \
	for f in examples/*.ccs; do \
		printf "  %-40s" "$$f"; \
		if $(CC_DIR)/bin/ccc --emit-c-only "$$f" -o /dev/null 2>/dev/null; then \
			echo "OK"; \
		else \
			echo "FAIL"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	for d in examples/*/; do \
		if [ -f "$$d/build.cc" ]; then \
			printf "  %-40s" "$$d"; \
			if $(CC_DIR)/bin/ccc build --build-file "$$d/build.cc" --dry-run 2>/dev/null; then \
				echo "OK"; \
			else \
				echo "FAIL"; \
				failed=$$((failed + 1)); \
			fi; \
		fi; \
	done; \
	if [ $$failed -gt 0 ]; then \
		echo "$$failed example(s) failed"; \
		exit 1; \
	fi; \
	echo "All examples OK"

# Verify all stress tests compile and run.
stress-check: cc
	@echo "=== Running stress tests ==="
	@failed=0; \
	for f in stress/*.ccs; do \
		printf "  %-40s" "$$f"; \
		if [ "$$f" = "stress/deadlock_detect_demo.ccs" ]; then \
			CC_DEADLOCK_TIMEOUT=1 $(CC_DIR)/bin/ccc run "$$f" >/dev/null 2>&1; \
			rc=$$?; \
			if [ $$rc -eq 124 ] || [ $$rc -eq 1 ]; then \
				echo "OK"; \
			else \
				echo "FAIL"; \
				failed=$$((failed + 1)); \
			fi; \
		else \
			if $(CC_DIR)/bin/ccc run "$$f" >/dev/null 2>&1; then \
				echo "OK"; \
			else \
				echo "FAIL"; \
				failed=$$((failed + 1)); \
			fi; \
		fi; \
	done; \
	if [ $$failed -gt 0 ]; then \
		echo "$$failed stress test(s) failed"; \
		exit 1; \
	fi; \
	echo "All stress tests OK"

# Verify all perf tests compile and run.
perf-check: cc
	@echo "=== Running perf tests ==="
	@failed=0; \
	for f in perf/*.ccs; do \
		printf "  %-40s" "$$f"; \
		if $(CC_DIR)/bin/ccc run "$$f" >/dev/null 2>&1; then \
			echo "OK"; \
		else \
			echo "FAIL"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	if [ $$failed -gt 0 ]; then \
		echo "$$failed perf test(s) failed"; \
		exit 1; \
	fi; \
	echo "All perf tests OK"

# Run examples, stress, and perf in one go.
full-check: examples-check stress-check perf-check

# ---- Dependencies -----------------------------------------------------------
#
# Dependencies are opt-in. Only build/link what you need:
#   make bearssl   - TLS support (for <std/tls.cch>)
#   make curl      - HTTP client (for <std/http.cch>)
#
# In your build.cc, add the libraries you need:
#   CC_TARGET_LIBS myapp third_party/bearssl/build/libbearssl.a
#   CC_TARGET_LIBS myapp third_party/curl/build/lib/libcurl.a

# Build BearSSL static library (for TLS)
bearssl:
	@echo "Building BearSSL..."
	@$(MAKE) -C $(BEARSSL_DIR) -j4
	@echo "BearSSL built: $(BEARSSL_DIR)/build/libbearssl.a"

bearssl-clean:
	@$(MAKE) -C $(BEARSSL_DIR) clean

# Check for system libcurl (preferred - already has TLS)
curl-check:
	@if command -v curl-config >/dev/null 2>&1; then \
		echo "System libcurl found: $$(curl-config --version)"; \
		echo "  Include: $$(curl-config --cflags)"; \
		echo "  Libs: $$(curl-config --libs)"; \
		echo "Use system curl in build.cc:"; \
		echo "  CC_TARGET_CFLAGS myapp \$$(curl-config --cflags)"; \
		echo "  CC_TARGET_LDFLAGS myapp \$$(curl-config --libs)"; \
	else \
		echo "System libcurl not found. Install via:"; \
		echo "  macOS: brew install curl"; \
		echo "  Ubuntu: apt install libcurl4-openssl-dev"; \
	fi

# Build vendored libcurl (requires cmake)
# Minimal build: HTTP/HTTPS only, uses BearSSL for TLS
curl-build: bearssl
	@command -v cmake >/dev/null 2>&1 || { echo "cmake required. Install via: brew install cmake"; exit 1; }
	@echo "Building libcurl (minimal, with BearSSL)..."
	@mkdir -p $(CURL_BUILD)
	@cd $(CURL_BUILD) && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DBUILD_CURL_EXE=OFF \
		-DCURL_USE_BEARSSL=ON \
		-DBEARSSL_INCLUDE_DIR=$(PWD)/$(BEARSSL_DIR)/inc \
		-DBEARSSL_LIBRARY=$(PWD)/$(BEARSSL_DIR)/build/libbearssl.a \
		-DCURL_DISABLE_LDAP=ON \
		-DCURL_DISABLE_LDAPS=ON \
		-DCURL_DISABLE_TELNET=ON \
		-DCURL_DISABLE_DICT=ON \
		-DCURL_DISABLE_FILE=ON \
		-DCURL_DISABLE_TFTP=ON \
		-DCURL_DISABLE_RTSP=ON \
		-DCURL_DISABLE_POP3=ON \
		-DCURL_DISABLE_IMAP=ON \
		-DCURL_DISABLE_SMTP=ON \
		-DCURL_DISABLE_GOPHER=ON \
		-DCURL_DISABLE_MQTT=ON \
		-DCURL_DISABLE_SMB=ON \
		-DCURL_DISABLE_FTP=ON \
		-DHTTP_ONLY=ON \
		-DCURL_CA_BUNDLE=none \
		-DCURL_CA_PATH=none \
		>/dev/null
	@$(MAKE) -C $(CURL_BUILD) -j4
	@echo "libcurl built: $(CURL_BUILD)/lib/libcurl.a"

curl-clean:
	@rm -rf $(CURL_BUILD)

# Build all optional dependencies (only BearSSL by default, curl uses system)
deps: bearssl

# Update all dependencies to latest versions
deps-update:
	@echo "Updating submodules to latest..."
	@git submodule update --remote --merge
	@echo "Submodules updated. Rebuild needed deps with: make bearssl / make curl"

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

