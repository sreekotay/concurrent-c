#!/bin/sh
# Two translation units bump the same generation. With a counter that is
# static inside a static inline function, each unit has its own counter
# and both hand out 1: a stamp would take a changed source for unchanged.
#   ./gen_units_repro.sh [cc_gen.h]
set -e
h=$(cd "$(dirname "${1:-cc_gen.h}")" && pwd)/$(basename "${1:-cc_gen.h}")
d=$(mktemp -d)
cat > "$d/a.c" <<C
#include "$h"
uint64_t bump_a(CCGen* g) { cc_gen_bump(g); return g->v; }
C
cat > "$d/b.c" <<C
#define CC_GEN_IMPL
#include "$h"
#include <stdio.h>
uint64_t bump_a(CCGen* g);
int main(void) {
    CCGen g = {0};
    uint64_t x = bump_a(&g);
    cc_gen_bump(&g);
    printf("unit A gave %llu, unit B gave %llu: %s\n", (unsigned long long)x,
           (unsigned long long)g.v, x == g.v ? "the same number twice" : "distinct");
    return x == g.v;
}
C
cc -O2 "$d/a.c" "$d/b.c" -o "$d/t" && "$d/t"
