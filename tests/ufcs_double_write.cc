#define CC_ENABLE_SHORT_NAMES
#include <std/prelude.h>

int main(void) {
    @arena {
        String s = string_new(arena);
        s.append("double\n");

        // Two UFCS calls with the same method in the same "window".
        std_out.write("A\n");
        std_out.write("B\n");
        std_out.write(s);
    }
    return 0;
}


