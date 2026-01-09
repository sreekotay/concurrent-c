#define CC_ENABLE_SHORT_NAMES
#include <std/prelude.h>

// UFCS-style hello: requires CC compiler UFCS lowering.
int main(void) {
    Arena a = cc_heap_arena(kilobytes(1));
    String s1 = string_new(&a);
    @arena {
        String s = string_new(arena);
        s.append("Hello, ");
        s.append("Concurrent-C via UFCS! - 2\n");
        std_out.write(s);
    }
    return 0;
}

