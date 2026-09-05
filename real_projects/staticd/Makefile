CCC = ../../out/cc/bin/ccc
OUT_DIR = out
DARKHTTPD_DIR = vendor/darkhttpd
DARKHTTPD_BIN = $(OUT_DIR)/darkhttpd

.PHONY: all staticd darkhttpd fixtures clean setup smoke string_tpl

all: staticd

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/staticd: staticd.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ staticd.ccs --release

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

clean:
	rm -rf $(OUT_DIR)
