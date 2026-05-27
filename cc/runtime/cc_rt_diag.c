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

/* ----------------------------------------------------------------------
 * R3 — `!>` source-location propagation chain.
 *
 * Two pointer stores per `!>` error path.  Strings are caller-owned C
 * string literals so we just keep the pointers; no allocation, no copy.
 *
 * Storage policy: bounded ring buffer.  When the chain fills, the oldest
 * entries are dropped on push (the most-recent N propagations win).  Most
 * useful CC programs propagate < 16 frames; for the deep-recursion case
 * the user gets a truncation marker via `cc_rt_diag_unwrap_chain_truncated_count`. */

static struct {
    const char* file;
    const char* line_str;
} g_uw_chain[CC_RT_DIAG_UNWRAP_CHAIN_MAX];
static int g_uw_chain_len = 0;
static int g_uw_chain_dropped = 0; /* count of entries dropped to the floor */

void cc_rt_diag_record_unwrap_site(const char* file, const char* line_str) {
    if (!file) file = "<input>";
    if (!line_str) line_str = "0";
    if (g_uw_chain_len < CC_RT_DIAG_UNWRAP_CHAIN_MAX) {
        g_uw_chain[g_uw_chain_len].file = file;
        g_uw_chain[g_uw_chain_len].line_str = line_str;
        g_uw_chain_len++;
        return;
    }
    /* Drop the oldest entry; shift the rest down by one.  N is small
     * (16 by default) so the memmove is fine on the slow path. */
    for (int i = 1; i < CC_RT_DIAG_UNWRAP_CHAIN_MAX; i++) {
        g_uw_chain[i - 1] = g_uw_chain[i];
    }
    g_uw_chain[CC_RT_DIAG_UNWRAP_CHAIN_MAX - 1].file = file;
    g_uw_chain[CC_RT_DIAG_UNWRAP_CHAIN_MAX - 1].line_str = line_str;
    g_uw_chain_dropped++;
}

void cc_rt_diag_clear_unwrap_chain(void) {
    g_uw_chain_len = 0;
    g_uw_chain_dropped = 0;
}

int cc_rt_diag_unwrap_chain_len(void) {
    return g_uw_chain_len;
}

int cc_rt_diag_unwrap_site(int i, const char** out_file, const char** out_line_str) {
    if (i < 0 || i >= g_uw_chain_len) return -1;
    if (out_file)     *out_file     = g_uw_chain[i].file;
    if (out_line_str) *out_line_str = g_uw_chain[i].line_str;
    return 0;
}

void cc_rt_diag_print_unwrap_chain(FILE* fp) {
    if (!fp) fp = stderr;
    if (g_uw_chain_len == 0) return;
    if (g_uw_chain_dropped > 0) {
        fprintf(fp, "  ... %d earlier propagation site(s) dropped\n",
                g_uw_chain_dropped);
    }
    for (int i = 0; i < g_uw_chain_len; i++) {
        fprintf(fp, "  propagated through %s:%s\n",
                g_uw_chain[i].file, g_uw_chain[i].line_str);
    }
}
