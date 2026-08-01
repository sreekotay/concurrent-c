#include "pass_callee_occ.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"

typedef struct {
    char* name;
    size_t* offs;
    int count;
    int cap;
} CCCalleeOccBucket;

struct CCCalleeOccIndex {
    CCCalleeOccBucket* buckets;
    int n;
    int cap;
};

static size_t cc__callee_skip_inert(const char* s, size_t n, size_t i) {
    if (!s || i >= n) return i;
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
        i += 2;
        while (i < n && s[i] != '\n') i++;
        return i;
    }
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
        if (i + 1 < n) i += 2;
        return i;
    }
    if (s[i] == '"' || s[i] == '\'') {
        char q = s[i++];
        while (i < n) {
            if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
            if (s[i] == q) { i++; break; }
            i++;
        }
        return i;
    }
    return i;
}

static CCCalleeOccBucket* cc__callee_bucket(CCCalleeOccIndex* idx,
                                            const char* name, size_t name_n) {
    int i;
    CCCalleeOccBucket* b;
    char* nm;
    if (!idx || !name || name_n == 0) return NULL;
    for (i = 0; i < idx->n; i++) {
        if (idx->buckets[i].name &&
            strlen(idx->buckets[i].name) == name_n &&
            memcmp(idx->buckets[i].name, name, name_n) == 0)
            return &idx->buckets[i];
    }
    if (idx->n >= idx->cap) {
        int ncap = idx->cap ? idx->cap * 2 : 64;
        CCCalleeOccBucket* nb = (CCCalleeOccBucket*)realloc(
            idx->buckets, (size_t)ncap * sizeof(CCCalleeOccBucket));
        if (!nb) return NULL;
        idx->buckets = nb;
        idx->cap = ncap;
    }
    b = &idx->buckets[idx->n];
    memset(b, 0, sizeof(*b));
    nm = (char*)malloc(name_n + 1);
    if (!nm) return NULL;
    memcpy(nm, name, name_n);
    nm[name_n] = '\0';
    b->name = nm;
    idx->n++;
    return b;
}

static int cc__callee_bucket_push(CCCalleeOccBucket* b, size_t off) {
    size_t* no;
    if (!b) return -1;
    if (b->count >= b->cap) {
        int ncap = b->cap ? b->cap * 2 : 8;
        no = (size_t*)realloc(b->offs, (size_t)ncap * sizeof(size_t));
        if (!no) return -1;
        b->offs = no;
        b->cap = ncap;
    }
    b->offs[b->count++] = off;
    return 0;
}

static const CCCalleeOccBucket* cc__callee_bucket_find(const CCCalleeOccIndex* idx,
                                                      const char* name) {
    int i;
    if (!idx || !name) return NULL;
    for (i = 0; i < idx->n; i++) {
        if (idx->buckets[i].name && strcmp(idx->buckets[i].name, name) == 0)
            return &idx->buckets[i];
    }
    return NULL;
}

CCCalleeOccIndex* cc_callee_occ_index_build(const char* src, size_t len,
                                            CCCalleeIsDeclaratorFn is_decl) {
    CCCalleeOccIndex* idx;
    size_t j = 0;
    if (!src) return NULL;
    idx = (CCCalleeOccIndex*)calloc(1, sizeof(*idx));
    if (!idx) return NULL;
    while (j < len) {
        size_t next = cc__callee_skip_inert(src, len, j);
        size_t name_s, name_n, after;
        CCCalleeOccBucket* b;
        if (next != j) { j = next; continue; }
        if (!(isalpha((unsigned char)src[j]) || src[j] == '_')) { j++; continue; }
        name_s = j++;
        while (j < len && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
        name_n = j - name_s;
        after = cc_skip_ws_and_comments(src, len, j);
        if (after >= len || src[after] != '(') continue;
        if (is_decl && is_decl(src, len, name_s, name_n)) continue;
        b = cc__callee_bucket(idx, src + name_s, name_n);
        if (!b || cc__callee_bucket_push(b, name_s) != 0) {
            cc_callee_occ_index_free(idx);
            return NULL;
        }
    }
    return idx;
}

void cc_callee_occ_index_free(CCCalleeOccIndex* idx) {
    int i;
    if (!idx) return;
    for (i = 0; i < idx->n; i++) {
        free(idx->buckets[i].name);
        free(idx->buckets[i].offs);
    }
    free(idx->buckets);
    free(idx);
}

size_t cc_callee_occ_index_nth(const CCCalleeOccIndex* idx,
                               const char* name, int want_occ) {
    const CCCalleeOccBucket* b = cc__callee_bucket_find(idx, name);
    if (!b || want_occ <= 0 || want_occ > b->count) return (size_t)-1;
    return b->offs[want_occ - 1];
}

size_t cc_callee_occ_index_first_in_range(const CCCalleeOccIndex* idx,
                                          const char* name,
                                          size_t start, size_t end) {
    const CCCalleeOccBucket* b = cc__callee_bucket_find(idx, name);
    int i;
    if (!b) return (size_t)-1;
    for (i = 0; i < b->count; i++) {
        if (b->offs[i] >= start && b->offs[i] < end) return b->offs[i];
    }
    return (size_t)-1;
}
