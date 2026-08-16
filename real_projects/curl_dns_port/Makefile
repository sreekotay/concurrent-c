# curl_dns_port — stock libcurl baseline, then CC asyn-thrdd swap.
#
# Upstream is read-only under curl_c/ (fetched by setup.sh).
# Builds install into out/prefix/ so the stock curl CLI is PATH-local.
#
# CC path: asyn_thrdd.c (host overlay) + cc_resolv_q.ccs (nursery/hybrid queue)
# swapped into libcurl.a; baseline_dns_cc links the CC runtime.

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

CURL_CONFIG = $(PREFIX)/bin/curl-config
ASYN_CFLAGS = -DHAVE_CONFIG_H \
	-I$(CURDIR)/$(CURL_SRC)/include \
	-I$(CURDIR)/$(BUILD_DIR)/lib \
	-I$(CURDIR)/$(CURL_SRC)/lib \
	-I$(CURDIR) \
	-DBUILDING_LIBCURL -DCURL_STATICLIB -DCURL_HIDDEN_SYMBOLS \
	$(if $(OPENSSL_PREFIX),-isystem $(OPENSSL_PREFIX)/include) \
	-fvisibility=hidden -O2 -Werror=implicit-function-declaration

BASELINE_CFLAGS = $(shell test -x $(CURL_CONFIG) && $(CURL_CONFIG) --cflags) -Wall -Wextra -O2
BASELINE_LIBS = $(shell test -x $(CURL_CONFIG) && $(CURL_CONFIG) --static-libs)
CC_EXTRA_LIBS = $(OUT_DIR)/cc_resolv_q.o $(CC_RT_OBJ) -lpthread -lm
ifeq ($(shell uname -s),Darwin)
CC_EXTRA_LIBS += -L$(OPENSSL_PREFIX)/lib -framework CoreFoundation \
	-framework CoreServices -framework SystemConfiguration -lssl -lcrypto -lz
endif

.PHONY: setup upstream smoke baseline baseline-save \
	cc-resolv cc-swap baseline-cc baseline-cc-save clean help

help:
	@echo "Targets:"
	@echo "  setup              fetch curl $(CURL_VERSION) into $(CURL_SRC)/"
	@echo "  upstream           stock curl (threaded DNS) → out/prefix"
	@echo "  smoke / baseline   stock contract (see BASELINE.md)"
	@echo "  cc-resolv          build cc_resolv_q.o + asyn_thrdd.o"
	@echo "  cc-swap            replace asyn-thrdd in libcurl.a with CC overlay"
	@echo "  baseline-cc        run contract against CC-swapped libcurl"
	@echo "  baseline-cc-save   write benchmarks/baseline_cc_YYYY_MM_DD.txt"
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

$(PREFIX)/bin/curl: $(BUILD_DIR)/Makefile
	$(MAKE) -C $(BUILD_DIR) -j$(NPROC)
	$(MAKE) -C $(BUILD_DIR) install
	@cp -f $(PREFIX)/lib/libcurl.a $(PREFIX)/lib/libcurl.a.stock
	@echo ""
	@echo "Installed:"
	@echo "  $(PREFIX)/bin/curl"
	@$(PREFIX)/bin/curl --version | head -n 3

upstream: $(PREFIX)/bin/curl

smoke: upstream
	@echo "=== stock curl smoke (HTTPS + DNS) ==="
	$(PREFIX)/bin/curl -fsS -o /dev/null -w "http=%{http_code} dns=%{time_namelookup}s total=%{time_total}s\n" \
		https://example.com/
	@echo "OK"

$(OUT_DIR)/baseline_dns: baseline_dns.c $(PREFIX)/bin/curl | $(OUT_DIR)
	$(CC) $(BASELINE_CFLAGS) -o $@ baseline_dns.c $(BASELINE_LIBS)

baseline: $(OUT_DIR)/baseline_dns
	./baseline.sh $(BASELINE_N) $(BASELINE_ROUNDS)

baseline-save: $(OUT_DIR)/baseline_dns
	./baseline.sh --save $(BASELINE_N) $(BASELINE_ROUNDS)

# --- Concurrent-C swap ---------------------------------------------------

$(OUT_DIR)/cc_resolv_q.o: cc_resolv_q.ccs cc_resolv_q.h | $(OUT_DIR)
	$(CCC) --compile --keep-c $< -o $@

$(OUT_DIR)/asyn_thrdd.o: asyn_thrdd.c cc_resolv_q.h $(PREFIX)/bin/curl | $(OUT_DIR)
	$(CC) $(ASYN_CFLAGS) -c asyn_thrdd.c -o $@

cc-resolv: $(OUT_DIR)/cc_resolv_q.o $(OUT_DIR)/asyn_thrdd.o
	@echo "CC objects ready: $^"
	@test -f $(CC_RT_OBJ) || (echo "missing $(CC_RT_OBJ) — build the CC toolchain first"; exit 1)

cc-swap: cc-resolv
	@test -f $(PREFIX)/lib/libcurl.a.stock || cp $(PREFIX)/lib/libcurl.a $(PREFIX)/lib/libcurl.a.stock
	cp $(PREFIX)/lib/libcurl.a.stock $(PREFIX)/lib/libcurl.a
	ar d $(PREFIX)/lib/libcurl.a libcurl_la-asyn-thrdd.o
	ar r $(PREFIX)/lib/libcurl.a $(OUT_DIR)/asyn_thrdd.o
	ranlib $(PREFIX)/lib/libcurl.a
	@echo "Swapped asyn_thrdd.o into $(PREFIX)/lib/libcurl.a"

$(OUT_DIR)/baseline_dns_cc: baseline_dns.c cc-swap | $(OUT_DIR)
	$(CC) $(BASELINE_CFLAGS) -o $@ baseline_dns.c \
		$(PREFIX)/lib/libcurl.a $(CC_EXTRA_LIBS)

baseline-cc: $(OUT_DIR)/baseline_dns_cc
	HARNESS=$(OUT_DIR)/baseline_dns_cc ./baseline.sh $(BASELINE_N) $(BASELINE_ROUNDS)

baseline-cc-save: $(OUT_DIR)/baseline_dns_cc
	@mkdir -p benchmarks
	HARNESS=$(OUT_DIR)/baseline_dns_cc ./baseline.sh $(BASELINE_N) $(BASELINE_ROUNDS) \
		| tee benchmarks/baseline_cc_$$(date +%Y_%m_%d).txt

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

clean:
	rm -rf $(OUT_DIR)
