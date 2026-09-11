# Fetch raylib (not in git) and build the walkable parallel_trace demo.
#
#   make            # fetch + libraylib.a + storm
#   make run        # WASD + mouse look
#
#   STORM_W / STORM_H   framebuffer (default 1024×1024)
#   STORM_WIN_W/H       window (default 1024×1024)
#   STORM_GRID          XZ DDA march steps for infinite sphere lattice
#   STORM_MODE          for (default) | unbound | quad
#   STORM_CUT           quadtree spawn depth (default 3)
#   STORM_SEQ=1         sequential rows, no spawn
#   STORM_SHADE=0       trivial leaf (no hit/shade)
#   STORM_FB_GAMMA=0    linear FB bytes (skip sqrt)
#
#   make run-tile      ADT construct / flatten / drop (storm_tile.ccs)
#   STORM_W/H           tile default 256×256 (1024² is ~1.4M nodes)
#   STORM_DROP          par (default) | seq
#   STORM_FLAT          seq (default) | par

CCC ?= $(shell test -x ../../cc/bin/ccc && echo ../../cc/bin/ccc || echo ../../out/cc/bin/ccc)
CC ?= cc
OUT_DIR = out
RAYLIB_DIR = vendor/raylib
RAYLIB_SRC = $(RAYLIB_DIR)/src
RAYLIB_LIB = $(RAYLIB_SRC)/libraylib.a
RAYLIB_TAG ?= 5.5

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  RL_LDFLAGS = -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
  RL_LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11
endif

.PHONY: all storm storm_tile raylib fetch run run-tile clean

all: storm

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

fetch: $(RAYLIB_DIR)/src/raylib.h

$(RAYLIB_DIR)/src/raylib.h:
	RAYLIB_TAG=$(RAYLIB_TAG) ./setup.sh

$(RAYLIB_LIB): $(RAYLIB_DIR)/src/raylib.h
	$(MAKE) -C $(RAYLIB_SRC) PLATFORM=PLATFORM_DESKTOP RAYLIB_LIBTYPE=STATIC \
		RAYLIB_MODULE_AUDIO=FALSE

raylib: $(RAYLIB_LIB)

$(OUT_DIR)/storm_rl.o: storm_rl.c storm_rl.h $(RAYLIB_DIR)/src/raylib.h | $(OUT_DIR)
	$(CC) -O2 -I$(RAYLIB_SRC) -c -o $@ storm_rl.c

$(OUT_DIR)/storm: storm.ccs storm_rl.h $(OUT_DIR)/storm_rl.o $(RAYLIB_LIB) | $(OUT_DIR)
	$(CCC) -O -o $@ storm.ccs --release \
		--cc-flags "-I." \
		--ld-flags "$(abspath $(OUT_DIR)/storm_rl.o) $(abspath $(RAYLIB_LIB)) $(RL_LDFLAGS)"

$(OUT_DIR)/storm_tile: storm_tile.ccs storm_world.cch storm_rl.h $(OUT_DIR)/storm_rl.o $(RAYLIB_LIB) | $(OUT_DIR)
	$(CCC) -O -o $@ storm_tile.ccs --release \
		--cc-flags "-I." \
		--ld-flags "$(abspath $(OUT_DIR)/storm_rl.o) $(abspath $(RAYLIB_LIB)) $(RL_LDFLAGS)"

storm: $(OUT_DIR)/storm

storm_tile: $(OUT_DIR)/storm_tile

run: storm
	./$(OUT_DIR)/storm

run-tile: storm_tile
	./$(OUT_DIR)/storm_tile

clean:
	rm -rf $(OUT_DIR)
	@if [ -f $(RAYLIB_SRC)/Makefile ]; then $(MAKE) -C $(RAYLIB_SRC) clean; fi
