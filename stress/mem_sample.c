/*
 * Host-C implementation of stress/mem_sample.h.
 * macOS: phys_footprint. Linux: VmRSS + VmHWM + optional cgroup peak.
 */
#include "mem_sample.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

static size_t stress__read_size_file(const char *path) {
    FILE *f = fopen(path, "r");
    unsigned long long v = 0;
    if (!f) return 0;
    if (fscanf(f, "%llu", &v) != 1) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return (size_t)v;
}

static void stress__sample_cgroup(StressMemSample *s) {
    size_t peak = stress__read_size_file("/sys/fs/cgroup/memory.peak");
    size_t cur = stress__read_size_file("/sys/fs/cgroup/memory.current");
    if (!peak)
        peak = stress__read_size_file("/sys/fs/cgroup/memory/memory.max_usage_in_bytes");
    if (!cur)
        cur = stress__read_size_file("/sys/fs/cgroup/memory/memory.usage_in_bytes");
    if (peak || cur) {
        s->cgroup_peak = peak;
        s->cgroup_current = cur;
        s->cgroup_ok = 1;
    }
}

StressMemSample stress_mem_sample(void) {
    StressMemSample s;
    memset(&s, 0, sizeof(s));
#if defined(__APPLE__)
    {
        task_vm_info_data_t info;
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
            s.current = (size_t)info.phys_footprint;
            s.ok = 1;
        }
    }
#elif defined(__linux__)
    {
        FILE *f = fopen("/proc/self/status", "r");
        char line[256];
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                unsigned long kb = 0;
                if (sscanf(line, "VmRSS: %lu", &kb) == 1) {
                    s.current = (size_t)kb * 1024;
                    s.ok = 1;
                } else if (sscanf(line, "VmHWM: %lu", &kb) == 1) {
                    s.hwm = (size_t)kb * 1024;
                    s.hwm_ok = 1;
                }
            }
            fclose(f);
        }
        stress__sample_cgroup(&s);
    }
#else
    (void)stress__sample_cgroup;
#endif
    return s;
}

size_t stress_mem_best_peak(const StressMemSample *now, size_t sampled_peak) {
    size_t p = sampled_peak;
    if (now->hwm_ok && now->hwm > p) p = now->hwm;
    if (now->cgroup_ok && now->cgroup_peak > p) p = now->cgroup_peak;
    return p;
}
