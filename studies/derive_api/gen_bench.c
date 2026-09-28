/* Cost of a generation bump: one global atomic, a plain per-object
 * increment, and per-thread blocks of 1024 from the global counter.
 * gcc -O2 -pthread gen_bench.c -o gen_bench && ./gen_bench */
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
static _Atomic uint64_t next;
typedef struct { uint64_t val; uint64_t g; char pad[48]; } Src;
static Src srcs[8];
enum { N = 50000000 };
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec*1e-9; }
static void* glob(void* p){ Src* s=p; for (uint64_t i=0;i<N;i++){ s->val=i; s->g = atomic_fetch_add_explicit(&next,1,memory_order_relaxed)+1; } return 0; }
static void* local(void* p){ volatile Src* s=p; for (uint64_t i=0;i<N;i++){ s->val=i; s->g = s->g+1; } return 0; }
static _Thread_local uint64_t tl_next, tl_end;
static inline uint64_t blk_next(void){ if (tl_next==tl_end){ tl_next=atomic_fetch_add_explicit(&next,1024,memory_order_relaxed)+1; tl_end=tl_next+1024; } return tl_next++; }
static void* block(void* p){ volatile Src* s=p; for (uint64_t i=0;i<N;i++){ s->val=i; s->g = blk_next(); } return 0; }
static void run(const char* nm, void*(*f)(void*), int th){ pthread_t t[8]; double a=now(); for(int i=0;i<th;i++) pthread_create(&t[i],0,f,&srcs[i]); for(int i=0;i<th;i++) pthread_join(t[i],0); printf("%-6s %d thr: %.2f ns/store\n", nm, th, (now()-a)*1e9/N); }
int main(void){ run("global",glob,1); run("local",local,1); run("block",block,1); run("global",glob,4); run("block",block,4); run("local",local,4); return 0; }
