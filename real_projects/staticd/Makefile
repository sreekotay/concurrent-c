CCC = ../../out/cc/bin/ccc
OUT_DIR = out
DARKHTTPD_DIR = vendor/darkhttpd
DARKHTTPD_BIN = $(OUT_DIR)/darkhttpd

# TLS (BearSSL). ccc --release skips the stock prebuilt runtime and compiles a
# per-flag host runtime; CFLAGS must carry -DCC_ENABLE_TLS so that object
# includes tls.c. staticd.ccs @link()s the archive (path, not -l).
CC_ENABLE_TLS ?= 1
BEARSSL_DIR ?= ../../third_party/bearssl
BEARSSL_LIB = $(BEARSSL_DIR)/build/libbearssl.a
BEARSSL_INC = $(abspath $(BEARSSL_DIR)/inc)
REPO_CC = ../../cc
REPO_ROOT = ../..

.PHONY: all staticd darkhttpd fixtures clean setup smoke bench string_tpl \
	ensure-tls-runtime ensure-bearssl

all: staticd

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

ensure-bearssl:
	@test -f $(BEARSSL_LIB) || $(MAKE) -C $(REPO_ROOT) bearssl

# Sync cc/runtime → out/runtime when headers change; ensure concurrent_c.o
# was built with TLS (ccc --release still compiles its own host runtime via
# CFLAGS below — this keeps the stock object consistent too).
ensure-tls-runtime: ensure-bearssl
ifeq ($(CC_ENABLE_TLS),1)
	$(MAKE) -C $(REPO_CC) CC_ENABLE_TLS=1 lower-headers
	@if ! nm $(REPO_ROOT)/out/cc/obj/runtime/concurrent_c.o 2>/dev/null | grep -q 'cc_tls_server_load'; then \
		rm -f $(REPO_ROOT)/out/cc/obj/runtime/concurrent_c.o; \
		$(MAKE) -C $(REPO_CC) CC_ENABLE_TLS=1 ../out/cc/obj/runtime/concurrent_c.o; \
	fi
endif

$(OUT_DIR)/staticd: staticd.ccs staticd_ws.cch \
		staticd_http.cch staticd_block.cch staticd_fs.cch staticd_script.cch \
		| $(OUT_DIR) ensure-tls-runtime
ifeq ($(CC_ENABLE_TLS),1)
	CFLAGS="-DCC_ENABLE_TLS -I$(BEARSSL_INC) $${CFLAGS:-}" \
		$(CCC) -O -o $@ staticd.ccs --release
else
	$(CCC) -O -o $@ staticd.ccs --release
endif

staticd: $(OUT_DIR)/staticd

$(OUT_DIR)/bench_string_tpl: bench_string_tpl.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ bench_string_tpl.ccs --release

string_tpl: $(OUT_DIR)/bench_string_tpl
	$(OUT_DIR)/bench_string_tpl

$(DARKHTTPD_DIR)/darkhttpd.c:
	./setup.sh --darkhttpd-only

$(DARKHTTPD_BIN): $(DARKHTTPD_DIR)/darkhttpd.c | $(OUT_DIR)
	$(CC) -O2 -o $@ $(DARKHTTPD_DIR)/darkhttpd.c

darkhttpd: $(DARKHTTPD_BIN)

fixtures:
	./gen_fixtures.sh

setup:
	./setup.sh

smoke: staticd fixtures
	./compare.sh --smoke

# Isolated RSS (fresh process per cell). Override with DURATION=5 REPEATS=3 …
bench: staticd fixtures
	./bench_latency.sh

clean:
	rm -rf $(OUT_DIR)
