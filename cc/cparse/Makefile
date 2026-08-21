# Standalone step-1 C parser. No shadow, no libtcc.
CC ?= cc
CFLAGS ?= -O0 -g -Wall -Wextra -std=c11
OUTDIR ?= ../../out/cc/bin
BIN := $(OUTDIR)/cparse-dump

.PHONY: all clean

all: $(BIN)

$(BIN): cparse.c expand.c cparse_dump.c cparse.h
	mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ cparse.c expand.c cparse_dump.c

clean:
	rm -f $(BIN)
