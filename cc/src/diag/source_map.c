#include "source_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CCSourceMap* cc_source_map_new(int primary_file_id) {
    CCSourceMap* map = calloc(1, sizeof(CCSourceMap));
    if (!map) return NULL;
    map->primary_file_id = primary_file_id;
    return map;
}

void cc_source_map_free(CCSourceMap* map) {
    if (!map) return;
    for (size_t i = 0; i < map->count; i++) {
        free(map->entries[i].macro_chain);
    }
    free(map->entries);
    free(map);
}

int cc_source_map_add(CCSourceMap* map,
                      CCSourceSpan generated,
                      CCSourceSpan origin,
                      const char* phase,
                      CCConstructKind kind,
                      const char* macro_chain) {
    if (!map) return -1;
    if (map->count >= map->capacity) {
        size_t nc = map->capacity ? map->capacity * 2 : 64;
        CCSourceMapEntry* ne = realloc(map->entries, nc * sizeof(*ne));
        if (!ne) return -1;
        map->entries = ne;
        map->capacity = nc;
    }
    CCSourceMapEntry* e = &map->entries[map->count++];
    e->generated = generated;
    e->origin = origin;
    e->phase = phase;
    e->construct_kind = kind;
    e->macro_chain = macro_chain ? strdup(macro_chain) : NULL;
    return 0;
}

int cc_source_map_lookup(CCSourceMap* map,
                         int gen_line,
                         int gen_col,
                         CCSourceSpan* out_origin,
                         const char** out_phase,
                         CCConstructKind* out_kind,
                         const char** out_macro_chain) {
    (void)gen_col; /* lookup is line-based; column reserved in the API */
    if (!map || !out_origin) return -1;
    for (size_t i = map->count; i > 0; i--) {
        const CCSourceMapEntry* e = &map->entries[i - 1];
        if (gen_line >= e->generated.start.line &&
            gen_line <= e->generated.end.line) {
            *out_origin = e->origin;
            if (out_phase) *out_phase = e->phase;
            if (out_kind) *out_kind = e->construct_kind;
            if (out_macro_chain) *out_macro_chain = e->macro_chain;
            return 0;
        }
    }
    return -1;
}

int cc_source_map_write_file(const CCSourceMap* map, const char* path) {
    if (!map || !path) return -1;
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# cc source map v1\n");
    fprintf(f, "primary_file_id %d\n", map->primary_file_id);
    fprintf(f, "count %zu\n", map->count);
    for (size_t i = 0; i < map->count; i++) {
        const CCSourceMapEntry* e = &map->entries[i];
        fprintf(f, "entry %zu gen %d:%d:%d origin %d:%d:%d kind %d phase %s\n",
                i,
                e->generated.start.file_id, e->generated.start.line, e->generated.start.col,
                e->origin.start.file_id, e->origin.start.line, e->origin.start.col,
                (int)e->construct_kind, e->phase ? e->phase : "");
        if (e->macro_chain)
            fprintf(f, "macro %s\n", e->macro_chain);
    }
    fclose(f);
    return 0;
}

CCSourceMap* cc_source_map_read_file(const char* path) {
    (void)path;
    /* Minimal stub for R0; full parser can be added when runtime track needs it */
    return cc_source_map_new(0);
}
