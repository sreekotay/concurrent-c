#ifndef CC_RUNTIME_WAIT_SELECT_INTERNAL_H
#define CC_RUNTIME_WAIT_SELECT_INTERNAL_H

#include <stdatomic.h>
#include <stddef.h>

#include "fiber_internal.h"

typedef struct cc__wait_select_group {
    cc__fiber* fiber;
    _Atomic int signaled;
    _Atomic int selected_index;
} cc__wait_select_group;

static inline int cc__wait_select_try_win(void* group_ptr, size_t index) {
    if (!group_ptr) return 1;
    cc__wait_select_group* group = (cc__wait_select_group*)group_ptr;
    int sel = atomic_load_explicit(&group->selected_index, memory_order_acquire);
    if (sel == (int)index) return 1;
    if (sel != -1) return 0;
    int expected = -1;
    return atomic_compare_exchange_strong_explicit(&group->selected_index,
                                                   &expected,
                                                   (int)index,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire);
}

#endif /* CC_RUNTIME_WAIT_SELECT_INTERNAL_H */
