/* Shared TSan helpers for runtime components */
#ifndef CC_TSAN_HELPERS_H
#define CC_TSAN_HELPERS_H

#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
extern void __tsan_acquire(void* addr);
extern void __tsan_release(void* addr);
extern void __tsan_write_range(void* addr, size_t size);
extern void* __tsan_create_fiber(unsigned flags);
extern void __tsan_switch_to_fiber(void* fiber, unsigned flags);

#define TSAN_ACQUIRE(addr) do { if (addr) __tsan_acquire(addr); } while (0)
#define TSAN_RELEASE(addr) do { if (addr) __tsan_release(addr); } while (0)
#define TSAN_WRITE_RANGE(addr, size) do { if (addr) __tsan_write_range(addr, size); } while (0)
#define TSAN_FIBER_CREATE() __tsan_create_fiber(0)
#define TSAN_FIBER_SWITCH(f) do { if (f) __tsan_switch_to_fiber((f), 0); } while (0)
#else
#define TSAN_ACQUIRE(addr) ((void)0)
#define TSAN_RELEASE(addr) ((void)0)
#define TSAN_WRITE_RANGE(addr, size) ((void)0)
#define TSAN_FIBER_CREATE() NULL
#define TSAN_FIBER_SWITCH(f) ((void)0)
#endif

#endif /* CC_TSAN_HELPERS_H */
/*
 * Shared TSan (ThreadSanitizer) macro definitions for runtime.
 * 
 * These macros provide synchronization annotations for TSan to understand
 * the happens-before relationships in our fiber scheduler and closure system.
 */

#ifndef CC_TSAN_HELPERS_H
#define CC_TSAN_HELPERS_H

#include <stddef.h>  /* for size_t */

#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
/* TSan is enabled - use real annotations */
extern void __tsan_acquire(void* addr);
extern void __tsan_release(void* addr);
extern void __tsan_write_range(void* addr, size_t size);
extern void* __tsan_create_fiber(unsigned flags);
extern void __tsan_switch_to_fiber(void* fiber, unsigned flags);

/* Basic acquire/release synchronization macros.
 * NULL-checking ensures we don't call TSan functions with NULL addresses. */
#define TSAN_ACQUIRE(addr) do { if (addr) __tsan_acquire(addr); } while(0)
#define TSAN_RELEASE(addr) do { if (addr) __tsan_release(addr); } while(0)

/* Write range annotation - used when reusing stack memory for pooled fibers */
#define TSAN_WRITE_RANGE(addr, size) do { if (addr && size > 0) __tsan_write_range(addr, size); } while(0)

/* Fiber context switching annotations */
#define TSAN_FIBER_CREATE() __tsan_create_fiber(0)
#define TSAN_FIBER_SWITCH(f) do { if (f) __tsan_switch_to_fiber((f), 0); } while (0)

#else
/* TSan is disabled - macros are no-ops */
#define TSAN_ACQUIRE(addr) ((void)0)
#define TSAN_RELEASE(addr) ((void)0)
#define TSAN_WRITE_RANGE(addr, size) ((void)0)
#define TSAN_FIBER_CREATE() NULL
#define TSAN_FIBER_SWITCH(f) ((void)0)
#endif

#endif /* CC_TSAN_HELPERS_H */
