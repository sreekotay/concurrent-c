#include "diag.h"
#include "source_map.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define CC_DIAG_MAX 256
#define CC_DIAG_MAX_FILES 64

static CCDiag g_diags[CC_DIAG_MAX];
static int g_diag_count;
static char* g_files[CC_DIAG_MAX_FILES];
static int g_file_count;
static int g_error_count;
static char g_show_lowered_phase[128];

static const char* g_construct_names[] = {
    "none", "closure", "async", "ufcs", "autoblock", "defer", "destroy",
    "unwrap_bang", "unwrap_qmark", "channel_handle", "channel_slice",
    "result_type", "comptime", "nursery", "spawn", "errhandler",
    "include", "proto_rewrite", "prep",
};

const char* cc_construct_kind_name(CCConstructKind k) {
    if (k < 0 || k >= CC_CONSTRUCT_COUNT) return "unknown";
    return g_construct_names[k];
}

void cc_diag_init(void) {
    cc_diag_clear();
    g_file_count = 0;
    memset(g_show_lowered_phase, 0, sizeof(g_show_lowered_phase));
}

void cc_diag_shutdown(void) {
    cc_diag_clear();
    for (int i = 0; i < g_file_count; i++) {
        free(g_files[i]);
        g_files[i] = NULL;
    }
    g_file_count = 0;
}

int cc_diag_register_file(const char* path) {
    if (!path) return 0;
    for (int i = 0; i < g_file_count; i++) {
        if (g_files[i] && strcmp(g_files[i], path) == 0) return i + 1;
    }
    if (g_file_count >= CC_DIAG_MAX_FILES) return 0;
    g_files[g_file_count] = strdup(path);
    if (!g_files[g_file_count]) return 0;
    return ++g_file_count;
}

const char* cc_diag_file_path(int file_id) {
    if (file_id <= 0 || file_id > g_file_count) return "<unknown>";
    return g_files[file_id - 1];
}

void cc_diag_clear(void) {
    for (int i = 0; i < g_diag_count; i++) {
        free(g_diags[i].message);
        free(g_diags[i].hint);
        free(g_diags[i].macro_chain);
        g_diags[i].message = NULL;
        g_diags[i].hint = NULL;
        g_diags[i].macro_chain = NULL;
    }
    g_diag_count = 0;
    g_error_count = 0;
}

int cc_diag_error_count(void) { return g_error_count; }

static void cc_diag_push(CCDiagLevel level,
                         const char* phase,
                         CCConstructKind construct_kind,
                         CCSourceSpan span,
                         const char* msg,
                         const char* hint,
                         const char* macro_chain) {
    if (g_diag_count >= CC_DIAG_MAX) return;
    CCDiag* d = &g_diags[g_diag_count++];
    d->level = level;
    d->phase = phase ? phase : "cc";
    d->construct_kind = construct_kind;
    d->span = span;
    d->message = msg ? strdup(msg) : strdup("");
    d->hint = hint ? strdup(hint) : NULL;
    d->macro_chain = macro_chain ? strdup(macro_chain) : NULL;
    if (level == CC_DIAG_ERROR) g_error_count++;

    if (cc_debug_enabled("DIAG")) {
        const char* fp = cc_diag_file_path(span.start.file_id);
        fprintf(stderr, "[cc:diag:%s] %s:%d:%d [%s] %s",
                d->phase, fp ? fp : "?", span.start.line, span.start.col,
                cc_construct_kind_name(construct_kind), d->message ? d->message : "");
        if (d->hint) fprintf(stderr, " (hint: %s)", d->hint);
        if (d->macro_chain) fprintf(stderr, " (%s)", d->macro_chain);
        fputc('\n', stderr);
    }
}

void cc_diag_emit(CCDiagLevel level,
                  const char* phase,
                  CCConstructKind construct_kind,
                  CCSourceSpan span,
                  const char* msg,
                  const char* hint) {
    cc_diag_push(level, phase, construct_kind, span, msg, hint, NULL);
}

void cc_diag_emitf(CCDiagLevel level,
                   const char* phase,
                   CCConstructKind construct_kind,
                   CCSourceSpan span,
                   const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cc_diag_emit(level, phase, construct_kind, span, buf, NULL);
}

void cc_diag_print_all(FILE* out) {
    if (!out) out = stderr;
    for (int i = 0; i < g_diag_count; i++) {
        const CCDiag* d = &g_diags[i];
        const char* fp = cc_diag_file_path(d->span.start.file_id);
        const char* lvl = d->level == CC_DIAG_ERROR ? "error" :
                          d->level == CC_DIAG_WARNING ? "warning" : "note";
        fprintf(out, "cc: %s: [%s] %s:%d:%d: %s",
                lvl, d->phase, fp ? fp : "?", d->span.start.line, d->span.start.col,
                d->message ? d->message : "");
        if (d->hint) fprintf(out, "\ncc: hint: %s", d->hint);
        if (d->macro_chain) fprintf(out, "\ncc: note: %s", d->macro_chain);
        fputc('\n', out);
    }
}

void cc_diag_translate_tcc_error(const char* tcc_msg,
                                 int tcc_line,
                                 int tcc_col,
                                 const char* tcc_file,
                                 CCSourceMap* map,
                                 const char* macro_chain) {
    CCSourceSpan span = CC_SPAN_NONE;
    const char* phase = "tcc";
    CCConstructKind kind = CC_CONSTRUCT_NONE;
    const char* chain = macro_chain;

    if (map) {
        CCSourceSpan origin;
        const char* mphase = NULL;
        const char* mchain = NULL;
        if (cc_source_map_lookup(map, tcc_line, tcc_col, &origin, &mphase, &kind, &mchain) == 0) {
            span = origin;
            if (mphase) phase = mphase;
            if (mchain && !chain) chain = mchain;
        }
    }
    if (cc_span_is_none(span) && tcc_file) {
        span.start.file_id = cc_diag_register_file(tcc_file);
        span.start.line = tcc_line;
        span.start.col = tcc_col;
        span.end = span.start;
    }
    cc_diag_push(CC_DIAG_ERROR, phase, kind, span, tcc_msg, NULL, chain);
}

int cc_debug_enabled(const char* name) {
    if (!name || !name[0]) return 0;
    char var[64];
    snprintf(var, sizeof(var), "CC_DEBUG_%s", name);
    const char* v = getenv(var);
    return v && v[0] && v[0] != '0';
}

void cc_debug_log(const char* phase, const char* fmt, ...) {
    if (!phase) phase = "cc";
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[cc:%s] %s\n", phase, buf);
}

void cc_diag_set_show_lowered_phase(const char* phase) {
    if (!phase) {
        g_show_lowered_phase[0] = '\0';
        return;
    }
    strncpy(g_show_lowered_phase, phase, sizeof(g_show_lowered_phase) - 1);
    g_show_lowered_phase[sizeof(g_show_lowered_phase) - 1] = '\0';
}

const char* cc_diag_get_show_lowered_phase(void) {
    return g_show_lowered_phase[0] ? g_show_lowered_phase : NULL;
}

void cc_diag_maybe_dump_lowered(const char* phase, const char* buf, size_t len) {
    const char* want = cc_diag_get_show_lowered_phase();
    if (!want || !phase || strcmp(want, phase) != 0) return;
    if (!buf) return;
    fprintf(stderr, "=== --show-lowered=%s (%zu bytes) ===\n", phase, len);
    fwrite(buf, 1, len, stderr);
    if (len == 0 || buf[len - 1] != '\n') fputc('\n', stderr);
    fprintf(stderr, "=== end ===\n");
}
