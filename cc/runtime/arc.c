#include <ccc/cc_arc.cch>
#include <stdlib.h>

CCArc cc_arc_from_ptr(void* ptr, CCArcDropFn drop_fn) {
    CCArc a = cc_arc_null();
    CCArcCtrl* c;
    if (!ptr) return a;
    c = (CCArcCtrl*)malloc(sizeof(CCArcCtrl));
    if (!c) {
        if (drop_fn) drop_fn(ptr);
        return a;
    }
    c->strong = 1;
    c->drop_fn = drop_fn;
    c->ptr = ptr;
    a.ctrl = c;
    return a;
}

void cc_arc_drop(CCArc* a) {
    CCArcCtrl* c;
    int prev;
    if (!a || !a->ctrl) return;
    c = a->ctrl;
    a->ctrl = NULL;
    prev = cc_atomic_fetch_sub(&c->strong, 1);
    if (prev == 1) {
        if (c->drop_fn) c->drop_fn(c->ptr);
        free(c);
    }
}
