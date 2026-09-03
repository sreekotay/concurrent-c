#include <ccc/cc_parallel.cch>
#include <ccc/cc_channel.cch>
#include <stdio.h>
#include <stdlib.h>
#include "sched_v2.h"
#if defined(__TINYC__)
#include "cc_pthread_tls.h"
#endif

fiber_v2* cc_task_fiber_v2(CCTask t);
CCTask cc_fiber_spawn_task(void* (*fn)(void*), void* arg);
void cc_parallel_join(CCTask t);

static void cc_parallel_die(const char* msg) {
    fprintf(stderr, "cc_parallel: %s\n", msg);
    abort();
}

void cc_parallel_attach(CCParallel* h, CCTask t) {
    fiber_v2* f = cc_task_fiber_v2(t);
    if (f && h)
        sched_v2_fiber_set_par_gate(f, h);
}

void cc_parallel_wake_attached(CCParallel* h) {
    int i;
    if (!h)
        return;
    for (i = 0; i < h->nt; i++) {
        fiber_v2* f = cc_task_fiber_v2(h->tasks[i]);
        if (f)
            sched_v2_signal(f);
    }
}

int cc_parallel_current_cancelled(void) {
    fiber_v2* f = sched_v2_current_fiber();
    CCParallel* h;
    if (!f)
        return 0;
    h = (CCParallel*)sched_v2_fiber_par_gate(f);
    if (!h)
        return 0;
    return cc_atomic_load(&h->cancelled) != 0;
}

#define CC_PAR_DENY_STACK 16

#if defined(__TINYC__)
#define cc_par_deny_n (cc_rt_tls_get()->par_deny_n)
#define cc_par_deny_dest (cc_rt_tls_get()->par_deny_dest)
#define cc_par_deny_flag (cc_rt_tls_get()->par_deny_flag)
#else
static __thread int cc_par_deny_n;
static __thread CCParallel* cc_par_deny_dest[CC_PAR_DENY_STACK];
static __thread unsigned char cc_par_deny_flag[CC_PAR_DENY_STACK];
#endif

void cc_parallel_deny_enter(CCParallel* dest) {
#if defined(__TINYC__)
    if (!cc_rt_tls_get())
        return;
#endif
    if (cc_par_deny_n >= CC_PAR_DENY_STACK)
        return;
    cc_par_deny_dest[cc_par_deny_n] = dest;
    cc_par_deny_flag[cc_par_deny_n] = 0;
    cc_par_deny_n++;
}

void cc_parallel_note_denied(void) {
#if defined(__TINYC__)
    if (!cc_rt_tls_get())
        return;
#endif
    if (cc_par_deny_n <= 0)
        return;
    cc_par_deny_flag[cc_par_deny_n - 1] = 1;
}

void cc_parallel_deny_leave(void) {
#if defined(__TINYC__)
    if (!cc_rt_tls_get())
        return;
#endif
    if (cc_par_deny_n <= 0)
        return;
    if (cc_par_deny_dest[cc_par_deny_n - 1] == NULL)
        cc_par_deny_n--;
}

void cc_parallel_deny_leave_dest(CCParallel* dest) {
    int i;
#if defined(__TINYC__)
    if (!cc_rt_tls_get())
        return;
#endif
    if (!dest || cc_par_deny_n <= 0)
        return;
    for (i = cc_par_deny_n - 1; i >= 0; i--) {
        if (cc_par_deny_dest[i] == dest) {
            int k;
            for (k = i; k < cc_par_deny_n - 1; k++) {
                cc_par_deny_dest[k] = cc_par_deny_dest[k + 1];
                cc_par_deny_flag[k] = cc_par_deny_flag[k + 1];
            }
            cc_par_deny_n--;
            return;
        }
    }
}

int cc_parallel_denied_here(void) {
#if defined(__TINYC__)
    if (!cc_rt_tls_get())
        return 0;
#endif
    if (cc_par_deny_n <= 0)
        return 0;
    return cc_par_deny_flag[cc_par_deny_n - 1] != 0;
}

void cc_parallel_abort_if_denied_chan(const char* reason) {
    if (!cc_parallel_denied_here())
        return;
    fprintf(stderr, "cc_parallel: denied sibling; channel park (%s)\n",
            reason ? reason : "chan");
    abort();
}

static void cc_parallel_close_list(struct CCChan** closing, int nclose) {
    int i;
    for (i = 0; i < nclose; i++) {
        if (closing[i])
            cc_chan_close(closing[i]);
    }
}

typedef struct CCParallelLeaveHost {
    CCTask tasks[CC_PARALLEL_TASK_MAX];
    void* envs[CC_PARALLEL_TASK_MAX];
    struct CCChan* closing[CC_PARALLEL_CLOSE_MAX];
    void (*leftover_fn)(void*);
    void* leftover_ctx;
    int nt;
    int nclose;
} CCParallelLeaveHost;

static void cc_parallel_leave_empty(CCParallelLeaveHost* L) {
    int i;
    if (!L)
        return;
    for (i = 0; i < L->nt; i++) {
        cc_parallel_join(L->tasks[i]);
        free(L->envs[i]);
        L->envs[i] = NULL;
    }
    cc_parallel_close_list(L->closing, L->nclose);
    if (L->leftover_fn)
        L->leftover_fn(L->leftover_ctx);
    free(L);
}

static void* cc_parallel_leave_reaper(void* p) {
    cc_parallel_leave_empty((CCParallelLeaveHost*)p);
    return NULL;
}

static CCParallelLeaveHost* cc_parallel_leave_pack(CCParallel* h) {
    CCParallelLeaveHost* L;
    int i;
    L = (CCParallelLeaveHost*)calloc(1, sizeof(*L));
    if (!L)
        return NULL;
    L->nt = h->nt;
    L->nclose = h->nclose;
    L->leftover_fn = h->leftover_fn;
    L->leftover_ctx = h->leftover_ctx;
    for (i = 0; i < h->nt; i++) {
        L->tasks[i] = h->tasks[i];
        L->envs[i] = h->envs[i];
        h->envs[i] = NULL;
    }
    for (i = 0; i < h->nclose; i++)
        L->closing[i] = h->closing[i];
    h->nt = 0;
    h->nclose = 0;
    h->leftover_fn = NULL;
    h->leftover_ctx = NULL;
    return L;
}

CCResult_void_CCError cc_parallel_wait(CCParallel* h) {
    int i;
    if (!h)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_wait"));
    if (cc_atomic_load(&h->left))
        cc_parallel_die("wait after leave");
    cc_parallel_deny_leave_dest(h);
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
    cc_parallel_close_list(h->closing, h->nclose);
    h->nclose = 0;
    cc_atomic_store(&h->joined, 1);
    if (h->fail)
        return cc_err_CCResult_void_CCError(h->err);
    return cc_ok_CCResult_void_CCError();
}

CCResult_void_CCError cc_parallel_close(CCParallel* h, CCChanTx tx) {
    if (!h || !tx.raw)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_close"));
    if (!cc_parallel_live(h))
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_close"));
    if (h->n)
        return cc_nursery_add_closing_chan_host(h->n, tx.raw);
    if (h->nclose >= CC_PARALLEL_CLOSE_MAX)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_close"));
    h->closing[h->nclose++] = tx.raw;
    return cc_ok_CCResult_void_CCError();
}

CCResult_void_CCError cc_parallel_register_leftover(CCParallel* h, void* ctx,
                                                   void (*finish)(void*)) {
    if (!h || !finish)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_register_leftover"));
    if (!cc_parallel_live(h))
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_register_leftover"));
    if (h->leftover_fn)
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_INVALID_ARG, "cc_parallel_register_leftover"));
    h->leftover_fn = finish;
    h->leftover_ctx = ctx;
    return cc_ok_CCResult_void_CCError();
}

void cc_parallel_leave1(CCParallel* h) {
    CCParallelLeaveHost* L;
    CCTask t;
    if (!h)
        return;
    if (h->n)
        cc_parallel_die("leave on wait-for dest");
    if (cc_atomic_load(&h->joined))
        cc_parallel_die("leave after wait");
    if (cc_atomic_load(&h->left))
        cc_parallel_die("double leave");
    if (!cc_parallel_live(h))
        cc_parallel_die("leave of idle dest");
    cc_parallel_deny_leave_dest(h);
    cc_atomic_store(&h->left, 1);
    L = cc_parallel_leave_pack(h);
    if (!L)
        cc_parallel_die("leave: out of memory");
    if (L->nt == 0) {
        cc_parallel_leave_empty(L);
        return;
    }
    t = cc_fiber_spawn_task(cc_parallel_leave_reaper, L);
    if (t.kind == CC_TASK_KIND_INVALID)
        cc_parallel_die("leave: cannot detach join set");
}

CCResult_void_CCError cc_parallel_leave_with(CCParallel* h, void* ctx,
                                            void (*finish)(void*)) {
    CCResult_void_CCError r = cc_parallel_register_leftover(h, ctx, finish);
    if (!r.ok) return r;
    cc_parallel_leave1(h);
    return cc_ok_CCResult_void_CCError();
}
