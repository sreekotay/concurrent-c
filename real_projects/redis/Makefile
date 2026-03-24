CCC = ../../out/cc/bin/ccc
OUT_DIR = out
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
	$(MAKE) -C $(REDIS_SRC_DIR) MALLOC=libc

$(REDIS_SRC_DIR)/src/redis-cli: $(REDIS_SRC_DIR)/src/redis-server
	@true

$(REDIS_SRC_DIR)/src/redis-benchmark: $(REDIS_SRC_DIR)/src/redis-server
	@true

upstream: $(REDIS_SRC_DIR)/src/redis-server $(REDIS_SRC_DIR)/src/redis-cli $(REDIS_SRC_DIR)/src/redis-benchmark

$(OUT_DIR)/redis_tutorial: redis_tutorial.ccs redis_cc/redis_cc.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_tutorial.ccs --release

$(OUT_DIR)/redis_idiomatic: redis_idiomatic.ccs redis_cc/redis_cc.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_idiomatic.ccs --release

$(OUT_DIR)/redis_cc: redis_cc/redis_cc.ccs redis_cc/redis_cc.cch | $(OUT_DIR)
	$(CCC) -O -o $@ redis_cc/redis_cc.ccs --release

redis_tutorial: $(OUT_DIR)/redis_tutorial
redis_idiomatic: $(OUT_DIR)/redis_idiomatic
redis_cc: $(OUT_DIR)/redis_cc

all: redis_tutorial redis_idiomatic redis_cc

clean:
	rm -rf $(OUT_DIR)

print-config:
	@echo "REDIS_VERSION=$(REDIS_VERSION)"
	@echo "REDIS_URL=$(REDIS_URL)"
	@echo "REDIS_SRC_DIR=$(REDIS_SRC_DIR)"

.PHONY: setup upstream redis_tutorial redis_idiomatic redis_cc all clean print-config
