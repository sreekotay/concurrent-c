#define CC_ENABLE_SHORT_NAMES
#include "cc/include/std/prelude.cch"
#include <stdio.h>

int main(void) {
    CCChan* ch = cc_chan_create(1);
    if (!ch) return 1;

    int value = 123;
    // First send establishes elem_size = sizeof(int), so send_take should fail.
    if (cc_chan_send(ch, &value, sizeof(int)) != 0) { cc_chan_free(ch); return 2; }

    int rc = cc_chan_send_take(ch, (void*)(uintptr_t)0xdeadbeef);
    cc_chan_free(ch);

    if (rc == 0) return 3; // should reject pointer send_take on value channel
    printf("channel send_take reject ok\\n");
    return 0;
}

