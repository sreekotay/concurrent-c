#ifndef CC_RUNTIME_CHANNEL_WAIT_INTERNAL_H
#define CC_RUNTIME_CHANNEL_WAIT_INTERNAL_H

#include <stddef.h>

#include "fiber_internal.h"

typedef struct CCChan CCChan;

enum {
    CC__CHAN_WAIT_PUBLISHED = 0,
    CC__CHAN_WAIT_DATA = 1,
    CC__CHAN_WAIT_CLOSE = 2,
    CC__CHAN_WAIT_SIGNAL = 3,
};

int cc__chan_publish_recv_wait_select(CCChan* ch,
                                      cc__fiber_wait_node* node,
                                      void* out_value,
                                      uint64_t wait_ticket,
                                      void* select_group,
                                      size_t select_index);

int cc__chan_finish_recv_wait_select(CCChan* ch, cc__fiber_wait_node* node);

#endif /* CC_RUNTIME_CHANNEL_WAIT_INTERNAL_H */
