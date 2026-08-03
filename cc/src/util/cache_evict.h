#ifndef CC_UTIL_CACHE_EVICT_H
#define CC_UTIL_CACHE_EVICT_H

#include <stddef.h>

/* Trim a content-addressed cache directory to a byte budget.
 *
 * Every on-disk cache here is keyed by a hash that folds in mtimes, so an
 * edited input mints a new entry and the old one is dead the moment it is
 * written.  Nothing else deletes them, so the directory is a monotonic record
 * of every file ever compiled; left alone it fills the disk.
 *
 * Deletion is oldest-mtime-first.  Entries are written once and never
 * rewritten, so that is insertion order rather than true LRU: a hot entry can
 * be evicted while a cold newer one survives.  The cost of that is one cache
 * miss, and the budget is sized so a full build's working set fits.
 *
 * `budget_bytes` is the default; `CC_CACHE_MAX_MB` overrides it per directory
 * (0 disables the cap).  Scanning on every store would cost a `readdir` of the
 * whole directory per compile, so a stamp file rate-limits the sweep to once
 * per `CC_CACHE_EVICT_INTERVAL` seconds (default 60).
 *
 * Returns bytes freed, 0 if under budget or rate-limited, -1 if the sweep
 * could not run.  A -1 means the cache is once again growing without bound,
 * which is the failure this exists to prevent, so it also warns to stderr
 * (once per process) rather than leaving the caller to infer it from a
 * directory that quietly keeps growing.
 */
long long cc_cache_evict(const char* dir, unsigned long long budget_bytes);

#endif /* CC_UTIL_CACHE_EVICT_H */
