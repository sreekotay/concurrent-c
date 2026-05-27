/* edit_buffer.c - Edit collection and application for source-to-source transforms. */

#include "edit_buffer.h"
#include "../diag/source_map.h"
#include "../util/text_scan.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void cc_edit_buffer_init(CCEditBuffer* eb, const char* src, size_t src_len) {
    if (!eb) return;
    memset(eb, 0, sizeof(*eb));
    eb->src = src;
    eb->src_len = src_len;
}

void cc_edit_buffer_free(CCEditBuffer* eb) {
    if (!eb) return;
    for (int i = 0; i < eb->count; i++) {
        free(eb->edits[i].replacement);
    }
    free(eb->edits);
    free(eb->protos);
    free(eb->defs);
    memset(eb, 0, sizeof(*eb));
}

int cc_edit_buffer_add_ex(CCEditBuffer* eb, size_t start_off, size_t end_off,
                          const char* replacement, int priority, const char* pass_name,
                          CCSourceSpan origin_span, const char* phase,
                          CCConstructKind construct_kind) {
    if (!eb) return -1;
    if (start_off > end_off) return -1;
    if (end_off > eb->src_len) return -1;

    if (eb->count >= eb->capacity) {
        int new_cap = eb->capacity ? eb->capacity * 2 : 64;
        CCEdit* new_edits = realloc(eb->edits, new_cap * sizeof(CCEdit));
        if (!new_edits) return -1;
        eb->edits = new_edits;
        eb->capacity = new_cap;
    }

    CCEdit* e = &eb->edits[eb->count];
    e->start_off = start_off;
    e->end_off = end_off;
    e->replacement = replacement ? strdup(replacement) : strdup("");
    e->priority = priority;
    e->pass_name = pass_name;
    e->origin_span = origin_span;
    e->phase = phase ? phase : pass_name;
    e->construct_kind = construct_kind;

    if (!e->replacement) return -1;
    eb->count++;
    return 0;
}

int cc_edit_buffer_add(CCEditBuffer* eb, size_t start_off, size_t end_off,
                       const char* replacement, int priority, const char* pass_name) {
    return cc_edit_buffer_add_ex(eb, start_off, end_off, replacement, priority, pass_name,
                                 CC_SPAN_NONE, pass_name, CC_CONSTRUCT_NONE);
}

void cc_edit_buffer_register_spans(CCEditBuffer* eb, CCSourceMap* map,
                                   const char* default_phase) {
    if (!eb || !map) return;
    for (int i = 0; i < eb->count; i++) {
        const CCEdit* e = &eb->edits[i];
        if (cc_span_is_none(e->origin_span)) continue;
        CCSourceSpan gen;
        gen.start.byte_off = e->start_off;
        gen.end.byte_off = e->end_off;
        gen.start.line = gen.end.line = 0;
        const char* ph = e->phase ? e->phase : default_phase;
        cc_source_map_add(map, gen, e->origin_span, ph, e->construct_kind, NULL);
        if (cc_debug_enabled("SPANS")) {
            cc_debug_log(ph ? ph : "edit", "registered span off %zu..%zu", e->start_off, e->end_off);
        }
    }
}

int cc_edit_buffer_add_protos(CCEditBuffer* eb, const char* protos, size_t len) {
    if (!eb || !protos || len == 0) return 0;
    
    size_t new_len = eb->protos_len + len;
    char* new_protos = realloc(eb->protos, new_len + 1);
    if (!new_protos) return -1;
    
    memcpy(new_protos + eb->protos_len, protos, len);
    new_protos[new_len] = '\0';
    eb->protos = new_protos;
    eb->protos_len = new_len;
    return 0;
}

int cc_edit_buffer_add_defs(CCEditBuffer* eb, const char* defs, size_t len) {
    if (!eb || !defs || len == 0) return 0;
    
    size_t new_len = eb->defs_len + len;
    char* new_defs = realloc(eb->defs, new_len + 1);
    if (!new_defs) return -1;
    
    memcpy(new_defs + eb->defs_len, defs, len);
    new_defs[new_len] = '\0';
    eb->defs = new_defs;
    eb->defs_len = new_len;
    return 0;
}

/* Compare edits for sorting: by start_off descending, then priority descending */
static int edit_cmp(const void* a, const void* b) {
    const CCEdit* ea = (const CCEdit*)a;
    const CCEdit* eb = (const CCEdit*)b;
    
    /* Sort by start_off descending (apply from end to start) */
    if (ea->start_off > eb->start_off) return -1;
    if (ea->start_off < eb->start_off) return 1;
    
    /* At same position, higher priority first */
    if (ea->priority > eb->priority) return -1;
    if (ea->priority < eb->priority) return 1;
    
    return 0;
}

size_t cc_find_protos_insertion_point(const char* src, size_t len);

/* Find insertion point for protos (after last #include line) */
static size_t find_protos_insertion_point(const char* src, size_t len) {
    size_t last_include_end = 0;
    size_t i = 0;
    
    while (i < len) {
        /* Skip whitespace */
        while (i < len && (src[i] == ' ' || src[i] == '\t')) i++;
        
        /* Check for #include or # include */
        if (i < len && src[i] == '#') {
            i++;
            while (i < len && (src[i] == ' ' || src[i] == '\t')) i++;
            
            if (i + 7 <= len && strncmp(src + i, "include", 7) == 0) {
                /* Find end of this line */
                while (i < len && src[i] != '\n') i++;
                if (i < len) i++; /* Skip newline */
                last_include_end = i;
                continue;
            }
        }
        
        /* Skip to next line */
        while (i < len && src[i] != '\n') i++;
        if (i < len) i++;
    }
    
    return last_include_end;
}

size_t cc_find_protos_insertion_point(const char* src, size_t len) {
    return find_protos_insertion_point(src, len);
}

/* Walk the buffer skipping C comments / strings / chars / preprocessor
 * directives and return the start-of-line offset for the first top-level
 * function DEFINITION.  A function definition is a depth-0 `{` whose
 * preceding token sequence ends with `... ident (...) `, where `ident`
 * is not a control-flow keyword (`if`/`for`/`while`/`switch`/`return`/
 * `sizeof`/`do`).
 *
 * The exclusion of `do` is intentional: `do { ... } while(...)` could
 * otherwise be mistaken for a function definition.  Returns `len` when
 * the buffer has no top-level function definition (header-only file).
 */
size_t cc_find_first_func_def_offset(const char* src, size_t len) {
    if (!src || len == 0) return len;
    size_t i = 0;
    size_t last_line_off = 0;
    int brace_depth = 0;
    /* Inert-region tracking (comments/strings/chars/pp-directives) is
     * delegated to `CCInertScan`; `brace_depth` and `last_line_off` stay
     * here because they're specific to function-def detection.  Newlines
     * inside inert regions still need to advance `last_line_off`, so the
     * post-step scan below sweeps consumed bytes for `\n`. */
    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    while (i < len) {
        size_t before = i;
        if (cc_inert_scan_step(&scan, src, len, &i)) {
            for (size_t k = before; k < i; k++) {
                if (src[k] == '\n') last_line_off = k + 1;
            }
            continue;
        }
        char c = src[i];
        if (c == '\n') { last_line_off = i + 1; i++; continue; }
        if (c == '{') {
            if (brace_depth == 0) {
                size_t j = i;
                while (j > 0 && (src[j - 1] == ' ' || src[j - 1] == '\t' || src[j - 1] == '\n')) j--;
                if (j > 0 && src[j - 1] == ')') {
                    /* match the `(` that opens this paren group */
                    size_t rp = j - 1;
                    int par = 1;
                    size_t lp = rp;
                    while (lp > 0) {
                        lp--;
                        if (src[lp] == ')') par++;
                        else if (src[lp] == '(') { par--; if (par == 0) break; }
                    }
                    if (par == 0) {
                        /* identifier immediately before `(` */
                        size_t k = lp;
                        while (k > 0 && (src[k - 1] == ' ' || src[k - 1] == '\t')) k--;
                        size_t id_end = k;
                        while (k > 0 && (((src[k - 1] >= 'A' && src[k - 1] <= 'Z')) ||
                                         ((src[k - 1] >= 'a' && src[k - 1] <= 'z')) ||
                                         ((src[k - 1] >= '0' && src[k - 1] <= '9')) ||
                                         src[k - 1] == '_')) k--;
                        if (id_end > k) {
                            size_t kw_n = id_end - k;
                            const char* kw = src + k;
                            int is_keyword = ((kw_n == 2 && strncmp(kw, "if", 2) == 0) ||
                                              (kw_n == 2 && strncmp(kw, "do", 2) == 0) ||
                                              (kw_n == 3 && strncmp(kw, "for", 3) == 0) ||
                                              (kw_n == 5 && strncmp(kw, "while", 5) == 0) ||
                                              (kw_n == 6 && strncmp(kw, "switch", 6) == 0) ||
                                              (kw_n == 6 && strncmp(kw, "return", 6) == 0) ||
                                              (kw_n == 6 && strncmp(kw, "sizeof", 6) == 0));
                            if (!is_keyword) return last_line_off;
                        }
                    }
                }
            }
            brace_depth++;
            i++;
            continue;
        }
        if (c == '}') {
            if (brace_depth > 0) brace_depth--;
            i++;
            continue;
        }
        i++;
    }
    return len;
}

char* cc_edit_buffer_apply(CCEditBuffer* eb, size_t* out_len) {
    if (!eb || !eb->src) return NULL;
    
    /* Sort edits by position descending */
    if (eb->count > 1) {
        qsort(eb->edits, eb->count, sizeof(CCEdit), edit_cmp);
    }
    
    /* Validate: no overlapping edits */
    for (int i = 0; i < eb->count - 1; i++) {
        /* After sorting descending, edits[i] starts at or after edits[i+1] */
        if (eb->edits[i].start_off < eb->edits[i + 1].end_off) {
            fprintf(stderr, "cc_edit_buffer: overlapping edits at %zu-%zu and %zu-%zu (passes: %s, %s)\n",
                    eb->edits[i].start_off, eb->edits[i].end_off,
                    eb->edits[i + 1].start_off, eb->edits[i + 1].end_off,
                    eb->edits[i].pass_name ? eb->edits[i].pass_name : "?",
                    eb->edits[i + 1].pass_name ? eb->edits[i + 1].pass_name : "?");
            return NULL;
        }
    }
    
    /* Calculate output size */
    size_t result_len = eb->src_len;
    for (int i = 0; i < eb->count; i++) {
        size_t removed = eb->edits[i].end_off - eb->edits[i].start_off;
        size_t added = strlen(eb->edits[i].replacement);
        result_len = result_len - removed + added;
    }
    result_len += eb->protos_len + eb->defs_len;
    
    /* Allocate result buffer */
    char* result = malloc(result_len + 1);
    if (!result) return NULL;
    
    /* Copy source with edits applied (end-to-start) */
    char* working = malloc(eb->src_len + 1);
    if (!working) { free(result); return NULL; }
    memcpy(working, eb->src, eb->src_len);
    working[eb->src_len] = '\0';
    size_t working_len = eb->src_len;
    
    /* Apply edits end-to-start */
    for (int i = 0; i < eb->count; i++) {
        CCEdit* e = &eb->edits[i];
        size_t removed = e->end_off - e->start_off;
        size_t added = strlen(e->replacement);
        
        /* Make room or shrink */
        size_t new_len = working_len - removed + added;
        char* new_working = malloc(new_len + 1);
        if (!new_working) { free(working); free(result); return NULL; }
        
        /* Copy: [0, start_off) + replacement + [end_off, working_len) */
        memcpy(new_working, working, e->start_off);
        memcpy(new_working + e->start_off, e->replacement, added);
        memcpy(new_working + e->start_off + added, working + e->end_off, working_len - e->end_off);
        new_working[new_len] = '\0';
        
        free(working);
        working = new_working;
        working_len = new_len;
    }
    
    /* Insert protos after includes */
    if (eb->protos_len > 0) {
        size_t insert_pt = find_protos_insertion_point(working, working_len);
        size_t new_len = working_len + eb->protos_len;
        char* new_working = malloc(new_len + 1);
        if (!new_working) { free(working); free(result); return NULL; }
        
        memcpy(new_working, working, insert_pt);
        memcpy(new_working + insert_pt, eb->protos, eb->protos_len);
        memcpy(new_working + insert_pt + eb->protos_len, working + insert_pt, working_len - insert_pt);
        new_working[new_len] = '\0';
        
        free(working);
        working = new_working;
        working_len = new_len;
    }
    
    /* Append defs at end */
    if (eb->defs_len > 0) {
        size_t new_len = working_len + eb->defs_len;
        char* new_working = realloc(working, new_len + 1);
        if (!new_working) { free(working); free(result); return NULL; }
        
        memcpy(new_working + working_len, eb->defs, eb->defs_len);
        new_working[new_len] = '\0';
        working = new_working;
        working_len = new_len;
    }
    
    /* Move to result */
    free(result);
    result = working;
    
    if (out_len) *out_len = working_len;
    return result;
}

void cc_edit_buffer_dump(const CCEditBuffer* eb) {
    if (!eb) return;
    fprintf(stderr, "CCEditBuffer: %d edits, %zu protos, %zu defs\n",
            eb->count, eb->protos_len, eb->defs_len);
    for (int i = 0; i < eb->count; i++) {
        const CCEdit* e = &eb->edits[i];
        fprintf(stderr, "  [%zu-%zu] prio=%d pass=%s: \"%.40s%s\"\n",
                e->start_off, e->end_off, e->priority,
                e->pass_name ? e->pass_name : "?",
                e->replacement,
                strlen(e->replacement) > 40 ? "..." : "");
    }
}
