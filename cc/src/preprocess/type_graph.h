/*
 * CCTypeGraph — per-TU compile-time type graph (instantiation seam, track A1).
 *
 * Wraps CCTypeRegistry today; later holds instantiation requests, emission
 * plan edges, and protocol slots.  New code should prefer cc_type_graph_*
 * over cc_type_registry_get_global(); the underlying registry remains
 * available via cc_type_graph_registry() during migration.
 *
 * See cc/docs/COMPTIME_INSTANTIATION_SEAM.md.
 */
#ifndef CC_TYPE_GRAPH_H
#define CC_TYPE_GRAPH_H

#include <stddef.h>

#include "type_registry.h"

typedef struct CCTypeGraph CCTypeGraph;

/* Instantiation request kinds (seam vocabulary; A1 records via registry). */
typedef enum CCTypeGraphRequestKind {
    CC_GRAPH_REQUEST_VEC = CC_CONTAINER_VEC,
    CC_GRAPH_REQUEST_MAP = CC_CONTAINER_MAP,
    CC_GRAPH_REQUEST_CHAN = CC_CONTAINER_CHANNEL,
} CCTypeGraphRequestKind;

/* --- lifecycle --- */
CCTypeGraph* cc_type_graph_new(void);
void cc_type_graph_free(CCTypeGraph* graph);
void cc_type_graph_clear(CCTypeGraph* graph);

/* Underlying registry (owned by graph; do not free separately). */
CCTypeRegistry* cc_type_graph_registry(CCTypeGraph* graph);

/* Registry that reads/writes should target *right now*: honors a scoped temp
 * registry pushed via cc_type_graph_scope_push during reparse, else falls back
 * to `graph`'s backing store.  Resolution-path callers (e.g. the UFCS pass)
 * should obtain their registry through this instead of reaching past the seam
 * to cc_type_registry_get_global(), so scope-awareness lives in one place. */
CCTypeRegistry* cc_type_graph_active_registry(CCTypeGraph* graph);

/* Ensure thread-local graph exists and is empty; returns NULL on OOM. */
CCTypeGraph* cc_type_graph_ensure_global_cleared(void);

/* Thread-local global graph (keeps registry global in sync on set). */
CCTypeGraph* cc_type_graph_get_global(void);
void cc_type_graph_set_global(CCTypeGraph* graph);

typedef struct {
    CCTypeGraph* saved_graph;
    CCTypeGraph* temp_graph;
} CCTypeGraphScope;

int  cc_type_graph_scope_push(CCTypeGraphScope* scope);
void cc_type_graph_scope_pop(CCTypeGraphScope* scope);

/* --- instantiation requests (delegate to registry for A1) --- */
int cc_type_graph_request_vec(CCTypeGraph* graph,
                              const char* elem_type,
                              const char* mangled_name);
int cc_type_graph_request_map(CCTypeGraph* graph,
                              const char* key_type,
                              const char* val_type,
                              const char* mangled_name);
int cc_type_graph_request_channel(CCTypeGraph* graph,
                                  const char* elem_type,
                                  const char* mangled_name);

/* Iteration mirrors registry (for emission planning). */
size_t cc_type_graph_vec_count(CCTypeGraph* graph);
const CCTypeInstantiation* cc_type_graph_get_vec(CCTypeGraph* graph, size_t idx);
size_t cc_type_graph_map_count(CCTypeGraph* graph);
const CCTypeInstantiation* cc_type_graph_get_map(CCTypeGraph* graph, size_t idx);
size_t cc_type_graph_channel_count(CCTypeGraph* graph);
const CCTypeInstantiation* cc_type_graph_get_channel(CCTypeGraph* graph, size_t idx);

#endif /* CC_TYPE_GRAPH_H */
