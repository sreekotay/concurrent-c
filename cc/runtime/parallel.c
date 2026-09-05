#include <ccc/cc_parallel.cch>
#include <ccc/cc_channel.cch>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sched_v2.h"
#if defined(__TINYC__)
#include "cc_pthread_tls.h"
#endif

fiber_v2* cc_task_fiber_v2(CCTask t);
CCTask cc_fiber_spawn_task(void* (*fn)(void*), void* arg);
void cc_parallel_join(CCTask t);

void cc_parallel_die(const char* msg) {
    fprintf(stderr, "cc_parallel: %s\n", msg);
    abort();
}

void cc_parallel_attach(CCParallel* h, CCTask t) {
    fiber_v2* f = cc_task_fiber_v2(t);
    if (f && h)
        sched_v2_fiber_set_par_gate(f, h);
}

static int cc_parallel_cap(const CCParallel* h) {
    if (!h)
        return 0;
    return h->ncap > 0 ? h->ncap : CC_PARALLEL_TASK_MAX;
}

static CCTask* cc_parallel_tasks(CCParallel* h) {
    return h && h->xtasks ? h->xtasks : h->tasks;
}

static void** cc_parallel_envs(CCParallel* h) {
    return h && h->xenvs ? h->xenvs : h->envs;
}

static void cc_parallel_env_free(CCParallel* h, void* env) {
    char* p = (char*)env;
    char* base = (char*)h;
    if (!env)
        return;
    /* Brace take-one may record a stack fallback; never free the dest. */
    if (h && p >= base && p < base + sizeof(*h))
        return;
    cc__heap_free(env);
}

/* Live-index lock. The index (tasks / envs / nt / ncap / xtasks / xenvs)
 * is read or written only under it. Nothing parks while holding it: a
 * fiber is joined only by whoever took its slot out under the lock. */
static inline void cc_par_cpu_pause(void) {
#if defined(__TINYC__)
    __asm__ __volatile__("" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static void cc_par_lock(CCParallel* h) {
    for (;;) {
        int z = 0;
        if (atomic_compare_exchange_weak_explicit(&h->lock, &z, 1,
                memory_order_acquire, memory_order_relaxed))
            return;
        cc_par_cpu_pause();
    }
}

static void cc_par_unlock(CCParallel* h) {
    atomic_store_explicit(&h->lock, 0, memory_order_release);
}

/* Live occupancy: drop finished admits. Brace pad stays; history does not.
 * Never join the fiber that is admitting (the kick). Caller holds the
 * lock; finished slots are taken out under it and joined after release
 * so the join never runs under the lock. */
#define CC_PAR_REAP_BATCH 16

typedef struct CCParReapSet {
    CCTask tasks[CC_PAR_REAP_BATCH];
    void* envs[CC_PAR_REAP_BATCH];
    int n;
} CCParReapSet;

static void cc_parallel_reap_take(CCParallel* h, CCParReapSet* rs) {
    CCTask* tasks;
    void** envs;
    fiber_v2* self;
    int i, w;
    rs->n = 0;
    if (!h)
        return;
    tasks = cc_parallel_tasks(h);
    envs = cc_parallel_envs(h);
    self = sched_v2_current_fiber();
    w = 0;
    for (i = 0; i < h->nt; i++) {
        fiber_v2* f = cc_task_fiber_v2(tasks[i]);
        if (f && f != self && rs->n < CC_PAR_REAP_BATCH &&
            sched_v2_fiber_done(f)) {
            rs->tasks[rs->n] = tasks[i];
            rs->envs[rs->n] = envs[i];
            rs->n++;
            continue;
        }
        if (w != i) {
            tasks[w] = tasks[i];
            envs[w] = envs[i];
        }
        w++;
    }
    h->nt = w;
}

static void cc_parallel_reap_finish(CCParallel* h, CCParReapSet* rs) {
    int i;
    for (i = 0; i < rs->n; i++) {
        cc_parallel_join(rs->tasks[i]);
        cc_parallel_env_free(h, rs->envs[i]);
    }
    rs->n = 0;
}

/* Drop slot i without joining. Shift the tail down. */
static void cc_parallel_remove_at(CCParallel* h, int i) {
    CCTask* tasks;
    void** envs;
    int k;
    if (!h || i < 0 || i >= h->nt)
        return;
    tasks = cc_parallel_tasks(h);
    envs = cc_parallel_envs(h);
    for (k = i; k < h->nt - 1; k++) {
        tasks[k] = tasks[k + 1];
        envs[k] = envs[k + 1];
    }
    h->nt--;
}

static void cc_parallel_release_index(CCParallel* h) {
    if (!h)
        return;
    if (h->xtasks)
        free(h->xtasks);
    if (h->xenvs)
        free(h->xenvs);
    h->xtasks = NULL;
    h->xenvs = NULL;
    h->ncap = 0;
}

/* Grow the live index. 32 is the inline pad, not a ceiling. */
static int cc_parallel_grow(CCParallel* h, int need) {
    int cap, ncap;
    CCTask* nt;
    void** ne;
    if (!h)
        return 0;
    cap = cc_parallel_cap(h);
    if (need <= cap)
        return 1;
    ncap = cap;
    while (ncap < need) {
        if (ncap > 1000000000 / 2)
            return 0;
        ncap *= 2;
    }
    nt = (CCTask*)malloc((size_t)ncap * sizeof(CCTask));
    ne = (void**)malloc((size_t)ncap * sizeof(void*));
    if (!nt || !ne) {
        free(nt);
        free(ne);
        return 0;
    }
    if (h->nt) {
        memcpy(nt, cc_parallel_tasks(h), (size_t)h->nt * sizeof(CCTask));
        memcpy(ne, cc_parallel_envs(h), (size_t)h->nt * sizeof(void*));
    }
    if (h->xtasks)
        free(h->xtasks);
    if (h->xenvs)
        free(h->xenvs);
    h->xtasks = nt;
    h->xenvs = ne;
    h->ncap = ncap;
    return 1;
}

CCResult_void_CCError cc_parallel_admit_ok(CCParallel* h) {
    if (!h || !cc_parallel_live(h))
        cc_parallel_die("admit on idle dest");
    if (cc_atomic_load(&h->cancelled))
        return cc_err_CCResult_void_CCError(
            CC_ERROR(CC_ERR_CANCELLED, "admit after cancel"));
    return cc_ok_CCResult_void_CCError();
}

void cc_parallel_admit(CCParallel* h, CCTask t, void* env) {
    CCTask* tasks;
    void** envs;
    CCParReapSet rs;
    if (!h)
        cc_parallel_die("admit on idle dest");
    if (t.kind == CC_TASK_KIND_INVALID)
        cc_parallel_die("admit denied");
    cc_par_lock(h);
    /* Liveness is decided under the lock: wait sets joined under the same
     * hold that saw nt == 0, so an admit cannot land on a joined dest. */
    if (!cc_parallel_live(h))
        cc_parallel_die("admit on idle dest");
    cc_parallel_reap_take(h, &rs);
    if (!cc_parallel_grow(h, h->nt + 1))
        cc_parallel_die("admit: oom");
    tasks = cc_parallel_tasks(h);
    envs = cc_parallel_envs(h);
    tasks[h->nt] = t;
    envs[h->nt] = env;
    cc_parallel_attach(h, t);
    h->nt++;
    cc_par_unlock(h);
    cc_parallel_reap_finish(h, &rs);
}

void cc_parallel_wake_attached(CCParallel* h) {
    CCTask* tasks;
    int i;
    if (!h)
        return;
    cc_par_lock(h);
    tasks = cc_parallel_tasks(h);
    for (i = 0; i < h->nt; i++) {
        fiber_v2* f = cc_task_fiber_v2(tasks[i]);
        if (f)
            sched_v2_signal(f);
    }
    cc_par_unlock(h);
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
    CCTask* tasks;
    void** envs;
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
        cc__heap_free(L->envs[i]);
        L->envs[i] = NULL;
    }
    free(L->tasks);
    free(L->envs);
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
    CCTask* tasks;
    void** envs;
    L = (CCParallelLeaveHost*)calloc(1, sizeof(*L));
    if (!L)
        return NULL;
    L->nt = h->nt;
    L->nclose = h->nclose;
    L->leftover_fn = h->leftover_fn;
    L->leftover_ctx = h->leftover_ctx;
    if (h->nt) {
        fiber_v2* self = sched_v2_current_fiber();
        int w = 0;
        L->tasks = (CCTask*)malloc((size_t)h->nt * sizeof(CCTask));
        L->envs = (void**)malloc((size_t)h->nt * sizeof(void*));
        if (!L->tasks || !L->envs) {
            free(L->tasks);
            free(L->envs);
            free(L);
            return NULL;
        }
        tasks = cc_parallel_tasks(h);
        envs = cc_parallel_envs(h);
        for (i = 0; i < h->nt; i++) {
            fiber_v2* f = cc_task_fiber_v2(tasks[i]);
            if (f && f == self) {
                cc_parallel_env_free(h, envs[i]);
                envs[i] = NULL;
                continue;
            }
            L->tasks[w] = tasks[i];
            L->envs[w] = envs[i];
            envs[i] = NULL;
            w++;
        }
        L->nt = w;
        if (w == 0) {
            free(L->tasks);
            free(L->envs);
            L->tasks = NULL;
            L->envs = NULL;
        }
    }
    for (i = 0; i < h->nclose; i++)
        L->closing[i] = h->closing[i];
    cc_parallel_release_index(h);
    h->nt = 0;
    h->nclose = 0;
    h->leftover_fn = NULL;
    h->leftover_ctx = NULL;
    return L;
}

void cc_parallel_invalidate(CCParallel* h) {
    if (!h || !cc_parallel_live(h))
        return;
    (void)cc__parallel_cancel_tree(h);
    (void)cc_parallel_wait(h);
}

CCResult_void_CCError cc_parallel_wait(CCParallel* h) {
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
    /* The kick may still be admitting. Take slot 0 (the oldest — the kick)
     * out of the index under the lock, then join it with the lock released;
     * whoever holds a slot is the only one who joins it, so a concurrent
     * admit's reap cannot join the same CCTask (a second join parks on a
     * recycled fiber — often this one). Admits that land during the join
     * are seen on the next pass. `joined` is set under the hold that saw
     * nt == 0, so no admit can land on a joined dest. */
    for (;;) {
        CCTask t;
        void* env;
        fiber_v2* f;
        fiber_v2* self = sched_v2_current_fiber();
        cc_par_lock(h);
        if (h->nt == 0) {
            cc_parallel_release_index(h);
            cc_atomic_store(&h->joined, 1);
            cc_par_unlock(h);
            break;
        }
        t = cc_parallel_tasks(h)[0];
        env = cc_parallel_envs(h)[0];
        cc_parallel_remove_at(h, 0);
        cc_par_unlock(h);
        f = cc_task_fiber_v2(t);
        if (f && f == self) {
            cc_parallel_env_free(h, env);
            continue;
        }
        cc_parallel_join(t);
        cc_parallel_env_free(h, env);
    }
    cc_parallel_close_list(h->closing, h->nclose);
    h->nclose = 0;
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
    /* Snapshot and go dead under one hold: no admit lands between them. */
    cc_par_lock(h);
    cc_atomic_store(&h->left, 1);
    L = cc_parallel_leave_pack(h);
    cc_par_unlock(h);
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
