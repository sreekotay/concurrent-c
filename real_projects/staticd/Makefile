CCC ?= $(shell test -x ../../cc/bin/ccc && echo ../../cc/bin/ccc || echo ../../out/cc/bin/ccc)
OUT_DIR = out
DARKHTTPD_DIR = vendor/darkhttpd
DARKHTTPD_BIN = $(OUT_DIR)/darkhttpd

# TLS comes with the toolchain: `make bearssl` at the repository root builds
# BearSSL, and the next `make -C cc` compiles the runtime against it; the
# driver links the archive by itself. Without it staticd builds and runs,
# and `--tls-cert` reports that the runtime has no TLS.
REPO_CC = ../../cc
REPO_ROOT = ../..

.PHONY: all staticd darkhttpd fixtures clean setup smoke bench ws stress string_tpl tls

all: staticd

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Build BearSSL and a TLS runtime, then staticd.
tls:
	$(MAKE) -C $(REPO_ROOT) bearssl
	$(MAKE) -C $(REPO_CC)
	$(MAKE) staticd

$(OUT_DIR)/staticd: staticd.ccs staticd_ws.cch \
		staticd_http.cch staticd_block.cch staticd_fs.cch staticd_script.cch \
		$(REPO_CC)/include/ccc/std/server.cch $(REPO_CC)/include/ccc/std/server_serve.ccs \
		$(REPO_CC)/include/ccc/std/server_poll.cch $(REPO_CC)/include/ccc/std/server_poll.ccs \
		| $(OUT_DIR)
	CFLAGS="$(EXTRA_CFLAGS) $${CFLAGS:-}" $(CCC) -O -o $@ staticd.ccs --release

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

# Narrow WebSocket subset (spawn throwaway staticd on :18080).
ws: staticd
	python3 ./ws_test.py --spawn ./out/staticd --port 18080

# Adversarial storms (see ../../stress/staticd/staticd_stress.md).
stress: staticd fixtures
	cd ../.. && CHAOS_SCALE=$${CHAOS_SCALE:-quick} ./stress/staticd/run.sh

# Isolated RSS (fresh process per cell). Override with DURATION=5 REPEATS=3 …
bench: staticd fixtures
	./bench_latency.sh

clean:
	rm -rf $(OUT_DIR)
