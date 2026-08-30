#include <ccc/cc_parallel.cch>
#include <stdlib.h>

CCResult_void_CCError cc_parallel_wait(CCParallel* h) {
    int i;
    if (!h)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_wait"));
    if (cc_atomic_load(&h->joined))
        return cc_ok_CCResult_void_CCError();
    if (h->n) {
        CCResult_void_CCError wr = cc_nursery_wait_host(h->n);
        if (!wr.ok) return wr;
    }
    for (i = 0; i < h->nt; i++) {
        cc_parallel_join(h->tasks[i]);
        free(h->envs[i]);
        h->envs[i] = NULL;
    }
    h->nt = 0;
    cc_atomic_store(&h->joined, 1);
    if (h->fail)
        return cc_err_CCResult_void_CCError(h->err);
    return cc_ok_CCResult_void_CCError();
}
