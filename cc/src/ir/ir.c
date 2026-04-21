/* CC-IR core: arena, node allocation, build-from-stub, emit.
 *
 * See cc/src/ir/ir.h for the public interface and
 * docs/refactor-ast-truth.md for the refactor plan this implements. */
#include "ir/ir.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ast/ast.h"
#include "util/text.h"

/* ------------------------------------------------------------------- */
/* Arena                                                                */
/* ------------------------------------------------------------------- */

/* Simple bump-allocator with a chain of blocks.  Each block is
 * `CC_IR_ARENA_BLOCK_DEFAULT` bytes by default; oversized requests get
 * their own dedicated block so we never split a single allocation.
 *
 * We do NOT reuse freed nodes — the whole arena is freed at once in
 * `cc_ir_arena_destroy`.  This matches the per-TU lifetime model
 * documented in docs/refactor-ast-truth.md. */

#define CC_IR_ARENA_BLOCK_DEFAULT ((size_t)64 * 1024)

typedef struct CCIrArenaBlock {
    struct CCIrArenaBlock* next;
    size_t                 cap;
    size_t                 used;
    /* data[] follows contiguously; placement computed via offsetof. */
    char                   data[];
} CCIrArenaBlock;

struct CCIrArena {
    CCIrArenaBlock* head;
    size_t          total_bytes; /* for metrics */
    cc_ir_node_id_t next_id;
};

static CCIrArenaBlock* cc_ir_arena_new_block(size_t min_bytes) {
    size_t cap = CC_IR_ARENA_BLOCK_DEFAULT;
    if (min_bytes > cap) cap = min_bytes;
    CCIrArenaBlock* b = (CCIrArenaBlock*)malloc(sizeof(CCIrArenaBlock) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->cap  = cap;
    b->used = 0;
    return b;
}

CCIrArena* cc_ir_arena_create(void) {
    CCIrArena* a = (CCIrArena*)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->head = cc_ir_arena_new_block(CC_IR_ARENA_BLOCK_DEFAULT);
    if (!a->head) {
        free(a);
        return NULL;
    }
    a->total_bytes = 0;
    a->next_id     = 1; /* 0 is reserved for "none" */
    return a;
}

void cc_ir_arena_destroy(CCIrArena* arena) {
    if (!arena) return;
    CCIrArenaBlock* b = arena->head;
    while (b) {
        CCIrArenaBlock* next = b->next;
        free(b);
        b = next;
    }
    free(arena);
}

void* cc_ir_alloc(CCIrArena* arena, size_t n) {
    if (!arena || n == 0) return NULL;
    /* Round up for alignment.  sizeof(void*) is enough for our payloads
     * (no larger-aligned types land in the IR). */
    size_t align = sizeof(void*);
    n = (n + (align - 1)) & ~(align - 1);

    CCIrArenaBlock* b = arena->head;
    if (b->used + n > b->cap) {
        CCIrArenaBlock* nb = cc_ir_arena_new_block(n);
        if (!nb) return NULL;
        nb->next    = b;
        arena->head = nb;
        b           = nb;
    }
    void* p = b->data + b->used;
    b->used += n;
    arena->total_bytes += n;
    memset(p, 0, n);
    return p;
}

char* cc_ir_strndup(CCIrArena* arena, const char* src, size_t n) {
    if (!arena) return NULL;
    if (n > 0 && !src) return NULL;
    char* buf = (char*)cc_ir_alloc(arena, n + 1);
    if (!buf) return NULL;
    if (n > 0) memcpy(buf, src, n);
    buf[n] = '\0';
    return buf;
}

size_t cc_ir_arena_bytes(const CCIrArena* arena) {
    return arena ? arena->total_bytes : 0;
}

/* ------------------------------------------------------------------- */
/* Node allocation                                                      */
/* ------------------------------------------------------------------- */

CCIrNode* cc_ir_node_new(CCIrArena* arena, CCIrKind kind, CCIrSpan span) {
    if (!arena) return NULL;
    CCIrNode* n = (CCIrNode*)cc_ir_alloc(arena, sizeof(*n));
    if (!n) return NULL;
    n->id       = arena->next_id++;
    n->kind     = kind;
    n->span     = span;
    n->stub     = NULL;
    n->raw_text = NULL;
    n->raw_len  = 0;
    n->children     = NULL;
    n->children_len = 0;
    n->children_cap = 0;
    memset(&n->as, 0, sizeof(n->as));
    return n;
}

int cc_ir_node_append_child(CCIrArena* arena,
                            CCIrNode* parent,
                            CCIrNode* child) {
    if (!arena || !parent || !child) return -1;
    if (parent->children_len >= parent->children_cap) {
        size_t new_cap = parent->children_cap ? parent->children_cap * 2 : 4;
        CCIrNode** nc  = (CCIrNode**)cc_ir_alloc(arena, new_cap * sizeof(*nc));
        if (!nc) return -1;
        if (parent->children_len > 0) {
            memcpy(nc, parent->children,
                   parent->children_len * sizeof(*nc));
        }
        /* old children array remains in the arena; it'll be freed with
         * everyone else at arena teardown.  No individual free. */
        parent->children     = nc;
        parent->children_cap = new_cap;
    }
    parent->children[parent->children_len++] = child;
    return 0;
}

/* ------------------------------------------------------------------- */
/* Build-from-stub: phase-1a recogniser                                 */
/* ------------------------------------------------------------------- */

/* Allocate an OPAQUE_TEXT child covering src[byte_start..byte_end) and
 * append it to `file`.  No-op for empty slices. */
static int cc_ir_append_opaque(CCIrArena* arena, CCIrNode* file,
                               const char* src,
                               size_t byte_start, size_t byte_end) {
    if (byte_end <= byte_start) return 0;
    CCIrSpan sp   = {byte_start, byte_end, 0, 0};
    CCIrNode* t   = cc_ir_node_new(arena, CC_IR_OPAQUE_TEXT, sp);
    if (!t) return -1;
    size_t slen   = byte_end - byte_start;
    t->raw_text   = cc_ir_strndup(arena, src + byte_start, slen);
    if (!t->raw_text) return -1;
    t->raw_len    = slen;
    return cc_ir_node_append_child(arena, file, t);
}

/* Sigil-token recogniser (step 1.3a).
 *
 * For every top-level `!>` / `?>` in `src[0..src_len)`, emit a typed
 * CC_IR_UNWRAP_BANG / CC_IR_UNWRAP_Q node with span covering just the
 * 2 sigil bytes, interleaved with CC_IR_OPAQUE_TEXT spans for
 * everything else.
 *
 * Phase-1a deliberately records only the sigil position and kind.  The
 * structured payload (lhs / body / binder / types) is left zero — step
 * 1.3b will extend the recogniser to walk the construct boundaries and
 * populate those fields, and only then will the pass-port replace the
 * legacy text rewriter.
 *
 * The value today is two-fold:
 *   1. Proves the IR can accurately locate every `!>` / `?>` site in
 *      real source, skipping comments, strings, `!=`, `?:`, and so on
 *      (via `cc_find_substr_top_level`).  Correctness is verified by
 *      running `CC_VERIFY_IR=1 tools/cc_test` — the emitter rebuilds
 *      the text byte-for-byte from the tree, so any mis-identification
 *      surfaces as a verifier diagnostic.
 *   2. Gives subsequent passes a structured handle to iterate over
 *      unwrap sites (`file->children` filtered by kind) instead of
 *      re-scanning the text from scratch. */
static int cc_ir_carve_sigils(CCIrArena* arena, CCIrNode* file,
                              const char* src, size_t n) {
    size_t cursor = 0;
    while (cursor < n) {
        size_t bang = cc_find_substr_top_level(src, cursor, n, "!>", 2);
        size_t qmrk = cc_find_substr_top_level(src, cursor, n, "?>", 2);
        size_t next_sigil = bang < qmrk ? bang : qmrk;
        if (next_sigil >= n) {
            /* No more sigils; emit the trailing text slice. */
            return cc_ir_append_opaque(arena, file, src, cursor, n);
        }
        /* Emit opaque prefix before the sigil. */
        if (cc_ir_append_opaque(arena, file, src, cursor, next_sigil) != 0)
            return -1;
        /* Emit the sigil as a typed node.  raw_text carries the 2-byte
         * sigil so the emitter can reproduce it exactly, structured
         * payload stays zero-initialised until step 1.3b. */
        CCIrKind  k  = (src[next_sigil] == '!') ? CC_IR_UNWRAP_BANG
                                                : CC_IR_UNWRAP_Q;
        CCIrSpan  sp = {next_sigil, next_sigil + 2, 0, 0};
        CCIrNode* un = cc_ir_node_new(arena, k, sp);
        if (!un) return -1;
        un->raw_text = cc_ir_strndup(arena, src + next_sigil, 2);
        if (!un->raw_text) return -1;
        un->raw_len  = 2;
        if (cc_ir_node_append_child(arena, file, un) != 0) return -1;
        cursor = next_sigil + 2;
    }
    return 0;
}

CCIrNode* cc_ir_build_from_stub(CCIrArena* arena,
                                const CCASTRoot* root,
                                const char* src,
                                size_t src_len,
                                const char* input_path) {
    (void)root;        /* reserved for per-phase construct recognition */
    (void)input_path;  /* reserved for diagnostics */
    if (!arena || (!src && src_len > 0)) return NULL;

    CCIrSpan file_span = {0, src_len, 1, 1};
    CCIrNode* file = cc_ir_node_new(arena, CC_IR_FILE, file_span);
    if (!file) return NULL;
    /* The root gets an arena copy of the full source too — handy for
     * debug dumps and emitter fallback paths that need to slice bytes
     * beyond a specific child's span. */
    file->raw_text = cc_ir_strndup(arena, src ? src : "", src_len);
    if (!file->raw_text) return NULL;
    file->raw_len  = src_len;

    if (src && src_len > 0) {
        if (cc_ir_carve_sigils(arena, file, src, src_len) != 0)
            return NULL;
    }
    return file;
}

/* ------------------------------------------------------------------- */
/* Emit                                                                 */
/* ------------------------------------------------------------------- */

/* Bounded grow helper for the malloc'd output buffer. */
static int cc_ir_emit_append(char** buf, size_t* len, size_t* cap,
                             const char* s, size_t n) {
    if (!buf || !len || !cap) return -1;
    if (n == 0) return 0;
    if (*len + n + 1 > *cap) {
        size_t new_cap = *cap ? *cap : 1024;
        while (new_cap < *len + n + 1) new_cap *= 2;
        char* nb = (char*)realloc(*buf, new_cap);
        if (!nb) return -1;
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int cc_ir_emit_node(const CCIrNode* n,
                           char** buf, size_t* len, size_t* cap) {
    if (!n) return 0;
    switch (n->kind) {
        case CC_IR_FILE: {
            /* The FILE root's own raw_text is not emitted — only its
             * children contribute bytes to the output.  The raw copy
             * on the root is for debug/fallback lookups. */
            for (size_t i = 0; i < n->children_len; ++i) {
                if (cc_ir_emit_node(n->children[i], buf, len, cap) != 0)
                    return -1;
            }
            return 0;
        }
        case CC_IR_OPAQUE_TEXT:
        case CC_IR_UNWRAP_BANG:
        case CC_IR_UNWRAP_Q: {
            /* Phase-1a: every recognised node carries its literal source
             * slice in `raw_text`, so the emitter is identical across
             * kinds.  Structured emitters for UNWRAP_BANG / UNWRAP_Q
             * will land in step 1.3b and will stop consulting raw_text. */
            return cc_ir_emit_append(buf, len, cap, n->raw_text, n->raw_len);
        }
        case CC_IR_INVALID:
        default:
            return -1;
    }
}

int cc_ir_emit_text(const CCIrNode* root,
                    char** out_src, size_t* out_len) {
    if (!root || !out_src || !out_len) return -1;
    char*  buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    if (cc_ir_emit_node(root, &buf, &len, &cap) != 0) {
        free(buf);
        return -1;
    }
    if (!buf) {
        /* empty file: emit a single NUL so callers can treat the output
         * as a valid C string without special-casing. */
        buf = (char*)malloc(1);
        if (!buf) return -1;
        buf[0] = '\0';
    }
    *out_src = buf;
    *out_len = len;
    return 0;
}

/* ------------------------------------------------------------------- */
/* Debug dump                                                           */
/* ------------------------------------------------------------------- */

static const char* cc_ir_kind_name(CCIrKind k) {
    switch (k) {
        case CC_IR_INVALID:     return "INVALID";
        case CC_IR_FILE:        return "FILE";
        case CC_IR_OPAQUE_TEXT: return "OPAQUE_TEXT";
        case CC_IR_UNWRAP_BANG: return "UNWRAP_BANG";
        case CC_IR_UNWRAP_Q:    return "UNWRAP_Q";
    }
    return "?";
}

static void cc_ir_dump_node(const CCIrNode* n, FILE* fp, int depth) {
    if (!n) return;
    for (int i = 0; i < depth; ++i) fputs("  ", fp);
    fprintf(fp, "#%u %s [%zu..%zu) line=%d col=%d",
            (unsigned)n->id, cc_ir_kind_name(n->kind),
            n->span.byte_start, n->span.byte_end,
            n->span.orig_line, n->span.orig_col);
    if (n->raw_text && n->raw_len > 0) {
        size_t shown = n->raw_len > 40 ? 40 : n->raw_len;
        fprintf(fp, " raw=%zu:\"", n->raw_len);
        for (size_t i = 0; i < shown; ++i) {
            char c = n->raw_text[i];
            if (c == '\n') fputs("\\n", fp);
            else if (c == '\t') fputs("\\t", fp);
            else if (c >= 32 && c < 127) fputc(c, fp);
            else fprintf(fp, "\\x%02x", (unsigned char)c);
        }
        fputs(shown < n->raw_len ? "...\"" : "\"", fp);
    }
    if (n->kind == CC_IR_UNWRAP_BANG || n->kind == CC_IR_UNWRAP_Q) {
        /* Structured payload (populated starting in step 1.3b). */
        if (n->as.unwrap.binder_name || n->as.unwrap.err_type ||
            n->as.unwrap.body_len    || n->as.unwrap.lhs_len) {
            fprintf(fp, " lhs=%zuB body=%zuB binder=%s err=%s diverges=%d stmt=%d",
                    n->as.unwrap.lhs_len,
                    n->as.unwrap.body_len,
                    n->as.unwrap.binder_name ? n->as.unwrap.binder_name : "-",
                    n->as.unwrap.err_type    ? n->as.unwrap.err_type    : "-",
                    (int)n->as.unwrap.diverges,
                    (int)n->as.unwrap.stmt_position);
        }
    }
    fputc('\n', fp);
    for (size_t i = 0; i < n->children_len; ++i) {
        cc_ir_dump_node(n->children[i], fp, depth + 1);
    }
}

void cc_ir_dump(const CCIrNode* root, FILE* fp) {
    if (!fp) fp = stderr;
    cc_ir_dump_node(root, fp, 0);
}
