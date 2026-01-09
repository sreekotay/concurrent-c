#define CC_ENABLE_SHORT_NAMES
#include <std/prelude.h>

int main(void) {
    @arena(ar) {
        String s = string_new(ar);
        s.append("stderr line1\n");
        s.append("stderr line2\n");
        std_err.write("stderr literal\n");
        std_err.write(s);
    }
    return 0;
}

