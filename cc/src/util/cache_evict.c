#include "cache_evict.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CC_EVICT_STAMP ".cc-evict-stamp"

typedef struct {
    char*              name;
    unsigned long long size;
    long long          mtime;
} cc__evict_entry;

static int cc__evict_cmp(const void* a, const void* b) {
    const cc__evict_entry* x = (const cc__evict_entry*)a;
    const cc__evict_entry* y = (const cc__evict_entry*)b;
    if (x->mtime < y->mtime) return -1;
    if (x->mtime > y->mtime) return 1;
    return strcmp(x->name, y->name);
}

static unsigned long long cc__evict_budget(unsigned long long fallback) {
    const char* env = getenv("CC_CACHE_MAX_MB");
    char* end = NULL;
    unsigned long long mb;
    if (!env || !env[0]) return fallback;
    mb = strtoull(env, &end, 10);
    if (end == env) return fallback;
    return mb * 1024ULL * 1024ULL;
}

static void cc__evict_warn(const char* dir, const char* what);

/* Rate limit: the stamp's mtime is the last sweep.  Bumping it before the
 * sweep (not after) keeps concurrent compiles from all sweeping at once.
 * Returns 1 to sweep, 0 if not yet due, -1 if it cannot rate-limit at all. */
static int cc__evict_due(const char* dir) {
    char stamp[1200];
    const char* env = getenv("CC_CACHE_EVICT_INTERVAL");
    long interval = 60;
    struct stat st;
    if (env && env[0]) {
        char* end = NULL;
        long v = strtol(env, &end, 10);
        if (end != env && v >= 0) interval = v;
    }
    if (snprintf(stamp, sizeof(stamp), "%s/" CC_EVICT_STAMP, dir) >= (int)sizeof(stamp))
        return 0;
    if (stat(stamp, &st) == 0 && (long long)st.st_mtime + interval > (long long)time(NULL))
        return 0;
    {
        /* Creating it stamps it: no separate utime needed. */
        FILE* f = fopen(stamp, "w");
        if (!f) {
            /* Without a stamp every compile would sweep the whole directory,
             * and a directory we cannot write to is one we cannot unlink from
             * either — so this is a genuine "cannot trim", not a no-op. */
            cc__evict_warn(dir, "stamp");
            return -1;
        }
        fclose(f);
    }
    return 1;
}

static void cc__evict_warn(const char* dir, const char* what) {
    static int warned = 0;
    if (warned) return;
    warned = 1;
    fprintf(stderr,
            "cc: warning: cannot trim cache %s (%s: %s); it will grow without bound\n",
            dir, what, strerror(errno));
}

long long cc_cache_evict(const char* dir, unsigned long long budget_bytes) {
    unsigned long long budget = cc__evict_budget(budget_bytes);
    unsigned long long total = 0;
    cc__evict_entry* ents = NULL;
    size_t n = 0, cap = 0;
    long long freed = 0;
    DIR* d;
    struct dirent* de;

    if (!dir || !dir[0] || budget == 0) return 0;
    {
        int due = cc__evict_due(dir);
        if (due <= 0) return due;
    }

    d = opendir(dir);
    if (!d) { cc__evict_warn(dir, "opendir"); return -1; }

    while ((de = readdir(d)) != NULL) {
        char path[1400];
        struct stat st;
        cc__evict_entry* grow;
        if (de->d_name[0] == '.') continue; /* `.`, `..`, and the stamp */
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= (int)sizeof(path))
            continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            grow = (cc__evict_entry*)realloc(ents, cap * sizeof(*ents));
            if (!grow) { closedir(d); goto done; }
            ents = grow;
        }
        ents[n].name = strdup(de->d_name);
        if (!ents[n].name) { closedir(d); goto done; }
        ents[n].size  = (unsigned long long)st.st_size;
        ents[n].mtime = (long long)st.st_mtime;
        total += ents[n].size;
        n++;
    }
    closedir(d);

    if (total <= budget) goto done;

    qsort(ents, n, sizeof(*ents), cc__evict_cmp);
    for (size_t i = 0; i < n && total > budget; i++) {
        char path[1400];
        if (snprintf(path, sizeof(path), "%s/%s", dir, ents[i].name) >= (int)sizeof(path))
            continue;
        if (unlink(path) != 0) {
            if (errno != ENOENT) cc__evict_warn(dir, "unlink");
            continue;
        }
        total -= ents[i].size;
        freed += (long long)ents[i].size;
    }

done:
    for (size_t i = 0; i < n; i++) free(ents[i].name);
    free(ents);
    return freed;
}
