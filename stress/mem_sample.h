/*
 * Process / cgroup memory sampling for stress benches (declarations).
 * Implementation: stress/mem_sample.c (host-compiled — keep #if out of
 * includes; shadow_lower blanks platform #if blocks in pulled-in headers).
 *
 * Prefer arena gross/ovf for reclaim correctness; use these for peak color
 * and Linux scoreboards (VmHWM / cgroup memory.peak).
 */
#ifndef STRESS_MEM_SAMPLE_H
#define STRESS_MEM_SAMPLE_H

#include <stddef.h>

typedef struct {
    size_t current;      /* RSS / footprint now */
    size_t hwm;          /* process high-water if known, else 0 */
    size_t cgroup_peak;  /* cgroup memory.peak if known, else 0 */
    size_t cgroup_current;
    int ok;
    int hwm_ok;
    int cgroup_ok;
} StressMemSample;

StressMemSample stress_mem_sample(void);
size_t stress_mem_best_peak(const StressMemSample *now, size_t sampled_peak);

#endif /* STRESS_MEM_SAMPLE_H */
