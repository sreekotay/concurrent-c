#define CC_ENABLE_SHORT_NAMES
#include "std/prelude.h"

int main(void) {
    @arena {
        String s = string_new(arena);
        s.append("nested\n");

        // Outer UFCS call spans multiple lines and contains an inner UFCS call.
        std_out.write(
            s.as_slice()
        );
    }
    return 0;
}


