/*
 * CPU pause / cycle-counter helpers.
 *
 * Host TCC on aarch64 rejects all inline asm ("ARM asm not implemented"),
 * including empty compiler-barrier asm. Under __TINYC__, use atomics /
 * clock_gettime instead. GCC/Clang keep the native asm paths in each .c file.
 */
#ifndef CC_CPU_PORT_H
#define CC_CPU_PORT_H

#if defined(__TINYC__)

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

static inline void cc_cpu_compiler_barrier(void) {
    atomic_signal_fence(memory_order_seq_cst);
}

static inline void cc_cpu_pause_port(void) {
    cc_cpu_compiler_barrier();
}

static inline void cc_cpu_yield_port(void) {
    cc_cpu_compiler_barrier();
}

static inline uint64_t cc_cpu_counter_port(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif /* __TINYC__ */

#endif /* CC_CPU_PORT_H */
