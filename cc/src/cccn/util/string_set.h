/*
 * String Set and String Map utilities for cccn passes.
 * Simple, allocation-based collections for tracking names and types.
 */
#ifndef CCCN_STRING_SET_H
#define CCCN_STRING_SET_H

#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* StringSet - Set of unique strings                                          */
/* ========================================================================== */

typedef struct {
    char** items;
    int len;
    int cap;
} StringSet;

static inline void string_set_init(StringSet* set) {
    set->items = NULL;
    set->len = set->cap = 0;
}

static inline void string_set_add(StringSet* set, const char* name) {
    if (!name) return;
    /* Check if already present */
    for (int i = 0; i < set->len; i++) {
        if (strcmp(set->items[i], name) == 0) return;
    }
    /* Add new entry */
    if (set->len >= set->cap) {
        int new_cap = set->cap ? set->cap * 2 : 8;
        set->items = realloc(set->items, new_cap * sizeof(char*));
        set->cap = new_cap;
    }
    set->items[set->len++] = strdup(name);
}

static inline int string_set_contains(const StringSet* set, const char* name) {
    if (!name) return 0;
    for (int i = 0; i < set->len; i++) {
        if (strcmp(set->items[i], name) == 0) return 1;
    }
    return 0;
}

static inline void string_set_free(StringSet* set) {
    for (int i = 0; i < set->len; i++) {
        free(set->items[i]);
    }
    free(set->items);
    set->items = NULL;
    set->len = set->cap = 0;
}

/* ========================================================================== */
/* StringMap - Map from strings to strings                                    */
/* ========================================================================== */

typedef struct {
    char** keys;
    char** values;
    int len;
    int cap;
} StringMap;

static inline void string_map_init(StringMap* map) {
    map->keys = map->values = NULL;
    map->len = map->cap = 0;
}

static inline void string_map_set(StringMap* map, const char* key, const char* value) {
    if (!key) return;
    /* Check if already present */
    for (int i = 0; i < map->len; i++) {
        if (strcmp(map->keys[i], key) == 0) {
            free(map->values[i]);
            map->values[i] = value ? strdup(value) : NULL;
            return;
        }
    }
    /* Add new entry */
    if (map->len >= map->cap) {
        int new_cap = map->cap ? map->cap * 2 : 16;
        map->keys = realloc(map->keys, new_cap * sizeof(char*));
        map->values = realloc(map->values, new_cap * sizeof(char*));
        map->cap = new_cap;
    }
    map->keys[map->len] = strdup(key);
    map->values[map->len] = value ? strdup(value) : NULL;
    map->len++;
}

static inline const char* string_map_get(const StringMap* map, const char* key) {
    if (!key) return NULL;
    for (int i = 0; i < map->len; i++) {
        if (strcmp(map->keys[i], key) == 0) return map->values[i];
    }
    return NULL;
}

static inline void string_map_free(StringMap* map) {
    for (int i = 0; i < map->len; i++) {
        free(map->keys[i]);
        free(map->values[i]);
    }
    free(map->keys);
    free(map->values);
    map->keys = map->values = NULL;
    map->len = map->cap = 0;
}

#endif /* CCCN_STRING_SET_H */
