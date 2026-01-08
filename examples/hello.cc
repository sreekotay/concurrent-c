#define CC_ENABLE_SHORT_NAMES
#include <std/prelude.h>

// CC-flavored hello using stdlib String builder + UFCS style.
int main() {
    CCArena a = cc_heap_arena(kilobytes(1));
    CCString s = string_new(&a);
    string_append(&s, "Hello, ");
    string_append(&s, "Concurrent-C via UFCS!\\n");
    CCSlice sl = string_as_slice(&s);
    cc_std_out_write(sl);
    return 0;
}

