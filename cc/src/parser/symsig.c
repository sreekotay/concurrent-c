#include "symsig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* name;
    char* ret;
    char* param0;
    int nparams;
    int variadic;
} CCSymSig;

static _Thread_local CCSymSig* g_sigs = NULL;
static _Thread_local size_t g_sig_count = 0;
static _Thread_local size_t g_sig_cap = 0;

void cc_symsig_reset(void) {
    size_t i;
    for (i = 0; i < g_sig_count; i++) {
        free(g_sigs[i].name);
        free(g_sigs[i].ret);
        free(g_sigs[i].param0);
    }
    g_sig_count = 0;
}

/* Drop storage-class words tcc prepends; keep cv and `struct ` tags —
 * consumers strip what they don't want. */
static const char* cc__sig_skip_storage(const char* s) {
    for (;;) {
        if (strncmp(s, "extern ", 7) == 0) { s += 7; continue; }
        if (strncmp(s, "static ", 7) == 0) { s += 7; continue; }
        if (strncmp(s, "inline ", 7) == 0) { s += 7; continue; }
        if (strncmp(s, "typedef ", 8) == 0) { s += 8; continue; }
        return s;
    }
}

void cc_symsig_note(const char* name, const char* ret_type,
                    const char* param0_type, int nparams, int is_variadic) {
    if (!name || !name[0]) return;
    /* Last write wins (a definition after a declaration re-reports);
     * the walk is newest-first, so keep the FIRST occurrence seen. */
    {
        size_t i;
        for (i = 0; i < g_sig_count; i++) {
            if (strcmp(g_sigs[i].name, name) == 0) return;
        }
    }
    if (g_sig_count == g_sig_cap) {
        size_t ncap = g_sig_cap ? g_sig_cap * 2 : 256;
        CCSymSig* ns = (CCSymSig*)realloc(g_sigs, ncap * sizeof(CCSymSig));
        if (!ns) return;
        g_sigs = ns;
        g_sig_cap = ncap;
    }
    g_sigs[g_sig_count].name = strdup(name);
    g_sigs[g_sig_count].ret = strdup(ret_type ? cc__sig_skip_storage(ret_type) : "");
    g_sigs[g_sig_count].param0 =
        strdup(param0_type ? cc__sig_skip_storage(param0_type) : "");
    g_sigs[g_sig_count].nparams = nparams;
    g_sigs[g_sig_count].variadic = is_variadic;
    if (!g_sigs[g_sig_count].name || !g_sigs[g_sig_count].ret ||
        !g_sigs[g_sig_count].param0)
        return;
    g_sig_count++;
}

size_t cc_symsig_count(void) { return g_sig_count; }

static const CCSymSig* cc__sig_find(const char* name) {
    size_t i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < g_sig_count; i++) {
        if (strcmp(g_sigs[i].name, name) == 0) return &g_sigs[i];
    }
    return NULL;
}

int cc_symsig_known(const char* name) { return cc__sig_find(name) != NULL; }

int cc_symsig_fn_return(const char* name, char* out, size_t out_sz) {
    const CCSymSig* s = cc__sig_find(name);
    if (!s || !out || out_sz == 0) return 0;
    snprintf(out, out_sz, "%s", s->ret);
    return out[0] != 0;
}

int cc_symsig_fn_first_param(const char* name, char* out, size_t out_sz,
                             int* out_nparams, int* out_variadic) {
    const CCSymSig* s = cc__sig_find(name);
    if (!s) return 0;
    if (out && out_sz) snprintf(out, out_sz, "%s", s->param0);
    if (out_nparams) *out_nparams = s->nparams;
    if (out_variadic) *out_variadic = s->variadic;
    return 1;
}
