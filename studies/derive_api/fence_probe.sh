#!/bin/sh
# The factory declares each instance's view in the code it emits
# (cc_source.cch: `@typeview on ${mangled} { r: ^value, ^g; }`), so every
# Source::[T] refuses a store that would skip the generation's bump, and
# its own methods still write it. One build per case: the checker reports
# the first refusal in a unit and stays quiet after it, and it reads the
# source before the C preprocessor, so #if cannot pick a case.
# via_field is the gap: a store through a field chain (h.n.value) is not
# checked, and builds.
#   ./fence_probe.sh [ccc]
C=${1:-ccc}
here=$(cd "$(dirname "$0")" && pwd)
d=$(mktemp -d)
cp "$here/cc_gen.h" "$here/cc_source.cch" "$d/"
probe() {
    cat > "$d/$1.ccs" <<C
#include <ccc/std/prelude.cch>
#define CC_GEN_IMPL
#include "cc_source.cch"
int main(void) {
    Source::[size_t] s = {0};
    s.set(3);
    $2
    return (int)s.val();
}
C
    if "$C" "$d/$1.ccs" -o "$d/$1" > "$d/$1.log" 2>&1; then
        "$d/$1"; echo "$1: builds, exits $?"
    else
        echo "$1: refused: $(grep -o "does not allow[^\"]*" "$d/$1.log" | head -1)"
    fi
}
probe store 's.value = 1;'
probe read_gen '(void)s.g.v;'
probe methods ''
probe via_field 'struct { Source::[size_t] n; } h = {0}; h.n.value = 1; (void)h;'
