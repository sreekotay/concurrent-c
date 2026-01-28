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

/* A single source edit: replace [start_off, end_off) with replacement text. */
typedef struct CCEdit {
    size_t start_off;           /* Start offset in source (inclusive) */
    size_t end_off;             /* End offset in source (exclusive) */
    char* replacement;          /* Replacement text (owned, freed on cleanup) */
    int priority;               /* For ordering edits at same position (higher = apply first) */
    const char* pass_name;      /* For debugging: which pass created this edit */
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

/* Add generated code to be inserted after includes. */
int cc_edit_buffer_add_protos(CCEditBuffer* eb, const char* protos, size_t len);

/* Add generated code to be appended at end of file. */
int cc_edit_buffer_add_defs(CCEditBuffer* eb, const char* defs, size_t len);

/* Apply all edits and produce the transformed source.
 * Edits are sorted by position (descending) and applied end-to-start.
 * Returns newly allocated string (caller owns), or NULL on error.
 */
char* cc_edit_buffer_apply(CCEditBuffer* eb, size_t* out_len);

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
 *   int cc__collect_arena_edits(const CCASTRoot* ast, CCEditBuffer* eb);
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
