/* edit_buffer.h - Edit collection and application for source-to-source transforms.
 *
 * Instead of: Parse → Edit → Reparse → Edit → Reparse → ...
 * We do:      Parse → Collect All Edits → Apply End-to-Start → Output
 *
 * Key insight: If edits are applied from end-of-file to start-of-file,
 * earlier offsets remain valid throughout the transformation.
 */

#ifndef CC_EDIT_BUFFER_H
#define CC_EDIT_BUFFER_H

#include <stddef.h>

#include "../diag/diag.h"

/* A single source edit: replace [start_off, end_off) with replacement text. */
typedef struct CCEdit {
    size_t start_off;           /* Start offset in source (inclusive) */
    size_t end_off;             /* End offset in source (exclusive) */
    char* replacement;          /* Replacement text (owned, freed on cleanup) */
    int priority;               /* For ordering edits at same position (higher = apply first) */
    const char* pass_name;      /* For debugging: which pass created this edit */
    CCSourceSpan origin_span;   /* User .ccs location (I5); CC_SPAN_NONE if unknown */
    const char* phase;          /* Pipeline phase for diagnostics (I2) */
    CCConstructKind construct_kind;
} CCEdit;

/* Collection of edits to apply to a source buffer. */
typedef struct CCEditBuffer {
    CCEdit* edits;              /* Array of edits */
    int count;                  /* Number of edits */
    int capacity;               /* Allocated capacity */
    
    /* Generated code to append at specific locations */
    char* protos;               /* Forward declarations (after includes) */
    size_t protos_len;
    char* defs;                 /* Definitions (at end of file) */
    size_t defs_len;
    
    /* Source info */
    const char* src;            /* Original source (not owned) */
    size_t src_len;
} CCEditBuffer;

/* Initialize an edit buffer for a source. */
void cc_edit_buffer_init(CCEditBuffer* eb, const char* src, size_t src_len);

/* Free an edit buffer and all owned memory. */
void cc_edit_buffer_free(CCEditBuffer* eb);

/* Add an edit to the buffer. Replacement is copied, caller still owns original. */
int cc_edit_buffer_add(CCEditBuffer* eb, size_t start_off, size_t end_off,
                       const char* replacement, int priority, const char* pass_name);

/* Same with origin span / phase / construct (I5). */
int cc_edit_buffer_add_ex(CCEditBuffer* eb, size_t start_off, size_t end_off,
                          const char* replacement, int priority, const char* pass_name,
                          CCSourceSpan origin_span, const char* phase,
                          CCConstructKind construct_kind);

/* After apply: register generated→origin mappings in source map. */
void cc_edit_buffer_register_spans(CCEditBuffer* eb, CCSourceMap* map,
                                   const char* default_phase);

/* Add generated code to be inserted after includes. */
int cc_edit_buffer_add_protos(CCEditBuffer* eb, const char* protos, size_t len);

/* Add generated code to be appended at end of file. */
int cc_edit_buffer_add_defs(CCEditBuffer* eb, const char* defs, size_t len);

/* Apply all edits and produce the transformed source.
 * Edits are sorted by position (descending) and applied end-to-start.
 * Returns newly allocated string (caller owns), or NULL on error.
 */
char* cc_edit_buffer_apply(CCEditBuffer* eb, size_t* out_len);

/* Find the offset at which file-scope forward declarations should be
 * inserted: just after the last top-level `#include` directive, or 0
 * if the buffer has none.  Used by callers that emit forward decls
 * (e.g. closure literal lifting) and want them visible to every call
 * site without splicing into block scope. */
size_t cc_find_protos_insertion_point(const char* src, size_t len);

/* Find the offset of the start of the line containing the first
 * top-level function DEFINITION (a depth-0 `T name(...) {` whose
 * preceding identifier is not a control-flow keyword).  Returns
 * src_len if none is found.  Useful when a caller wants to insert
 * forward declarations AFTER all top-level type declarations
 * (typedefs, structs) but BEFORE any function body that might call
 * the declared symbols. */
size_t cc_find_first_func_def_offset(const char* src, size_t len);

/* Debug: dump all edits to stderr. */
void cc_edit_buffer_dump(const CCEditBuffer* eb);

/* ============================================================================
 * Pass Integration
 * 
 * Each pass becomes an "edit collector" that adds edits to the buffer
 * without modifying the source directly:
 *
 *   int cc__collect_closure_edits(const CCASTRoot* ast, CCEditBuffer* eb);
 *   int cc__collect_spawn_edits(const CCASTRoot* ast, CCEditBuffer* eb);
 *   int cc__collect_nursery_edits(const CCASTRoot* ast, CCEditBuffer* eb);
 *   int cc__collect_ufcs_edits(const CCASTRoot* ast, CCEditBuffer* eb);
 *   etc.
 *
 * The orchestrator then:
 *   1. Parses once
 *   2. Calls all collectors
 *   3. Calls cc_edit_buffer_apply() once
 *   4. Outputs result
 * ============================================================================
 */

#endif /* CC_EDIT_BUFFER_H */
