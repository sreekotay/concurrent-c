#ifndef CC_SOURCE_MAP_H
#define CC_SOURCE_MAP_H

#include "diag.h"
#include <stddef.h>

typedef struct CCSourceMapEntry {
    CCSourceSpan generated;
    CCSourceSpan origin;
    const char* phase;
    CCConstructKind construct_kind;
    char* macro_chain;
} CCSourceMapEntry;

struct CCSourceMap {
    CCSourceMapEntry* entries;
    size_t count;
    size_t capacity;
    int primary_file_id;
};

CCSourceMap* cc_source_map_new(int primary_file_id);
void cc_source_map_free(CCSourceMap* map);

int cc_source_map_add(CCSourceMap* map,
                      CCSourceSpan generated,
                      CCSourceSpan origin,
                      const char* phase,
                      CCConstructKind kind,
                      const char* macro_chain);

int cc_source_map_lookup(CCSourceMap* map,
                         int gen_line,
                         int gen_col,
                         CCSourceSpan* out_origin,
                         const char** out_phase,
                         CCConstructKind* out_kind,
                         const char** out_macro_chain);

/* Serialize for runtime track (R0): simple line-oriented format */
int cc_source_map_write_file(const CCSourceMap* map, const char* path);
CCSourceMap* cc_source_map_read_file(const char* path);

#endif /* CC_SOURCE_MAP_H */
