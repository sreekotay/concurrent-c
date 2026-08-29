CCC = ../../out/cc/bin/ccc
OUT_DIR = out
REPO_ROOT = ../..
CC_LOCAL ?= cc
REDIS_IDIOMATIC_DEFS ?=
REDIS_SRC_DIR = redis_c
REDIS_VERSION := $(shell tr -d '\n' < REDIS_VERSION)

ifeq ($(REDIS_VERSION),stable)
REDIS_URL = https://download.redis.io/redis-stable.tar.gz
else
REDIS_URL = https://download.redis.io/releases/redis-$(REDIS_VERSION).tar.gz
endif

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

setup:
	./setup.sh

$(REDIS_SRC_DIR)/src/redis-server: | $(OUT_DIR)
	@if [ ! -d "$(REDIS_SRC_DIR)" ]; then \
		echo "Missing $(REDIS_SRC_DIR). Run 'make setup' first."; \
		exit 1; \
	fi
	$(MAKE) -C $(REDIS_SRC_DIR)/src MALLOC=libc

$(REDIS_SRC_DIR)/src/redis-cli: $(REDIS_SRC_DIR)/src/redis-server
	@true

$(REDIS_SRC_DIR)/src/redis-benchmark: $(REDIS_SRC_DIR)/src/redis-server
	@true

upstream: $(REDIS_SRC_DIR)/src/redis-server $(REDIS_SRC_DIR)/src/redis-cli $(REDIS_SRC_DIR)/src/redis-benchmark

# Default: full RedisDb + sketch I/O (ln.serve, Conn.out encode, sync fiber).
$(OUT_DIR)/redis_idiomatic: redis_idiomatic.ccs redis_cc/redis_cc.cch redis_db.cch redis_reply.cch redis_mem.cch redis_glob.cch redis_resp.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_idiomatic.ccs --release --cc-flags "$(REDIS_IDIOMATIC_DEFS)"

# Channel/owner-fiber variant (historical N:1 request path).
$(OUT_DIR)/redis_owner: redis_owner.ccs redis_cc/redis_cc.cch redis_db.cch redis_mem.cch redis_glob.cch redis_resp.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_owner.ccs --release --cc-flags "$(REDIS_IDIOMATIC_DEFS)"

# Alias: older scripts/docs said redis_lock for the exclusive variant.
$(OUT_DIR)/redis_lock: $(OUT_DIR)/redis_idiomatic
	ln -sfn redis_idiomatic $@

# Reader A/B variants: gen (the default @grammar reader, same as plain
# redis_idiomatic) and hand (-DREDIS_RESP_HAND selects redis_resp.cch's
# handrolled reader).  Distinct outputs so both coexist for bench_readers.sh.
$(OUT_DIR)/redis_idiomatic_gen: redis_idiomatic.ccs redis_cc/redis_cc.cch redis_db.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_idiomatic.ccs --release --cc-flags "$(REDIS_IDIOMATIC_DEFS)"

$(OUT_DIR)/redis_idiomatic_hand: redis_idiomatic.ccs redis_cc/redis_cc.cch redis_db.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_idiomatic.ccs --release --cc-flags "$(REDIS_IDIOMATIC_DEFS) -DREDIS_RESP_HAND=1"

# Profile: optimized release build with symbols (sample / Instruments).
$(OUT_DIR)/redis_idiomatic_profile: redis_idiomatic.ccs redis_cc/redis_cc.cch redis_db.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_idiomatic.ccs --release --cc-flags "$(REDIS_IDIOMATIC_DEFS) -g"

# Debug: symbols + light optimization (lldb-friendly without pathological slowdown).
$(OUT_DIR)/redis_idiomatic_debug: redis_idiomatic.ccs redis_cc/redis_cc.cch redis_db.cch | $(OUT_DIR)
	$(CCC) -o $@ redis_idiomatic.ccs --cc-flags "$(REDIS_IDIOMATIC_DEFS) -g -Og"

$(OUT_DIR)/redis_cc: redis_cc/redis_cc.ccs redis_cc/redis_cc.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_cc/redis_cc.ccs --release

# Tiny command surface (redis.go); sync fiber + ln.serve + Conn encode.
$(OUT_DIR)/redis_async_sketch: redis_async_sketch.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ redis_async_sketch.ccs --release

# Throwaway: sketch + wait-for MGET (per-key hold, snapshot, ordered encode).
$(OUT_DIR)/redis_parallel: redis_parallel.ccs | $(OUT_DIR)
	$(CCC) -O -o $@ redis_parallel.ccs --release

redis_idiomatic: $(OUT_DIR)/redis_idiomatic
redis_owner: $(OUT_DIR)/redis_owner
redis_lock: $(OUT_DIR)/redis_lock
redis_idiomatic_gen: $(OUT_DIR)/redis_idiomatic_gen
redis_idiomatic_hand: $(OUT_DIR)/redis_idiomatic_hand
redis_idiomatic_profile: $(OUT_DIR)/redis_idiomatic_profile
redis_idiomatic_debug: $(OUT_DIR)/redis_idiomatic_debug
redis_cc: $(OUT_DIR)/redis_cc
redis_async_sketch: $(OUT_DIR)/redis_async_sketch
redis_parallel: $(OUT_DIR)/redis_parallel

all: redis_idiomatic redis_cc

clean:
	rm -rf $(OUT_DIR)

print-config:
	@echo "REDIS_VERSION=$(REDIS_VERSION)"
	@echo "REDIS_URL=$(REDIS_URL)"
	@echo "REDIS_SRC_DIR=$(REDIS_SRC_DIR)"
	@echo "REDIS_IDIOMATIC_DEFS=$(REDIS_IDIOMATIC_DEFS)"

.PHONY: setup upstream redis_idiomatic redis_owner redis_lock redis_idiomatic_gen redis_idiomatic_hand redis_idiomatic_profile redis_idiomatic_debug redis_cc redis_async_sketch redis_parallel all clean print-config
