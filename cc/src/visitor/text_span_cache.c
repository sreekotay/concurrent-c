#include "text_span.h"

#include <stdlib.h>
#include <string.h>

/* TU-lifetime cache for cc_offset_for_logical_line: one #line ledger scan
 * instead of a full-buffer walk per AST node. */

typedef struct {
    char* key; /* basename, "" (pre-#line), or "*" (any file) */
    int line;
    size_t off;
    int used;
} CCLogicalLineSlot;

typedef struct {
    const char* src;
    size_t n;
    CCLogicalLineSlot* slots;
    int cap; /* power of two */
    int count;
} CCLogicalLineCache;

static CCLogicalLineCache g_ll_cache;

static const char* cc__basename_view(const char* path, size_t* out_len) {
    const char* slash;
    if (!path) {
        if (out_len) *out_len = 0;
        return "";
    }
    slash = strrchr(path, '/');
    if (slash) {
        if (out_len) *out_len = strlen(slash + 1);
        return slash + 1;
    }
    if (out_len) *out_len = strlen(path);
    return path;
}

static unsigned cc__ll_hash(const char* key, size_t key_len, int line) {
    unsigned h = 5381u ^ (unsigned)line * 2654435761u;
    size_t i;
    for (i = 0; i < key_len; i++)
        h = ((h << 5) + h) + (unsigned char)key[i];
    return h;
}

static void cc__ll_cache_clear(void) {
    int i;
    if (g_ll_cache.slots) {
        for (i = 0; i < g_ll_cache.cap; i++)
            free(g_ll_cache.slots[i].key);
        free(g_ll_cache.slots);
    }
    g_ll_cache.slots = NULL;
    g_ll_cache.cap = 0;
    g_ll_cache.count = 0;
    g_ll_cache.src = NULL;
    g_ll_cache.n = 0;
}

static int cc__ll_cache_grow(void) {
    int ncap = g_ll_cache.cap ? g_ll_cache.cap * 2 : 1024;
    CCLogicalLineSlot* nslots = (CCLogicalLineSlot*)calloc((size_t)ncap, sizeof(CCLogicalLineSlot));
    int i;
    if (!nslots) return -1;
    for (i = 0; i < g_ll_cache.cap; i++) {
        CCLogicalLineSlot* s = &g_ll_cache.slots[i];
        unsigned h;
        int j;
        if (!s->used) continue;
        h = cc__ll_hash(s->key, strlen(s->key), s->line);
        j = (int)(h & (unsigned)(ncap - 1));
        while (nslots[j].used)
            j = (j + 1) & (ncap - 1);
        nslots[j] = *s; /* moves key pointer */
        s->key = NULL;
    }
    free(g_ll_cache.slots);
    g_ll_cache.slots = nslots;
    g_ll_cache.cap = ncap;
    return 0;
}

/* Insert or overwrite (last wins). */
static int cc__ll_cache_set(const char* key, size_t key_len, int line, size_t off) {
    unsigned h;
    int j;
    char* k;
    if (line <= 0) return 0;
    if (g_ll_cache.cap == 0 || g_ll_cache.count * 2 >= g_ll_cache.cap) {
        if (cc__ll_cache_grow() != 0) return -1;
    }
    h = cc__ll_hash(key, key_len, line);
    j = (int)(h & (unsigned)(g_ll_cache.cap - 1));
    for (;;) {
        CCLogicalLineSlot* s = &g_ll_cache.slots[j];
        if (!s->used) {
            k = (char*)malloc(key_len + 1);
            if (!k) return -1;
            memcpy(k, key, key_len);
            k[key_len] = '\0';
            s->key = k;
            s->line = line;
            s->off = off;
            s->used = 1;
            g_ll_cache.count++;
            return 0;
        }
        if (s->line == line && s->key &&
            strlen(s->key) == key_len && memcmp(s->key, key, key_len) == 0) {
            s->off = off;
            return 0;
        }
        j = (j + 1) & (g_ll_cache.cap - 1);
    }
}

static size_t cc__ll_cache_get(const char* key, int line) {
    unsigned h;
    int j;
    size_t key_len;
    if (!key || !g_ll_cache.slots || line <= 0) return (size_t)-1;
    key_len = strlen(key);
    h = cc__ll_hash(key, key_len, line);
    j = (int)(h & (unsigned)(g_ll_cache.cap - 1));
    for (;;) {
        CCLogicalLineSlot* s = &g_ll_cache.slots[j];
        if (!s->used) return (size_t)-1;
        if (s->line == line && s->key &&
            strlen(s->key) == key_len && memcmp(s->key, key, key_len) == 0)
            return s->off;
        j = (j + 1) & (g_ll_cache.cap - 1);
    }
}

static int cc__ll_cache_rebuild(const char* s, size_t n) {
    char cur_file[1024] = {0};
    int cur_line = 1;
    size_t i = 0;
    cc__ll_cache_clear();
    g_ll_cache.src = s;
    g_ll_cache.n = n;
    if (!s) return -1;
    while (i < n) {
        size_t j = i;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j < n && s[j] == '#') {
            size_t k = j + 1;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k + 4 <= n && strncmp(s + k, "line", 4) == 0) k += 4;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k < n && s[k] >= '0' && s[k] <= '9') {
                int ln = 0;
                while (k < n && s[k] >= '0' && s[k] <= '9') ln = ln * 10 + (s[k++] - '0');
                while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                if (k < n && s[k] == '"') {
                    size_t fs = ++k, o = 0;
                    while (k < n && s[k] != '"') k++;
                    for (size_t m = fs; m < k && o + 1 < sizeof(cur_file); m++)
                        cur_file[o++] = s[m];
                    cur_file[o] = '\0';
                }
                cur_line = ln;
                while (i < n && s[i] != '\n') i++;
                if (i < n) i++;
                continue;
            }
        }
        {
            size_t blen = 0;
            const char* base = cc__basename_view(cur_file, &blen);
            if (cc__ll_cache_set(base, blen, cur_line, i) != 0) return -1;
            if (cc__ll_cache_set("*", 1, cur_line, i) != 0) return -1;
        }
        while (i < n && s[i] != '\n') i++;
        if (i < n) i++;
        cur_line++;
    }
    return 0;
}

static void cc__ll_cache_ensure(const char* s, size_t n) {
    if (g_ll_cache.src == s && g_ll_cache.n == n && g_ll_cache.slots) return;
    (void)cc__ll_cache_rebuild(s, n);
}

size_t cc_offset_for_logical_line(const char* s, size_t n,
                                  const char* want_file, int want_line) {
    size_t best = (size_t)-1;
    size_t a, b;
    if (!s || want_line <= 0) return 0;
    cc__ll_cache_ensure(s, n);
    if (!g_ll_cache.slots)
        return cc__offset_of_line_1based(s, n, want_line);
    if (!want_file || !want_file[0]) {
        best = cc__ll_cache_get("*", want_line);
    } else {
        size_t blen = 0;
        const char* base = cc__basename_view(want_file, &blen);
        char tmp[512];
        if (blen >= sizeof(tmp)) blen = sizeof(tmp) - 1;
        memcpy(tmp, base, blen);
        tmp[blen] = '\0';
        a = cc__ll_cache_get(tmp, want_line);
        b = cc__ll_cache_get("", want_line); /* pre-#line matches any file */
        if (a != (size_t)-1) best = a;
        if (b != (size_t)-1 && (best == (size_t)-1 || b > best)) best = b;
    }
    if (best != (size_t)-1) return best;
    return cc__offset_of_line_1based(s, n, want_line);
}

size_t cc_offset_for_logical_line_col(const char* s, size_t n,
                                      const char* want_file,
                                      int want_line, int col_no) {
    size_t loff = cc_offset_for_logical_line(s, n, want_file, want_line);
    size_t off;
    if (col_no <= 1) return loff;
    off = loff + (size_t)(col_no - 1);
    if (off > n) off = n;
    return off;
}
