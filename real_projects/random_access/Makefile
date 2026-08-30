CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CCC ?= $(shell test -x ../../cc/bin/ccc && echo ../../cc/bin/ccc || echo ../../out/cc/bin/ccc)
OUT_DIR = out

CHPL ?= chpl

.PHONY: all clean cc chapel dist

all: $(OUT_DIR)/ra_cc $(OUT_DIR)/ra_cc_dist

chapel: $(OUT_DIR)/ra_chpl $(OUT_DIR)/ra_chpl_atomic

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/ra_c: ra.c ra.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ ra.c

$(OUT_DIR)/ra_cc: ra_idiomatic.ccs ra.h | $(OUT_DIR)
	$(CCC) build --release ra_idiomatic.ccs -o $@

$(OUT_DIR)/ra_cc_dist: ra_dist.ccs ra.h | $(OUT_DIR)
	$(CCC) build --release ra_dist.ccs -o $@

$(OUT_DIR)/ra_chpl: chapel/ra.chpl chapel/RARandomStream.chpl chapel/HPCCProblemSize.chpl | $(OUT_DIR)
	cd chapel && $(CHPL) --fast -suseLCG=false ra.chpl -o ../$(OUT_DIR)/ra_chpl

$(OUT_DIR)/ra_chpl_atomic: chapel/ra-atomics.chpl chapel/RARandomStream.chpl chapel/HPCCProblemSize.chpl | $(OUT_DIR)
	cd chapel && $(CHPL) --fast -suseLCG=false ra-atomics.chpl -o ../$(OUT_DIR)/ra_chpl_atomic

cc: $(OUT_DIR)/ra_cc

dist: $(OUT_DIR)/ra_cc_dist

clean:
	rm -rf $(OUT_DIR)
