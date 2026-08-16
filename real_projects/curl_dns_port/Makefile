# curl_dns_port — stock libcurl in out/prefix, then a CC Curl_thrdq.
#
# Upstream under curl_c/ is read-only (setup.sh). `make cc` / `make stock`
# flip the live prefix; smoke and baseline always test what is installed.

CCC ?= $(shell test -x ../../cc/bin/ccc && echo ../../cc/bin/ccc || echo ../../out/cc/bin/ccc)
REPO_ROOT := $(abspath ../..)
CURL_SRC = curl_c
PREFIX = $(CURDIR)/out/prefix
OUT_DIR = out
BUILD_DIR = $(OUT_DIR)/build
NPROC ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
BASELINE_N ?= 32
BASELINE_ROUNDS ?= 10

CURL_VERSION := $(shell tr -d '[:space:]' < CURL_VERSION)
CC_RT_OBJ ?= $(REPO_ROOT)/out/cc/obj/runtime/concurrent_c.o
STOCK_LIB = $(PREFIX)/lib/libcurl.a.stock
STOCK_BIN = $(PREFIX)/bin/curl.stock
LIVE_LIB = $(PREFIX)/lib/libcurl.a
LIVE_BIN = $(PREFIX)/bin/curl
CURL_CONFIG = $(PREFIX)/bin/curl-config
THRDQ_MEMBER = libcurl_la-thrdqueue.o
CC_MARK = cc_curl_thrdq

ifeq ($(origin OPENSSL_PREFIX),undefined)
  ifneq ($(wildcard /opt/homebrew/opt/openssl@3),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl@3
  else ifneq ($(wildcard /usr/local/opt/openssl@3),)
    OPENSSL_PREFIX := /usr/local/opt/openssl@3
  else ifneq ($(wildcard /opt/homebrew/opt/openssl),)
    OPENSSL_PREFIX := /opt/homebrew/opt/openssl
  else
    OPENSSL_PREFIX :=
  endif
endif

CONFIGURE_FLAGS = \
	--prefix=$(PREFIX) \
	--enable-threaded-resolver \
	--disable-ares \
	--disable-shared \
	--enable-static \
	$(if $(OPENSSL_PREFIX),--with-openssl=$(OPENSSL_PREFIX),--with-openssl) \
	--without-libpsl \
	--without-libidn2 \
	--without-brotli \
	--without-zstd \
	--without-nghttp2 \
	--without-libssh2 \
	--disable-ldap \
	--disable-ldaps

BASELINE_CFLAGS = $(shell test -x $(CURL_CONFIG) && $(CURL_CONFIG) --cflags) -Wall -Wextra -O2
BASELINE_LIBS = $(shell test -x $(CURL_CONFIG) && $(CURL_CONFIG) --static-libs)
# curl-config --static-libs minus the archive path — used when relinking curl.
CURL_SYS_LIBS = $(filter-out $(LIVE_LIB),$(BASELINE_LIBS))
CC_LINK = $(CC_RT_OBJ) -lpthread -lm $(CURL_SYS_LIBS)

SAN_CC ?= $(shell command -v clang >/dev/null 2>&1 && echo clang || echo $(CC))
SAN_FLAGS = -fsanitize=address -fno-omit-frame-pointer -g -O1

.PHONY: setup upstream stock cc smoke baseline baseline-save \
	cc-swap baseline-cc baseline-cc-save flavor queue-smoke queue-asan \
	report clean help

help:
	@echo "Targets:"
	@echo "  setup              fetch curl $(CURL_VERSION) into $(CURL_SRC)/"
	@echo "  upstream           stock curl → $(PREFIX)"
	@echo "  cc                 swap Curl_thrdq for the CC queue, relink curl"
	@echo "  stock              restore the stock snapshot into prefix"
	@echo "  flavor             print stock or cc"
	@echo "  queue-smoke        knobs + blocked join/detach (no libcurl)"
	@echo "  queue-asan         same smoke, AddressSanitizer"
	@echo "  smoke / baseline   contract against the installed prefix"
	@echo "  baseline-save      write benchmarks/baseline_<flavor>_YYYY_MM_DD.txt"
	@echo "  report             stock vs cc table (REPORT_FLAGS=--md --queue)"
	@echo "  clean              remove out/"

setup:
	./setup.sh

$(CURL_SRC)/configure:
	@if [ ! -f "$(CURL_SRC)/configure" ]; then \
		echo "Missing $(CURL_SRC). Run 'make setup' first."; \
		exit 1; \
	fi

$(BUILD_DIR)/Makefile: $(CURL_SRC)/configure | $(OUT_DIR)
	@mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && ../../$(CURL_SRC)/configure $(CONFIGURE_FLAGS)

$(LIVE_BIN): $(BUILD_DIR)/Makefile
	$(MAKE) -C $(BUILD_DIR) -j$(NPROC)
	$(MAKE) -C $(BUILD_DIR) install
	@cp -f $(LIVE_LIB) $(STOCK_LIB)
	@cp -f $(LIVE_BIN) $(STOCK_BIN)
	@echo ""
	@echo "Installed stock:"
	@echo "  $(LIVE_BIN)"
	@$(LIVE_BIN) --version | head -n 3

upstream: $(LIVE_BIN)

# --- prefix flavor -------------------------------------------------------

$(OUT_DIR)/thrdqueue.o: thrdqueue.ccs cc_thrdqueue.h | $(OUT_DIR)
	$(CCC) --compile --keep-c $< -o $@

$(OUT_DIR)/thrdqueue_smoke: thrdqueue_smoke.c $(OUT_DIR)/thrdqueue.o
	@test -f "$(CC_RT_OBJ)" || { echo "missing $(CC_RT_OBJ) — build the CC toolchain first"; exit 1; }
	$(CC) -Wall -Wextra -O2 -o $@ thrdqueue_smoke.c $(OUT_DIR)/thrdqueue.o \
		$(CC_RT_OBJ) -lpthread -lm

queue-smoke: $(OUT_DIR)/thrdqueue_smoke
	$(OUT_DIR)/thrdqueue_smoke

$(OUT_DIR)/thrdqueue.asan.o: thrdqueue.ccs cc_thrdqueue.h | $(OUT_DIR)
	$(CCC) --compile --no-cache --keep-c --cc-flags "$(SAN_FLAGS)" $< -o $@

$(OUT_DIR)/thrdqueue_smoke.asan: thrdqueue_smoke.c $(OUT_DIR)/thrdqueue.asan.o
	@test -f "$(CC_RT_OBJ)" || { echo "missing $(CC_RT_OBJ) — build the CC toolchain first"; exit 1; }
	$(SAN_CC) $(SAN_FLAGS) -Wall -Wextra -o $@ thrdqueue_smoke.c \
		$(OUT_DIR)/thrdqueue.asan.o $(CC_RT_OBJ) -lpthread -lm

# Darwin ASan + CC fibers can hang (see scripts/real_projects_sanitize.sh).
# The unsanitized join_blocked test is the timing proof; this is the UAF net.
queue-asan: $(OUT_DIR)/thrdqueue_smoke.asan
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=0 \
		$(OUT_DIR)/thrdqueue_smoke.asan

# Snapshot the stock binary if upstream ran before this Makefile grew curl.stock.
$(STOCK_BIN): $(LIVE_BIN)
	@if [ ! -f "$(STOCK_BIN)" ]; then cp -f $(LIVE_BIN) $(STOCK_BIN); fi
	@if [ ! -f "$(STOCK_LIB)" ]; then cp -f $(LIVE_LIB) $(STOCK_LIB); fi

cc: queue-smoke $(STOCK_BIN)
	@test -f "$(CC_RT_OBJ)" || { echo "missing $(CC_RT_OBJ) — build the CC toolchain first"; exit 1; }
	@test -f "$(STOCK_LIB)" || { echo "missing $(STOCK_LIB) — run 'make upstream'"; exit 1; }
	@set -e; \
	swap() { \
		cp "$(STOCK_LIB)" "$$1"; \
		ar dS "$$1" $(THRDQ_MEMBER); \
		ar rS "$$1" "$(OUT_DIR)/thrdqueue.o"; \
		ranlib "$$1" >/dev/null 2>&1; \
	}; \
	swap "$(LIVE_LIB)"; \
	if [ -f "$(BUILD_DIR)/lib/.libs/libcurl.a" ]; then swap "$(BUILD_DIR)/lib/.libs/libcurl.a"; fi; \
	if [ -f "$(BUILD_DIR)/lib/libcurl.a" ]; then cp "$(LIVE_LIB)" "$(BUILD_DIR)/lib/libcurl.a"; fi
	@rm -f "$(BUILD_DIR)/src/curl" "$(BUILD_DIR)/src/.libs/curl"
	$(MAKE) -C $(BUILD_DIR)/src curl LIBS="$(CC_LINK)"
	@if [ -x "$(BUILD_DIR)/src/.libs/curl" ]; then \
		cp "$(BUILD_DIR)/src/.libs/curl" "$(LIVE_BIN)"; \
	elif [ -x "$(BUILD_DIR)/src/curl" ]; then \
		cp "$(BUILD_DIR)/src/curl" "$(LIVE_BIN)"; \
	else \
		echo "cc remake did not produce src/curl"; exit 1; \
	fi
	@nm "$(LIVE_BIN)" | grep $(CC_MARK) | grep -vq ' U ' || { \
		echo "cc curl is missing $(CC_MARK) — relink failed"; exit 1; }
	@echo "prefix is cc: $(LIVE_BIN)"

stock: $(STOCK_BIN)
	@test -f "$(STOCK_LIB)" || { echo "missing $(STOCK_LIB) — run 'make upstream'"; exit 1; }
	cp "$(STOCK_LIB)" "$(LIVE_LIB)"
	cp "$(STOCK_BIN)" "$(LIVE_BIN)"
	@if [ -f "$(BUILD_DIR)/lib/.libs/libcurl.a" ]; then cp "$(STOCK_LIB)" "$(BUILD_DIR)/lib/.libs/libcurl.a"; fi
	@if [ -f "$(BUILD_DIR)/lib/libcurl.a" ]; then cp "$(STOCK_LIB)" "$(BUILD_DIR)/lib/libcurl.a"; fi
	@echo "prefix is stock: $(LIVE_BIN)"

flavor:
	@if nm "$(LIVE_LIB)" 2>/dev/null | grep $(CC_MARK) | grep -vq ' U '; then echo cc; \
	else echo stock; fi

# Old names.
cc-swap: cc
baseline-cc: cc baseline
baseline-cc-save: cc baseline-save

# --- contract ------------------------------------------------------------

smoke: $(LIVE_BIN)
	@flav=stock; nm "$(LIVE_LIB)" 2>/dev/null | grep $(CC_MARK) | grep -vq ' U ' && flav=cc; \
	echo "=== curl smoke ($$flav) ==="
	$(LIVE_BIN) -fsS -o /dev/null -w "http=%{http_code} dns=%{time_namelookup}s total=%{time_total}s\n" \
		https://example.com/
	@echo "OK"

# Link the harness against the live archive. CC runtime is added only when
# the archive actually exports the CC queue marker (otherwise unused weight).
$(OUT_DIR)/baseline_dns: baseline_dns.c $(LIVE_BIN) | $(OUT_DIR)
	@extra=""; \
	if nm "$(LIVE_LIB)" | grep $(CC_MARK) | grep -vq ' U '; then extra="$(CC_LINK)"; fi; \
	$(CC) $(BASELINE_CFLAGS) -o $@ baseline_dns.c $(BASELINE_LIBS) $$extra

baseline: $(OUT_DIR)/baseline_dns
	./baseline.sh $(BASELINE_N) $(BASELINE_ROUNDS)

baseline-save: $(OUT_DIR)/baseline_dns
	./baseline.sh --save $(BASELINE_N) $(BASELINE_ROUNDS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

clean:
	rm -rf $(OUT_DIR)
