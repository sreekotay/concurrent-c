#include "cc_rt_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_map_path[1024];
static CCRuntimeSourceLoc g_last_async = {0};

int cc_rt_diag_load_map(const char* map_path) {
    if (!map_path) return -1;
    strncpy(g_map_path, map_path, sizeof(g_map_path) - 1);
    return 0;
}

int cc_rt_resolve_pc(void* pc, CCRuntimeSourceLoc* out) {
    (void)pc;
    if (!out) return -1;
    *out = g_last_async;
    return g_last_async.file ? 0 : -1;
}

void cc_rt_diag_set_async_name(const char* cc_name, const char* file, int line) {
    g_last_async.user_name = cc_name;
    g_last_async.file = file;
    g_last_async.line = line;
    g_last_async.construct = "async";
}

void cc_rt_diag_set_channel_meta(const char* name, const char* topology,
                                 const char* file, int line) {
    (void)name;
    (void)topology;
    (void)file;
    (void)line;
}
