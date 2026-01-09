#define CC_ENABLE_SHORT_NAMES
#include <std/prelude.h>

// UFCS-style hello: requires CC compiler UFCS lowering.
int main(void) {
    @arena {
        String s = string_new(arena);
        s.append("Hello, ");
        s.append("Concurrent-C via UFCS! - 2\n");
        std_out.write(s);
    }
    return 0;
}

