/*
 * CCEmitPlan — unified splice anchors (track A2).
 */
#include "emit_plan.h"

#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"

typedef struct CCEmitComptimeFragment {
    CCEmitAnchor anchor;
    size_t site_pos;
    char* text;
} CCEmitComptimeFragment;

static CCEmitComptimeFragment cc__comptime_frags[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
static size_t cc__comptime_frag_count = 0;

/* Thin aliases onto the shared scanners in util/text.h.  Kept as file-local
 * names so the @comptime intrinsic collectors below read uniformly; the logic
 * has a single source of truth shared with symbols.c. */
#define cc__emit_match_kw(src, len, pos, kw)  cc_match_ident_kw((src), (len), (pos), (kw))
#define cc__emit_parse_c_string(src, len, pos, out, cap) \
    cc_parse_c_string_literal((src), (len), (pos), (out), (cap))

static int cc__emit_parse_anchor(const char* src, size_t len, size_t* pos, CCEmitAnchor* out) {
    size_t p = cc_skip_ws_len(src, len, *pos);
    if (p >= len) return 0;
    if (src[p] >= '0' && src[p] <= '9') {
        int v = 0;
        while (p < len && src[p] >= '0' && src[p] <= '9') {
            v = v * 10 + (src[p] - '0');
            p++;
        }
        if (v < 0 || v > (int)CC_EMIT_AT_COMPTIME_SITE) return 0;
        *out = (CCEmitAnchor)v;
        *pos = p;
        return 1;
    }
    if (cc__emit_match_kw(src, len, p, "CC_EMIT_AFTER_PRELUDE")) {
        *out = CC_EMIT_AFTER_PRELUDE;
        *pos = p + strlen("CC_EMIT_AFTER_PRELUDE");
        return 1;
    }
    if (cc__emit_match_kw(src, len, p, "CC_EMIT_BEFORE_FIRST_USE")) {
        *out = CC_EMIT_BEFORE_FIRST_USE;
        *pos = p + strlen("CC_EMIT_BEFORE_FIRST_USE");
        return 1;
    }
    if (cc__emit_match_kw(src, len, p, "CC_EMIT_AT_COMPTIME_SITE")) {
        *out = CC_EMIT_AT_COMPTIME_SITE;
        *pos = p + strlen("CC_EMIT_AT_COMPTIME_SITE");
        return 1;
    }
    return 0;
}

static int cc__emit_try_collect_cc_emit_cstr(const char* src, size_t len, size_t call_pos,
                                             size_t site_pos) {
    size_t p = call_pos + strlen("cc_emit_cstr");
    CCEmitAnchor anchor;
    char frag[4096];
    if (cc__comptime_frag_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p++;
    if (!cc__emit_parse_anchor(src, len, &p, &anchor)) return 0;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != ',') return 0;
    p = cc_skip_ws_len(src, len, p + 1);
    if (!cc__emit_parse_c_string(src, len, &p, frag, sizeof(frag))) return 0;
    {
        CCEmitComptimeFragment* f = &cc__comptime_frags[cc__comptime_frag_count++];
        f->anchor = anchor;
        f->site_pos = site_pos;
        f->text = strdup(frag);
        if (!f->text) {
            cc__comptime_frag_count--;
            return 0;
        }
    }
    return 1;
}

/* --- comptime explicit instantiation requests (track C1) --- */

typedef struct CCEmitComptimeInst {
    CCTypeGraphRequestKind kind;
    char a[128];   /* vec elem / map key / chan elem */
    char b[128];   /* map val (unused otherwise) */
} CCEmitComptimeInst;

static CCEmitComptimeInst cc__comptime_insts[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
static size_t cc__comptime_inst_count = 0;

void cc_emit_plan_clear_comptime_instantiations(void) {
    cc__comptime_inst_count = 0;
}

size_t cc_emit_plan_comptime_instantiation_count(void) {
    return cc__comptime_inst_count;
}

/* ---- Comptime intrinsic registry ----
 *
 * The compiler interprets a fixed set of builtin calls that appear inside
 * `@comptime { … }`.  Rather than open-coding an `if (match_kw …)` chain per
 * intrinsic, they are listed once in `cc__comptime_intrinsics`; the dispatcher
 * (`cc__emit_visit_dispatch`) walks each block body and routes a matched call
 * to its handler.  Each public collector simply selects the group(s) it cares
 * about via a mask.  This is the last text-matching layer before a real
 * `@comptime` evaluator — adding/replacing an intrinsic is a table edit.
 *
 * Note: `cc_type_register` / `cc_type_define` are *also* comptime intrinsics
 * but live in comptime/symbols.c, a different evaluation stage that drives
 * dylib compilation and propagates parse errors.  They share this module's
 * block recognizer and lexers (util/text.h) but not its static-buffer
 * collectors. */
enum { CC_CI_INSTANTIATE = 1u << 0, CC_CI_EMIT = 1u << 1 };

typedef struct CCComptimeIntrinsicDesc CCComptimeIntrinsicDesc;
typedef int (*CCComptimeIntrinsicFn)(const CCComptimeIntrinsicDesc* d,
                                     const char* src, size_t len,
                                     size_t pos, size_t splice_end);
struct CCComptimeIntrinsicDesc {
    const char*            name;    /* exact call keyword to match            */
    unsigned               group;   /* CC_CI_* bit (which collector wants it) */
    CCComptimeIntrinsicFn  collect; /* parse the matched call                 */
    CCTypeGraphRequestKind kind;    /* instantiate handler: request kind      */
    int                    n_args;  /* instantiate handler: 1 or 2 string args*/
};

static int cc__ci_collect_instantiate(const CCComptimeIntrinsicDesc* d,
                                      const char* src, size_t len,
                                      size_t pos, size_t splice_end) {
    size_t p = pos + strlen(d->name);
    CCEmitComptimeInst inst;
    (void)splice_end;
    if (cc__comptime_inst_count >= CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS) return 0;
    memset(&inst, 0, sizeof(inst));
    inst.kind = d->kind;
    p = cc_skip_ws_len(src, len, p);
    if (p >= len || src[p] != '(') return 0;
    p = cc_skip_ws_len(src, len, p + 1);
    if (!cc__emit_parse_c_string(src, len, &p, inst.a, sizeof(inst.a))) return 0;
    if (d->n_args == 2) {
        p = cc_skip_ws_len(src, len, p);
        if (p >= len || src[p] != ',') return 0;
        p = cc_skip_ws_len(src, len, p + 1);
        if (!cc__emit_parse_c_string(src, len, &p, inst.b, sizeof(inst.b))) return 0;
    }
    cc__comptime_insts[cc__comptime_inst_count++] = inst;
    return 1;
}

static int cc__ci_collect_emit(const CCComptimeIntrinsicDesc* d,
                               const char* src, size_t len,
                               size_t pos, size_t splice_end) {
    (void)d;
    return cc__emit_try_collect_cc_emit_cstr(src, len, pos, splice_end);
}

static const CCComptimeIntrinsicDesc cc__comptime_intrinsics[] = {
    { "cc_instantiate_vec",  CC_CI_INSTANTIATE, cc__ci_collect_instantiate, CC_GRAPH_REQUEST_VEC,  1 },
    { "cc_instantiate_map",  CC_CI_INSTANTIATE, cc__ci_collect_instantiate, CC_GRAPH_REQUEST_MAP,  2 },
    { "cc_instantiate_chan", CC_CI_INSTANTIATE, cc__ci_collect_instantiate, CC_GRAPH_REQUEST_CHAN, 1 },
    { "cc_emit_cstr",        CC_CI_EMIT,        cc__ci_collect_emit,        (CCTypeGraphRequestKind)0, 0 },
};

static const unsigned cc__ci_mask_instantiate = CC_CI_INSTANTIATE;
static const unsigned cc__ci_mask_emit        = CC_CI_EMIT;

/* Single enumerator over top-level `@comptime { ... }` blocks.  The body of
 * each block is handed to a visitor; the table-driven `cc__emit_visit_dispatch`
 * is the only visitor needed — it routes recognized intrinsics from
 * `cc__comptime_intrinsics`.  Block bounds come from the shared
 * `cc_match_comptime_block` recognizer (util/text.h), the same one symbols.c
 * uses.  See COMPTIME_INSTANTIATION_SEAM.md §1b. */
typedef void (*CCEmitComptimeBlockVisitor)(const char* src, size_t len,
                                           size_t body_l, size_t body_r, void* ctx);

static void cc__emit_for_each_comptime_block(const char* src, size_t len,
                                             CCEmitComptimeBlockVisitor visit, void* ctx) {
    size_t i = 0;
    if (!src || len == 0 || !visit) return;
    while (i < len) {
        size_t body_l = 0, body_r = 0;
        if (!cc_match_comptime_block(src, len, i, &body_l, &body_r)) {
            i++;
            continue;
        }
        visit(src, len, body_l, body_r, ctx);
        i = body_r + 1;
    }
}

/* Per-block dispatcher: scan the body once and route each recognized intrinsic
 * call (whose group is enabled in *ctx mask) to its registry handler. */
static void cc__emit_visit_dispatch(const char* src, size_t len,
                                    size_t body_l, size_t body_r, void* ctx) {
    unsigned mask = ctx ? *(const unsigned*)ctx : 0u;
    const size_t n_intr = sizeof(cc__comptime_intrinsics) / sizeof(cc__comptime_intrinsics[0]);
    for (size_t j = body_l + 1; j < body_r; j++) {
        for (size_t k = 0; k < n_intr; k++) {
            const CCComptimeIntrinsicDesc* d = &cc__comptime_intrinsics[k];
            if (!(d->group & mask)) continue;
            if (!cc__emit_match_kw(src, len, j, d->name)) continue;
            d->collect(d, src, len, j, body_r + 1);
            j += strlen(d->name) - 1; /* loop's ++ steps past the last char */
            break;
        }
    }
}

void cc_emit_plan_collect_comptime_instantiations(const char* src, size_t len) {
    cc__emit_for_each_comptime_block(src, len, cc__emit_visit_dispatch,
                                     (void*)&cc__ci_mask_instantiate);
}

void cc_emit_plan_apply_comptime_instantiations(CCTypeGraph* graph) {
    /* graph may be NULL: cc_type_graph_request_* falls back to the global
     * registry (see cc_type_graph_active_registry), which is what the
     * final-compile path in visit_codegen.c reads. */
    for (size_t i = 0; i < cc__comptime_inst_count; i++) {
        const CCEmitComptimeInst* inst = &cc__comptime_insts[i];
        char mangled[256];
        switch (inst->kind) {
        case CC_GRAPH_REQUEST_VEC:
            if (!inst->a[0]) break;
            snprintf(mangled, sizeof(mangled), "CCVec_%s", inst->a);
            cc_type_graph_request_vec(graph, inst->a, mangled);
            break;
        case CC_GRAPH_REQUEST_MAP:
            if (!inst->a[0] || !inst->b[0]) break;
            snprintf(mangled, sizeof(mangled), "Map_%s_%s", inst->a, inst->b);
            cc_type_graph_request_map(graph, inst->a, inst->b, mangled);
            break;
        case CC_GRAPH_REQUEST_CHAN:
            if (!inst->a[0]) break;
            snprintf(mangled, sizeof(mangled), "CCChan_%s", inst->a);
            cc_type_graph_request_channel(graph, inst->a, mangled);
            break;
        default:
            break;
        }
    }
}

void cc_emit_plan_clear_comptime_fragments(void) {
    for (size_t i = 0; i < cc__comptime_frag_count; i++) {
        free(cc__comptime_frags[i].text);
        cc__comptime_frags[i].text = NULL;
    }
    cc__comptime_frag_count = 0;
}

size_t cc_emit_plan_comptime_fragment_count(void) {
    return cc__comptime_frag_count;
}

void cc_emit_plan_collect_comptime_emits(const char* src, size_t len) {
    cc__emit_for_each_comptime_block(src, len, cc__emit_visit_dispatch,
                                     (void*)&cc__ci_mask_emit);
}

static size_t cc__emit_resolve_anchor_pos(CCEmitAnchor anchor, size_t site_pos,
                                          size_t insert_pos, size_t container_pos) {
    switch (anchor) {
    case CC_EMIT_AT_COMPTIME_SITE:
        return site_pos;
    case CC_EMIT_BEFORE_FIRST_USE:
        return insert_pos;
    case CC_EMIT_AFTER_PRELUDE:
    default:
        return container_pos > 0 && container_pos < insert_pos ? container_pos : insert_pos;
    }
}

void cc_emit_plan_build_comptime_schedule(const char* src, size_t len,
                                          size_t insert_pos, size_t container_pos,
                                          CCEmitPlanComptimeSchedule* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    (void)src;
    (void)len;
    for (size_t i = 0; i < cc__comptime_frag_count &&
                       out->n < CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS; i++) {
        out->pos[out->n] = cc__emit_resolve_anchor_pos(cc__comptime_frags[i].anchor,
                                                         cc__comptime_frags[i].site_pos,
                                                         insert_pos, container_pos);
        out->frag_index[out->n] = i;
        out->n++;
    }
}

void cc_emit_plan_fprint_comptime_fragment(FILE* out, size_t frag_index) {
    if (!out || frag_index >= cc__comptime_frag_count) return;
    const CCEmitComptimeFragment* f = &cc__comptime_frags[frag_index];
    if (!f->text || !f->text[0]) return;
    fprintf(out, "/* --- comptime cc_emit_cstr --- */\n%s", f->text);
    if (f->text[strlen(f->text) - 1] != '\n') fputc('\n', out);
}

static int cc__emit_splice_at(char** src, size_t* len, size_t pos, const char* insert,
                              const char* input_path) {
    size_t ins_len;
    char* nb;
    size_t old_len;
    if (!src || !len || !insert) return -1;
    old_len = *len;
    if (pos > old_len) pos = old_len;
    ins_len = strlen(insert);
    nb = (char*)malloc(old_len + ins_len + 256);
    if (!nb) return -1;
    memcpy(nb, *src, pos);
    memcpy(nb + pos, insert, ins_len);
    size_t tail = old_len - pos;
    memcpy(nb + pos + ins_len, *src + pos, tail);
    nb[pos + ins_len + tail] = '\0';
    free(*src);
    *src = nb;
    *len = pos + ins_len + tail;
    (void)input_path;
    return 0;
}

int cc_emit_plan_splice_comptime_fragments(char** src, size_t* len, const char* input_path) {
    size_t insert_pos;
    size_t container_pos;
    CCEmitPlanComptimeSchedule sched;
    size_t order[CC_EMIT_PLAN_MAX_COMPTIME_FRAGMENTS];
    if (!src || !*src || !len) return 0;
    if (cc__comptime_frag_count == 0) return 0;
    insert_pos = cc_emit_plan_compute_prelude_insert_pos(*src, *len);
    container_pos = cc_emit_plan_compute_container_anchor(*src, *len);
    cc_emit_plan_build_comptime_schedule(*src, *len, insert_pos, container_pos, &sched);
    for (size_t i = 0; i < sched.n; i++) order[i] = i;
    for (size_t i = 0; i + 1 < sched.n; i++) {
        for (size_t j = i + 1; j < sched.n; j++) {
            if (sched.pos[order[j]] > sched.pos[order[i]]) {
                size_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    for (size_t oi = 0; oi < sched.n; oi++) {
        size_t si = order[oi];
        size_t frag_i = sched.frag_index[si];
        const CCEmitComptimeFragment* f = &cc__comptime_frags[frag_i];
        char block[8192];
        int n = snprintf(block, sizeof(block),
                         "\n/* --- comptime cc_emit_cstr --- */\n%s%s",
                         f->text ? f->text : "",
                         (f->text && f->text[0] && f->text[strlen(f->text) - 1] == '\n') ? "" : "\n");
        if (n <= 0 || (size_t)n >= sizeof(block)) return -1;
        if (cc__emit_splice_at(src, len, sched.pos[si], block, input_path) != 0) return -1;
    }
    return 0;
}

size_t cc_emit_plan_line_start_before(const char* src, size_t pos) {
    if (!src) return 0;
    while (pos > 0 && src[pos - 1] != '\n') pos--;
    return pos;
}

size_t cc_emit_plan_find_ident_top_level(const char* src, size_t start, size_t len,
                                         const char* ident) {
    size_t ident_len = ident ? strlen(ident) : 0;
    size_t pos = start;
    if (!src || !ident || ident_len == 0 || start >= len) return len;
    while (pos + ident_len <= len) {
        pos = cc_find_substr_top_level(src, pos, len, ident, ident_len);
        if (pos >= len) return len;
        int left_ok = (pos == 0) || !cc_is_ident_char(src[pos - 1]);
        int right_ok = (pos + ident_len >= len) || !cc_is_ident_char(src[pos + ident_len]);
        if (left_ok && right_ok) return pos;
        pos++;
    }
    return len;
}

size_t cc_emit_plan_type_decl_end_top_level(const char* src, size_t len,
                                            const char* type_name) {
    size_t p = 0;
    if (!src || !type_name || !type_name[0]) return 0;
    if (strcmp(type_name, "void") == 0 ||
        strcmp(type_name, "bool") == 0 ||
        strcmp(type_name, "char") == 0 ||
        strcmp(type_name, "short") == 0 ||
        strcmp(type_name, "int") == 0 ||
        strcmp(type_name, "long") == 0 ||
        strcmp(type_name, "float") == 0 ||
        strcmp(type_name, "double") == 0 ||
        strcmp(type_name, "size_t") == 0 ||
        strcmp(type_name, "ssize_t") == 0 ||
        strcmp(type_name, "CCError") == 0) {
        return 0;
    }
    while (p < len) {
        size_t line_start = p;
        size_t line_end = line_start;
        while (line_end < len && src[line_end] != '\n') line_end++;
        size_t s = line_start;
        while (s < line_end && (src[s] == ' ' || src[s] == '\t' || src[s] == '\r')) s++;
        int is_type_decl =
            (s + 7 <= line_end && memcmp(src + s, "typedef", 7) == 0 && !cc_is_ident_char(src[s + 7])) ||
            (s + 6 <= line_end && memcmp(src + s, "struct", 6) == 0 && !cc_is_ident_char(src[s + 6])) ||
            (s + 5 <= line_end && memcmp(src + s, "union", 5) == 0 && !cc_is_ident_char(src[s + 5])) ||
            (s + 4 <= line_end && memcmp(src + s, "enum", 4) == 0 && !cc_is_ident_char(src[s + 4]));
        if (!is_type_decl) {
            p = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }

        size_t q = s;
        int brace_depth = 0;
        int in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
        for (; q < len; q++) {
            char c = src[q];
            char c2 = (q + 1 < len) ? src[q + 1] : 0;
            if (in_lc) { if (c == '\n') in_lc = 0; continue; }
            if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; q++; } continue; }
            if (in_str) { if (c == '\\' && c2) { q++; continue; } if (c == '"') in_str = 0; continue; }
            if (in_chr) { if (c == '\\' && c2) { q++; continue; } if (c == '\'') in_chr = 0; continue; }
            if (c == '/' && c2 == '/') { in_lc = 1; q++; continue; }
            if (c == '/' && c2 == '*') { in_bc = 1; q++; continue; }
            if (c == '"') { in_str = 1; continue; }
            if (c == '\'') { in_chr = 1; continue; }
            if (c == '{') { brace_depth++; continue; }
            if (c == '}') { if (brace_depth > 0) brace_depth--; continue; }
            if (c == ';' && brace_depth == 0) {
                size_t end = q + 1;
                int declares_type = 0;
                if (s + 7 <= line_end && memcmp(src + s, "typedef", 7) == 0 && !cc_is_ident_char(src[s + 7])) {
                    size_t e = q;
                    while (e > s && (src[e - 1] == ' ' || src[e - 1] == '\t' || src[e - 1] == '\r' || src[e - 1] == '\n')) e--;
                    size_t b = e;
                    while (b > s && cc_is_ident_char(src[b - 1])) b--;
                    size_t type_len = strlen(type_name);
                    declares_type = (e > b && e - b == type_len && memcmp(src + b, type_name, type_len) == 0);
                } else {
                    size_t kw_len =
                        (s + 6 <= line_end && memcmp(src + s, "struct", 6) == 0 && !cc_is_ident_char(src[s + 6])) ? 6 :
                        (s + 5 <= line_end && memcmp(src + s, "union", 5) == 0 && !cc_is_ident_char(src[s + 5])) ? 5 :
                        (s + 4 <= line_end && memcmp(src + s, "enum", 4) == 0 && !cc_is_ident_char(src[s + 4])) ? 4 : 0;
                    size_t b = s + kw_len;
                    while (b < end && (src[b] == ' ' || src[b] == '\t' || src[b] == '\r' || src[b] == '\n')) b++;
                    size_t e = b;
                    while (e < end && cc_is_ident_char(src[e])) e++;
                    size_t type_len = strlen(type_name);
                    declares_type = (e > b && e - b == type_len && memcmp(src + b, type_name, type_len) == 0);
                }
                if (declares_type) {
                    if (end < len && src[end] == '\n') end++;
                    return end;
                }
                p = (end < len && src[end] == '\n') ? end + 1 : end;
                break;
            }
        }
        if (q >= len) return 0;
    }
    return 0;
}

static int cc__emit_plan_block_references_container(const char* src, size_t block_start,
                                                    size_t block_end, int is_typedef_block) {
    int refs_container = 0;
    int typedef_uses_only_predeclared_vec_char = 0;
    if (!src || block_end <= block_start) return 0;
    for (size_t si = block_start; si + 7 < block_end && !refs_container; si++) {
        if (memcmp(src + si, "__CC_MAP", 8) == 0 ||
            memcmp(src + si, "__CC_VEC", 8) == 0) {
            refs_container = 1;
        } else if ((si + 4 < block_end && memcmp(src + si, "Map_", 4) == 0) ||
                   (si + 6 < block_end && memcmp(src + si, "CCVec_", 6) == 0)) {
            refs_container = 1;
        }
    }
    if (is_typedef_block && refs_container) {
        for (size_t si = block_start; si + 14 <= block_end; si++) {
            if (memcmp(src + si, "__CC_VEC(char)", 14) == 0) {
                typedef_uses_only_predeclared_vec_char = 1;
                break;
            }
        }
    }
    if (!refs_container) return 0;
    if (!is_typedef_block) return 1;
    return !typedef_uses_only_predeclared_vec_char;
}

size_t cc_emit_plan_compute_prelude_insert_pos(const char* src, size_t len) {
    size_t insert_pos = 0;
    if (!src || len == 0) return 0;
    while (insert_pos < len) {
        size_t line_start = insert_pos;
        size_t line_end = line_start;
        while (line_end < len && src[line_end] != '\n') line_end++;
        size_t p = line_start;
        while (p < line_end && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r')) p++;
        if (p + 1 < len && p + 1 < line_end && src[p] == '/' && src[p + 1] == '*') {
            size_t end = p + 2;
            while (end + 1 < len && !(src[end] == '*' && src[end + 1] == '/')) end++;
            insert_pos = (end + 1 < len) ? end + 2 : len;
            if (insert_pos < len && src[insert_pos] == '\n') insert_pos++;
            continue;
        }
        if (p + 9 <= line_end && memcmp(src + p, "#include ", 9) == 0) {
            insert_pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (p < line_end && src[p] == '#') {
            insert_pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (p == line_end || (p + 1 < line_end && src[p] == '/' && src[p + 1] == '/')) {
            insert_pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if ((p + 7 <= line_end && memcmp(src + p, "typedef", 7) == 0 && !cc_is_ident_char(src[p + 7])) ||
            (p + 6 <= line_end && memcmp(src + p, "struct", 6) == 0 && !cc_is_ident_char(src[p + 6])) ||
            (p + 5 <= line_end && memcmp(src + p, "union", 5) == 0 && !cc_is_ident_char(src[p + 5])) ||
            (p + 4 <= line_end && memcmp(src + p, "enum", 4) == 0 && !cc_is_ident_char(src[p + 4]))) {
            size_t q = p;
            int brace_depth = 0;
            int in_str = 0, in_chr = 0, in_lc = 0, in_bc = 0;
            for (; q < len; q++) {
                char c = src[q];
                char c2 = (q + 1 < len) ? src[q + 1] : 0;
                if (in_lc) { if (c == '\n') in_lc = 0; continue; }
                if (in_bc) { if (c == '*' && c2 == '/') { in_bc = 0; q++; } continue; }
                if (in_str) { if (c == '\\' && c2) { q++; continue; } if (c == '"') in_str = 0; continue; }
                if (in_chr) { if (c == '\\' && c2) { q++; continue; } if (c == '\'') in_chr = 0; continue; }
                if (c == '/' && c2 == '/') { in_lc = 1; q++; continue; }
                if (c == '/' && c2 == '*') { in_bc = 1; q++; continue; }
                if (c == '"') { in_str = 1; continue; }
                if (c == '\'') { in_chr = 1; continue; }
                if (c == '{') { brace_depth++; continue; }
                if (c == '}') { if (brace_depth > 0) brace_depth--; continue; }
                if (c == ';' && brace_depth == 0) {
                    q++;
                    if (q < len && src[q] == '\n') q++;
                    insert_pos = q;
                    break;
                }
            }
            if (q >= len) insert_pos = len;
            continue;
        }
        break;
    }
    return insert_pos;
}

size_t cc_emit_plan_compute_container_anchor(const char* src, size_t len) {
    size_t pos = 0;
    if (!src || len == 0) return 0;
    while (pos < len) {
        size_t line_start = pos;
        size_t line_end = line_start;
        while (line_end < len && src[line_end] != '\n') line_end++;
        size_t p = line_start;
        while (p < line_end && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r')) p++;
        if (p == line_end) {
            pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (p + 1 < len && src[p] == '/' && src[p + 1] == '*') {
            size_t end = p + 2;
            while (end + 1 < len && !(src[end] == '*' && src[end + 1] == '/')) end++;
            pos = (end + 1 < len) ? end + 2 : len;
            if (pos < len && src[pos] == '\n') pos++;
            continue;
        }
        if (p + 1 < line_end && src[p] == '/' && src[p + 1] == '/') {
            pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if (src[p] == '#') {
            pos = (line_end < len) ? line_end + 1 : line_end;
            continue;
        }
        if ((p + 7 <= line_end && memcmp(src + p, "typedef", 7) == 0 && !cc_is_ident_char(src[p + 7])) ||
            (p + 6 <= line_end && memcmp(src + p, "struct", 6) == 0 && !cc_is_ident_char(src[p + 6])) ||
            (p + 5 <= line_end && memcmp(src + p, "union", 5) == 0 && !cc_is_ident_char(src[p + 5])) ||
            (p + 4 <= line_end && memcmp(src + p, "enum", 4) == 0 && !cc_is_ident_char(src[p + 4]))) {
            int is_typedef_block =
                (p + 7 <= line_end && memcmp(src + p, "typedef", 7) == 0 && !cc_is_ident_char(src[p + 7]));
            size_t block_start = p;
            size_t q = p;
            int brace_depth = 0;
            size_t block_end = len;
            while (q < len) {
                char c = src[q];
                if (c == '{') brace_depth++;
                else if (c == '}') { if (brace_depth > 0) brace_depth--; }
                else if (c == ';' && brace_depth == 0) {
                    q++;
                    if (q < len && src[q] == '\n') q++;
                    block_end = q;
                    break;
                }
                q++;
            }
            if (q >= len) block_end = len;
            if (cc__emit_plan_block_references_container(src, block_start, block_end, is_typedef_block)) {
                return line_start;
            }
            pos = block_end;
            continue;
        }
        break;
    }
    return pos;
}

size_t cc_emit_plan_compute_before_first_use(const char* src, size_t len, size_t anchor_pos,
                                             const char* payload_type, const char* mangled_name) {
    size_t decl_end = cc_emit_plan_type_decl_end_top_level(src, len, payload_type);
    if (decl_end <= anchor_pos) return anchor_pos;
    if (mangled_name && mangled_name[0]) {
        size_t first_use = cc_emit_plan_find_ident_top_level(src, decl_end, len, mangled_name);
        if (first_use < len) {
            return cc_emit_plan_line_start_before(src, first_use);
        }
    }
    return decl_end;
}

void cc_emit_plan_build_container_schedule(const char* src, size_t len, CCTypeGraph* graph,
                                           CCEmitPlanContainerSchedule* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        out->vec_pos[i] = len + 1;
        out->map_pos[i] = len + 1;
    }
    if (!graph) return;
    out->anchor_pos = cc_emit_plan_compute_container_anchor(src, len);
    out->n_vec = cc_type_graph_vec_count(graph);
    out->n_map = cc_type_graph_map_count(graph);
    for (size_t i = 0; i < out->n_vec && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        const CCTypeInstantiation* inst = cc_type_graph_get_vec(graph, i);
        if (!inst || !inst->type1 || !inst->mangled_name) continue;
        const char* mangled_elem = inst->mangled_name + 6;
        if (strcmp(mangled_elem, "char") == 0) continue;
        size_t pos = cc_emit_plan_compute_before_first_use(src, len, out->anchor_pos,
                                                           inst->type1, inst->mangled_name);
        if (pos > out->anchor_pos) {
            out->vec_delayed[i] = 1;
            out->vec_pos[i] = pos;
        }
    }
    for (size_t i = 0; i < out->n_map && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        const CCTypeInstantiation* inst = cc_type_graph_get_map(graph, i);
        if (!inst || !inst->type1 || !inst->type2 || !inst->mangled_name) continue;
        size_t k_end = cc_emit_plan_type_decl_end_top_level(src, len, inst->type1);
        size_t v_end = cc_emit_plan_type_decl_end_top_level(src, len, inst->type2);
        size_t decl_end = k_end > v_end ? k_end : v_end;
        if (decl_end <= out->anchor_pos) continue;
        size_t pos = cc_emit_plan_compute_before_first_use(src, len, out->anchor_pos,
                                                           inst->type1, inst->mangled_name);
        size_t pos2 = cc_emit_plan_compute_before_first_use(src, len, out->anchor_pos,
                                                            inst->type2, inst->mangled_name);
        if (pos2 > pos) pos = pos2;
        out->map_delayed[i] = 1;
        out->map_pos[i] = pos;
    }
}

void cc_emit_plan_build_result_delays(const char* src, size_t len,
                                      const CCResultSpecTable* specs,
                                      size_t prelude_insert_pos,
                                      CCEmitPlanResultDelay* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        out->pos[i] = len + 1;
    }
    if (!specs) return;
    for (size_t i = 0; i < specs->count && i < CC_EMIT_PLAN_MAX_DELAYED; i++) {
        const CCResultSpec* spec = cc_result_spec_table_get(specs, i);
        size_t ok_decl_end = spec ? cc_emit_plan_type_decl_end_top_level(src, len, spec->ok_type) : 0;
        size_t err_decl_end = spec ? cc_emit_plan_type_decl_end_top_level(src, len, spec->err_type) : 0;
        size_t decl_end = ok_decl_end > err_decl_end ? ok_decl_end : err_decl_end;
        if (!spec || decl_end <= prelude_insert_pos) continue;

        char concrete[256];
        cc_result_spec_format_name(spec->mangled_ok, spec->mangled_err,
                                   concrete, sizeof(concrete));
        size_t first_use = cc_emit_plan_find_ident_top_level(src, decl_end, len, concrete);
        out->delayed[i] = 1;
        if (first_use < len) {
            out->pos[i] = cc_emit_plan_line_start_before(src, first_use);
        } else {
            out->pos[i] = decl_end;
        }
    }
}

void cc_emit_plan_fprint_container_prelude(FILE* out, int use_cch,
                                           int need_vec, int need_map, int need_chan) {
    if (!out) return;
    fprintf(out, "/* --- CC generic container declarations --- */\n");
    fprintf(out, "#ifdef CC_PARSER_MODE\n");
    fprintf(out, "#undef CC_PARSER_MODE\n");
    fprintf(out, "#define __CC_RESTORE_PARSER_MODE_AFTER_CONTAINERS 1\n");
    fprintf(out, "#endif\n");
    if (use_cch) {
        /* TCC parse path: map_forward (from prelude) already froze the parser
         * stub CC_MAP_DECL_ARENA — do not include map_impl (cc_containers). */
        if (need_vec) fprintf(out, "#include <ccc/std/vec.cch>\n");
        if (need_chan) fprintf(out, "#include <ccc/cc_channel.cch>\n");
    } else {
        if (need_vec) fprintf(out, "#include <ccc/std/vec.h>\n");
        if (need_map) fprintf(out, "#include <ccc/std/map.h>\n");
        if (need_chan) fprintf(out, "#include <ccc/cc_channel.h>\n");
    }
}

void cc_emit_plan_fprint_container_epilogue(FILE* out) {
    if (!out) return;
    fprintf(out, "/* --- end container declarations (post-prelude) --- */\n");
    fprintf(out, "#ifdef __CC_RESTORE_PARSER_MODE_AFTER_CONTAINERS\n");
    fprintf(out, "#undef __CC_RESTORE_PARSER_MODE_AFTER_CONTAINERS\n");
    fprintf(out, "#ifndef CC_PARSER_MODE\n");
    fprintf(out, "#define CC_PARSER_MODE 1\n");
    fprintf(out, "#endif\n");
    fprintf(out, "#endif\n\n");
}

void cc_emit_plan_fprint_vec_decl(FILE* out, const CCTypeInstantiation* inst) {
    if (!out || !inst || !inst->type1 || !inst->mangled_name) return;
    const char* mangled_elem = inst->mangled_name + 6;
    if (strcmp(mangled_elem, "char") == 0) return;
    fprintf(out, "CC_VEC_DECL_ARENA(%s, %s)\n", inst->type1, inst->mangled_name);
}

void cc_emit_plan_fprint_map_decl(FILE* out, const CCTypeInstantiation* inst) {
    if (!out || !inst || !inst->type1 || !inst->type2 || !inst->mangled_name) return;
    const char* hash_fn = "cc_map_hash_i32";
    const char* eq_fn = "cc_map_eq_i32";
    if (strcmp(inst->type1, "int") == 0) {
        hash_fn = "cc_map_hash_i32"; eq_fn = "cc_map_eq_i32";
    } else if (strcmp(inst->type1, "CCSliceHdr") == 0) {
        hash_fn = "cc_map_hash_slice_hdr"; eq_fn = "cc_map_eq_slice_hdr";
    } else if (strstr(inst->type1, "64") != NULL) {
        hash_fn = "cc_map_hash_u64"; eq_fn = "cc_map_eq_u64";
    } else if (strstr(inst->type1, "slice") != NULL || strstr(inst->type1, "Slice") != NULL ||
               strcmp(inst->type1, "charslice") == 0) {
        hash_fn = "cc_map_hash_slice"; eq_fn = "cc_map_eq_slice";
    }
    fprintf(out, "CC_MAP_DECL_ARENA(%s, %s, %s, %s, %s)\n",
            inst->type1, inst->type2, inst->mangled_name, hash_fn, eq_fn);
}

int cc_emit_plan_format_result_arm(char* out, size_t out_sz,
                                   const char* concrete,
                                   CCResultArmKind kind,
                                   int ok_is_void, int with_diag) {
    if (!out || out_sz == 0 || !concrete) return -1;
    switch (kind) {
    case CC_RESULT_ARM_IS_ERR:
        return snprintf(out, out_sz,
            "    %s: (!((%s*)(void*)&(__x__))->ok), \\\n",
            concrete, concrete);
    case CC_RESULT_ARM_VALUE:
        if (ok_is_void) {
            return snprintf(out, out_sz, "    %s: ((void)0), \\\n", concrete);
        }
        return snprintf(out, out_sz,
            "    %s: ((%s*)(void*)&(__x__))->u.value, \\\n",
            concrete, concrete);
    case CC_RESULT_ARM_ERR:
        if (with_diag) {
            return snprintf(out, out_sz,
                "    %s: (cc_rt_diag_record_unwrap_site(__f__, __l__), "
                "((%s*)(void*)&(__x__))->u.error), \\\n",
                concrete, concrete);
        }
        return snprintf(out, out_sz,
            "    %s: ((%s*)(void*)&(__x__))->u.error, \\\n",
            concrete, concrete);
    default:
        return -1;
    }
}

void cc_emit_plan_fprint_line_directive(FILE* out, const char* src, size_t offset,
                                        const char* input_path) {
    char rel[1024];
    int resume_line = 1;
    if (!out || !src) return;
    for (size_t i = 0; i < offset; i++) {
        if (src[i] == '\n') resume_line++;
    }
    fprintf(out, "#line %d \"%s\"\n", resume_line,
            cc_path_rel_to_repo(input_path ? input_path : "<string>", rel, sizeof(rel)));
}
