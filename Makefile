CC_DIR := cc
BUILD ?= debug
BEARSSL_DIR := third_party/bearssl
CURL_DIR := third_party/curl
CURL_BUILD := $(CURL_DIR)/build

.PHONY: all cc clean distclean fmt lint example smoke test tools
.PHONY: install install-check uninstall
.PHONY: runtime-variant-smoke fuzz-check
.PHONY: tcc-patch-apply tcc-patch-regen tcc-update-check check-submodules lint-scanners lint-soft-return-emit test-strict
.PHONY: deps bearssl bearssl-clean curl curl-clean deps-update
.PHONY: examples-check stress-check perf-check full-check
.PHONY: perf-baseline perf-regress perf-regress-oracle

all: cc

cc:
	$(MAKE) -C $(CC_DIR) BUILD=$(BUILD)

clean:
	$(MAKE) -C $(CC_DIR) clean

# Nuke ALL regenerable build clutter, including the multi-GB bin/, out/, and
# tmp/ trees plus any stray misfired-compile artifacts. Forces a full rebuild.
distclean: clean
	rm -rf bin out tmp
	rm -f -- ./-- cc/-E cc/src/cc/-E

# ---- Installation -----------------------------------------------------------
#
# Install the compiler and runtime to a prefix (default: /usr/local).
# Use DESTDIR for staged installs (e.g., packaging).
#
#   make install                     # install to /usr/local
#   make install PREFIX=/opt/ccc     # install to /opt/ccc
#   make install DESTDIR=/tmp/pkg    # staged install for packaging
#
# The installed layout:
#   $PREFIX/bin/ccc                   - compiler driver (default front: serdes)
#   $PREFIX/bin/shadow_lower          - serdes lowerer (required beside ccc)
#   $PREFIX/include/ccc/**/*.cch      - standard library headers
#   $PREFIX/include/ccc/**/*.h        - the same headers, pre-lowered
#   $PREFIX/lib/ccc/runtime/*.c,.h    - pre-lowered runtime source and internal headers
#   $PREFIX/lib/ccc/runtime/vendor/   - vendored third-party runtime sources
#
# An installed tree is self-contained: the driver resolves headers, runtime,
# and shadow_lower from the prefix (cc_init_paths / cc__find_shadow_lower) and
# never reaches back into a checkout. Consequences:
#
#   * ccc must be the real binary. cc/bin/ccc and out/cc/bin/ccc are wrappers
#     that reach siblings by relative path; those paths do not survive
#     relocation into a prefix.
#   * shadow_lower must ship next to ccc — serdes is the default front.
#   * .cch headers and runtime sources must ship pre-lowered. Lowering is a
#     build-tree step (out/cc/bin/lower_headers), so an installed driver
#     cannot do it on demand.
#
# `make install-check` compiles a program against the installed prefix and is
# the check that both of those actually hold.

PREFIX ?= /usr/local

ZMIJ_VENDOR := cc/runtime/vendor
TCC_DIR := third_party/tcc

install: cc
	@echo "Installing Concurrent-C to $(DESTDIR)$(PREFIX)..."
	@test -f $(ZMIJ_VENDOR)/zmij.c -a -f $(ZMIJ_VENDOR)/zmij-c.h || { \
		echo "Error: missing vendored zmij under $(ZMIJ_VENDOR)/"; \
		exit 1; }
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/lib/ccc/runtime
	install -d $(DESTDIR)$(PREFIX)/lib/ccc/runtime/vendor
	install -d $(DESTDIR)$(PREFIX)/include/ccc/std
	install -d $(DESTDIR)$(PREFIX)/include/ccc/script
	install -d $(DESTDIR)$(PREFIX)/include/ccc/vendor
	install -d $(DESTDIR)$(PREFIX)/lib/ccc/tcc/include
	@test -x cc/bin/.ccc-bin || { echo "Error: missing cc/bin/.ccc-bin (make cc)"; exit 1; }
	@test -x out/cc/bin/shadow_lower -o -x cc/bin/shadow_lower || { \
		echo "Error: missing shadow_lower (make -C cc); serdes front needs it"; exit 1; }
	install -m 755 cc/bin/.ccc-bin $(DESTDIR)$(PREFIX)/bin/ccc
	@if [ -x out/cc/bin/shadow_lower ]; then \
		install -m 755 out/cc/bin/shadow_lower $(DESTDIR)$(PREFIX)/bin/shadow_lower; \
	else \
		install -m 755 cc/bin/shadow_lower $(DESTDIR)$(PREFIX)/bin/shadow_lower; \
	fi
	install -m 644 cc/include/ccc/*.cch $(DESTDIR)$(PREFIX)/include/ccc/
	install -m 644 cc/include/ccc/std/*.cch $(DESTDIR)$(PREFIX)/include/ccc/std/
	install -m 644 cc/include/ccc/script/*.cch $(DESTDIR)$(PREFIX)/include/ccc/script/
	install -m 644 out/include/ccc/*.h $(DESTDIR)$(PREFIX)/include/ccc/
	@if [ -n "$$(ls cc/include/ccc/*.h 2>/dev/null)" ]; then \
		install -m 644 cc/include/ccc/*.h $(DESTDIR)$(PREFIX)/include/ccc/; \
	fi
	install -m 644 out/include/ccc/std/*.h $(DESTDIR)$(PREFIX)/include/ccc/std/
	install -m 644 out/include/ccc/script/*.h $(DESTDIR)$(PREFIX)/include/ccc/script/
	@if [ -n "$$(ls cc/include/ccc/vendor/*.h 2>/dev/null)" ]; then \
		install -m 644 cc/include/ccc/vendor/*.h $(DESTDIR)$(PREFIX)/include/ccc/vendor/; \
	fi
	install -m 644 out/runtime/*.c $(DESTDIR)$(PREFIX)/lib/ccc/runtime/
	@if [ -n "$$(ls out/runtime/*.h 2>/dev/null)" ]; then \
		install -m 644 out/runtime/*.h $(DESTDIR)$(PREFIX)/lib/ccc/runtime/; \
	fi
	install -m 644 cc/runtime/float_format_zmij.c $(DESTDIR)$(PREFIX)/lib/ccc/runtime/
	install -m 644 $(ZMIJ_VENDOR)/zmij.c $(DESTDIR)$(PREFIX)/lib/ccc/runtime/vendor/zmij.c
	install -m 644 $(ZMIJ_VENDOR)/zmij-c.h $(DESTDIR)$(PREFIX)/lib/ccc/runtime/vendor/zmij-c.h
	install -m 644 $(ZMIJ_VENDOR)/LICENSE $(DESTDIR)$(PREFIX)/lib/ccc/runtime/vendor/LICENSE
	install -m 644 $(ZMIJ_VENDOR)/ZMIJ_NOTICE.txt $(DESTDIR)$(PREFIX)/lib/ccc/runtime/vendor/ZMIJ_NOTICE.txt
	install -m 644 $(TCC_DIR)/include/*.h $(DESTDIR)$(PREFIX)/lib/ccc/tcc/include/
	@if [ -f $(TCC_DIR)/libtcc1.a ]; then \
		install -m 644 $(TCC_DIR)/libtcc1.a $(DESTDIR)$(PREFIX)/lib/ccc/tcc/; \
	fi
	@echo "Installed. Add $(DESTDIR)$(PREFIX)/bin to PATH if needed."

# Compile and run a program using only $(PREFIX), from a directory with no
# checkout above it. Catches the failure mode where an install looks complete
# but the driver falls back to a dev tree (or to nothing) at first use.
install-check:
	@ccc_bin="$(DESTDIR)$(PREFIX)/bin/ccc"; \
	test -x "$$ccc_bin" || { echo "install-check: $$ccc_bin is not executable"; exit 1; }; \
	work="$$(mktemp -d)"; \
	trap 'rm -rf "$$work"' EXIT INT TERM; \
	printf '#include <ccc/std/prelude.cch>\n#include <stdio.h>\n\nint main(void) {\n    CCArena a = cc_arena_heap(kilobytes(4));\n    CCVec::[int] v = cc_vec_new::[int](&a);\n    v.push(41);\n    int got = *v.get(0);\n    printf("install-check ok\\n");\n    cc_arena_free(&a);\n    return got == 41 ? 0 : 1;\n}\n' > "$$work/install_check.ccs"; \
	echo "install-check: compiling against $(DESTDIR)$(PREFIX) in $$work"; \
	( cd "$$work" && "$$ccc_bin" run install_check.ccs ) || { \
		echo "install-check: FAILED — the installed prefix cannot compile a program"; exit 1; }; \
	printf '%s\n' \
	  '#include <ccc/std/prelude.cch>' \
	  '#include <ccc/script/js.cch>' \
	  '' \
	  'typedef struct ICCounter { int n; } ICCounter;' \
	  'int ICCounter_inc(ICCounter* self) { return ++self->n; }' \
	  'static ICCounter ic_seed = {0};' \
	  '@comptime cc_js_export("ic_counter", "ICCounter", &ic_seed);' \
	  > "$$work/ic_mod.ccs"; \
	echo "install-check: module-export (out-of-tree source) against $(DESTDIR)$(PREFIX)"; \
	( cd "$$work" && "$$ccc_bin" build ic_mod.ccs ) || { \
		echo "install-check: FAILED — js_module factory not available out-of-tree"; exit 1; }; \
	echo "install-check: ok"

uninstall:
	@echo "Uninstalling Concurrent-C from $(DESTDIR)$(PREFIX)..."
	rm -f $(DESTDIR)$(PREFIX)/bin/ccc
	rm -f $(DESTDIR)$(PREFIX)/bin/shadow_lower
	rm -rf $(DESTDIR)$(PREFIX)/lib/ccc
	rm -rf $(DESTDIR)$(PREFIX)/include/ccc
	@echo "Uninstalled."

fmt:
	@./tools/dev.shcc @fmt

lint:
	@./tools/dev.shcc @lint

# Scanner-hygiene ratchet (PASS_CLEANUP_PLAN phase 4): fails on new
# hand-rolled comment/string state machines outside the canonical scanners.
lint-scanners:
	@./tools/dev.shcc @lint_scanners

# Soft-return / watermark emit-shape ratchet (spec §5.1 conformance).
lint-soft-return-emit:
	@./tools/dev.shcc @lint_soft_return_emit

# Offsets golden smoke (PASS_CLEANUP_PLAN phase 1): the full suite with the
# UFCS byte-offset self-check FATAL.  Any drift between recorded offsets and
# the parse buffer fails the run instead of warning.
test-strict:
	CC_STRICT_OFFSETS=1 ./tools/cc_test

check-submodules:
	@./tools/dev.shcc @check_submodules

# Build and run the UFCS hello example through our compiler.
example: cc
	@$(CC_DIR)/bin/ccc build run --out-dir out examples/hello.ccs

smoke: cc
	@$(CC_DIR)/bin/ccc build test --out-dir out

# Build tools (host C).
tools:
	@mkdir -p tools
	@cc -O2 -Wall -Wextra tools/cc_test.c -o tools/cc_test

# Prefer using ccc itself for tests (the runner drives ./cc/bin/ccc).
test: cc tools out-of-tree-smoke runtime-variant-smoke
	@./tools/cc_test

# Smoke: verify `ccc` can compile a source file that lives outside the repo
# tree.  Regression guard for `cc_path_find_repo_root` -> header include
# path resolution.  Without this, `!>` on pointer-returning runtime funcs
# (e.g. `cc_nursery_create`) silently fall back to Result-style lowering
# because the function isn't registered as pointer-returning.
out-of-tree-smoke: cc
	@./tools/out_of_tree_smoke.shcc

# Smoke: the runtime object a build links matches the flags it was asked for.
# Regression guard for cc__prebuilt_runtime_applies and the per-variant runtime
# cache. Without it, `CFLAGS=-DFOO ccc run x.ccs` silently reuses the runtime
# `make -C cc` built without -DFOO, and reports it as reused.
runtime-variant-smoke: cc
	@./tools/runtime_variant_smoke.shcc

# Fuzzers: comment-insertion behavior preservation + mutation crash oracle.
fuzz-check: cc
	@./tools/fuzz_comment_insert.shcc 1 100
	@./tools/fuzz_mutate.shcc 1 100

# Verify all examples compile (tools/make.shcc @examples_check).
examples-check: cc
	@./tools/make.shcc @examples_check

# Suite runners via tools/make.shcc (policy still lives in run_all.ccs).
stress-check: cc
	@./tools/make.shcc @stress_check

perf-check: cc
	@./tools/make.shcc @perf_check

full-check: examples-check stress-check perf-check

# Compiler perf baseline / regression guard. See perf/README.md
# "Compiler perf baseline" section for what each captured metric means.
perf-baseline: cc tools
	@./tools/perf.shcc @perf_baseline

perf-regress: cc tools
	@./tools/perf.shcc @perf_regress

# Bash oracle for the .shcc twin (same metrics / tolerances).
perf-regress-oracle: cc tools
	@./tools/perf.shcc @perf_oracle

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

