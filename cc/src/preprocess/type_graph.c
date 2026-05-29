/*
 * CCTypeGraph — thin wrapper over CCTypeRegistry (instantiation seam A1).
 */
#include "type_graph.h"

#include <stdlib.h>

struct CCTypeGraph {
    CCTypeRegistry* reg;
};

static _Thread_local CCTypeGraph* g_type_graph = NULL;

/* Registry actually used for reads/writes: honors cc_type_registry_scope_push
 * temp registries during reparse without clobbering the TU graph backing store. */
CCTypeRegistry* cc_type_graph_active_registry(CCTypeGraph* graph) {
    CCTypeRegistry* active = cc_type_registry_get_global();
    if (active) return active;
    return graph ? graph->reg : NULL;
}

CCTypeGraph* cc_type_graph_new(void) {
    CCTypeGraph* graph = (CCTypeGraph*)calloc(1, sizeof(CCTypeGraph));
    if (!graph) return NULL;
    graph->reg = cc_type_registry_new();
    if (!graph->reg) {
        free(graph);
        return NULL;
    }
    return graph;
}

void cc_type_graph_free(CCTypeGraph* graph) {
    if (!graph) return;
    cc_type_registry_free(graph->reg);
    free(graph);
}

void cc_type_graph_clear(CCTypeGraph* graph) {
    if (!graph || !graph->reg) return;
    cc_type_registry_clear(graph->reg);
}

CCTypeRegistry* cc_type_graph_registry(CCTypeGraph* graph) {
    return graph ? graph->reg : NULL;
}

CCTypeGraph* cc_type_graph_get_global(void) {
    return g_type_graph;
}

void cc_type_graph_set_global(CCTypeGraph* graph) {
    g_type_graph = graph;
    cc_type_registry_set_global(graph ? graph->reg : NULL);
}

CCTypeGraph* cc_type_graph_ensure_global_cleared(void) {
    CCTypeGraph* graph = cc_type_graph_get_global();
    if (!graph) {
        graph = cc_type_graph_new();
        if (!graph) return NULL;
        cc_type_graph_set_global(graph);
    }
    /* When visit_codegen reparse pushes a scoped temp registry, only clear that
     * temp view — do not wipe graph->reg (TU Vec/Map instantiations). */
    CCTypeRegistry* active = cc_type_registry_get_global();
    if (active) {
        cc_type_registry_clear(active);
    } else {
        cc_type_graph_clear(graph);
    }
    return graph;
}

int cc_type_graph_scope_push(CCTypeGraphScope* scope) {
    if (!scope) return 0;
    scope->saved_graph = NULL;
    scope->temp_graph = NULL;
    CCTypeGraph* temp = cc_type_graph_new();
    if (!temp) return 0;
    scope->saved_graph = g_type_graph;
    scope->temp_graph = temp;
    cc_type_graph_set_global(temp);
    return 1;
}

void cc_type_graph_scope_pop(CCTypeGraphScope* scope) {
    if (!scope || !scope->temp_graph) return;
    cc_type_graph_set_global(scope->saved_graph);
    cc_type_graph_free(scope->temp_graph);
    scope->saved_graph = NULL;
    scope->temp_graph = NULL;
}

int cc_type_graph_request_vec(CCTypeGraph* graph,
                              const char* elem_type,
                              const char* mangled_name) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    if (!reg) return -1;
    return cc_type_registry_add_vec(reg, elem_type, mangled_name);
}

int cc_type_graph_request_map(CCTypeGraph* graph,
                              const char* key_type,
                              const char* val_type,
                              const char* mangled_name) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    if (!reg) return -1;
    return cc_type_registry_add_map(reg, key_type, val_type, mangled_name);
}

int cc_type_graph_request_channel(CCTypeGraph* graph,
                                  const char* elem_type,
                                  const char* mangled_name) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    if (!reg) return -1;
    return cc_type_registry_add_channel(reg, elem_type, mangled_name);
}

size_t cc_type_graph_vec_count(CCTypeGraph* graph) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    return reg ? cc_type_registry_vec_count(reg) : 0;
}

const CCTypeInstantiation* cc_type_graph_get_vec(CCTypeGraph* graph, size_t idx) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    return reg ? cc_type_registry_get_vec(reg, idx) : NULL;
}

size_t cc_type_graph_map_count(CCTypeGraph* graph) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    return reg ? cc_type_registry_map_count(reg) : 0;
}

const CCTypeInstantiation* cc_type_graph_get_map(CCTypeGraph* graph, size_t idx) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    return reg ? cc_type_registry_get_map(reg, idx) : NULL;
}

size_t cc_type_graph_channel_count(CCTypeGraph* graph) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    return reg ? cc_type_registry_channel_count(reg) : 0;
}

const CCTypeInstantiation* cc_type_graph_get_channel(CCTypeGraph* graph, size_t idx) {
    CCTypeRegistry* reg = cc_type_graph_active_registry(graph);
    return reg ? cc_type_registry_get_channel(reg, idx) : NULL;
}
