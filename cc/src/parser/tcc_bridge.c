#include "tcc_bridge.h"

#include "cc_macro_recognizer.h"
#include "../../../third_party/tcc-patches/tcc_ext_api.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

// Thin wrappers around patched TCC hooks when available. When built without
// CC_TCC_EXT_AVAILABLE, we provide stubs that return NULL.

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>
/* tcc.h redefines malloc/free to TCC's internal allocators; we want libc here. */
#ifdef malloc
#undef malloc
#endif
#ifdef free
#undef free
#endif
#ifdef strdup
#undef strdup
#endif

/* DRIFT GUARD: the stub-node layout is mirrored on the CC side (CCNodeView).
 * A field added on one side without the other reads garbage through the
 * array stride — caught here at compile time instead of as a segfault.
 * (visitor_ast_common.h holds a second mirror that cannot be included in
 * the same TU — same struct tag; phase 1 of PASS_CLEANUP_PLAN consolidates
 * the mirrors to one header, guarded by this assert.) */
#define CC_PASS_COMMON_SKIP_KINDS
#include "visitor/pass_common.h"
_Static_assert(sizeof(struct CCASTStubNode) == sizeof(CCNodeView),
               "CCNodeView mirror out of sync with tcc.h CCASTStubNode "
               "(apply third_party/tcc-patches + rebuild libtcc, or update the mirrors)");

/* The patched TCC should export these. Mark weak so we can still link if the
   extension is absent, and fall back to stubs at runtime. */
__attribute__((weak)) struct CCASTStubRoot* cc_tcc_parse_to_ast(const char* preprocessed_path, const char* original_path, CCSymbolTable* symbols);
__attribute__((weak)) struct CCASTStubRoot* cc_tcc_parse_string_to_ast(const char* source_code, const char* virtual_filename, const char* original_path, CCSymbolTable* symbols);
__attribute__((weak)) void cc_tcc_free_ast(struct CCASTStubRoot* r);
__attribute__((weak)) void tcc_set_ext_parser(struct TCCExtParser const *p);
extern const struct TCCExtParser cc_ext_parser;

/* Capture TCC's stderr; on success discard it (benign warnings), on failure
   replay it so the user sees actual error messages. */
static void cc__tcc_stderr_capture_start(int* saved_fd, char* tmppath, size_t tmppath_sz) {
    *saved_fd = -1;
    tmppath[0] = '\0';
    if (getenv("CC_DEBUG_TCC_WARNINGS")) return;
    fflush(stderr);
    *saved_fd = dup(STDERR_FILENO);
    snprintf(tmppath, tmppath_sz, "/tmp/cc_tcc_stderr_XXXXXX");
    int tmpfd = mkstemp(tmppath);
    if (tmpfd >= 0) { dup2(tmpfd, STDERR_FILENO); close(tmpfd); }
}
static void cc__tcc_stderr_capture_end(int saved_fd, const char* tmppath, int parse_failed) {
    if (saved_fd < 0) return;
    fflush(stderr);
    dup2(saved_fd, STDERR_FILENO);
    close(saved_fd);
    if (tmppath[0]) {
        if (parse_failed) {
            FILE* f = fopen(tmppath, "r");
            if (f) {
                char buf[512];
                while (fgets(buf, sizeof(buf), f)) fputs(buf, stderr);
                fclose(f);
            }
        }
        unlink(tmppath);
    }
}

// Call into patched TCC to parse and return an opaque AST root.
CCASTRoot* cc_tcc_bridge_parse_to_ast(const char* preprocessed_path, const char* original_path, CCSymbolTable* symbols) {
    if (!preprocessed_path || !cc_tcc_parse_to_ast) return NULL;
    (void)symbols;
    if (tcc_set_ext_parser) {
        tcc_set_ext_parser(&cc_ext_parser);
    }
    cc_macro_recognizer_register(NULL);
    int _saved_fd = -1; char _tmppath[256];
    cc__tcc_stderr_capture_start(&_saved_fd, _tmppath, sizeof(_tmppath));
    struct CCASTStubRoot* r = cc_tcc_parse_to_ast(preprocessed_path, original_path, symbols);
    cc__tcc_stderr_capture_end(_saved_fd, _tmppath, r == NULL);
    if (!r) return NULL;
    CCASTRoot* root = (CCASTRoot*)malloc(sizeof(CCASTRoot));
    if (!root) {
        cc_tcc_free_ast(r);
        return NULL;
    }
    memset(root, 0, sizeof(*root));
    root->lowered_path = strdup(preprocessed_path);
    if (!root->lowered_path) {
        cc_tcc_free_ast(r);
        free(root);
        return NULL;
    }
    root->tcc_root = r;
    root->nodes = (const struct CCASTStubNode*)r->nodes;
    root->node_count = r->count;
    root->skipped_clo_ids = r->skipped_clo_ids;
    root->skipped_clo_count = r->skipped_clo_count;

    /* Debug: dump stub nodes (best-effort) */
    if (getenv("CC_DEBUG_STUB_NODES") && root->nodes && root->node_count > 0) {
        const struct CCASTStubNode* nn = root->nodes;
        int arenas = 0;
        for (int i = 0; i < root->node_count; i++) {
            if (nn[i].kind == 4) arenas++;
        }
        fprintf(stderr, "CC_DEBUG_STUB_NODES: %s: nodes=%d arenas=%d\n",
                original_path ? original_path : "<input>", root->node_count, arenas);
        for (int i = 0; i < root->node_count; i++) {
            if (nn[i].kind == 4) {
                fprintf(stderr,
                        "  [arena] idx=%d parent=%d file=%s line=%d..%d col=%d..%d name=%s size=%s\n",
                        i, nn[i].parent,
                        nn[i].file ? nn[i].file : "<null>",
                        nn[i].line_start, nn[i].line_end,
                        nn[i].col_start, nn[i].col_end,
                        nn[i].aux_s1 ? nn[i].aux_s1 : "<null>",
                        nn[i].aux_s2 ? nn[i].aux_s2 : "<null>");
            }
        }
    }
    return root;
}

// Parse from in-memory source string (no temp files).
CCASTRoot* cc_tcc_bridge_parse_string_to_ast(const char* source_code, const char* virtual_filename, const char* original_path, CCSymbolTable* symbols) {
    if (!source_code || !cc_tcc_parse_string_to_ast) return NULL;
    (void)symbols;
    if (tcc_set_ext_parser) {
        tcc_set_ext_parser(&cc_ext_parser);
    }
    cc_macro_recognizer_register(NULL);
    int _saved_fd2 = -1; char _tmppath2[256];
    cc__tcc_stderr_capture_start(&_saved_fd2, _tmppath2, sizeof(_tmppath2));
    struct CCASTStubRoot* r = cc_tcc_parse_string_to_ast(source_code, virtual_filename, original_path, symbols);
    cc__tcc_stderr_capture_end(_saved_fd2, _tmppath2, r == NULL);
    if (!r) return NULL;
    CCASTRoot* root = (CCASTRoot*)malloc(sizeof(CCASTRoot));
    if (!root) {
        cc_tcc_free_ast(r);
        return NULL;
    }
    memset(root, 0, sizeof(*root));
    // No lowered_path for string-based parsing (no temp file to clean up).
    root->lowered_path = virtual_filename ? strdup(virtual_filename) : NULL;
    root->lowered_is_temp = 0; // No temp file to delete.
    root->tcc_root = r;
    root->nodes = (const struct CCASTStubNode*)r->nodes;
    root->node_count = r->count;
    root->skipped_clo_ids = r->skipped_clo_ids;
    root->skipped_clo_count = r->skipped_clo_count;

    /* Debug: dump stub nodes (best-effort) */
    if (getenv("CC_DEBUG_STUB_NODES") && root->nodes && root->node_count > 0) {
        const struct CCASTStubNode* nn = root->nodes;
        int arenas = 0;
        for (int i = 0; i < root->node_count; i++) {
            if (nn[i].kind == 4) arenas++;
        }
        fprintf(stderr, "CC_DEBUG_STUB_NODES: %s: nodes=%d arenas=%d\n",
                original_path ? original_path : "<string>", root->node_count, arenas);
        /* Nesting-depth report: the checker recurses the child graph, so a
         * parent-chain leak (missed record_end) shows up here as a depth
         * near the node count instead of the syntactic nesting. */
        {
            int maxd = 0, maxi = -1;
            for (int i = 0; i < root->node_count; i++) {
                int d = 0;
                for (int p = nn[i].parent; p >= 0 && p < root->node_count && d <= root->node_count; p = nn[p].parent) d++;
                if (d > maxd) { maxd = d; maxi = i; }
            }
            fprintf(stderr, "CC_DEBUG_STUB_NODES: max parent depth=%d at node %d\n", maxd, maxi);
            if (maxi >= 0) {
                int c = 0;
                fprintf(stderr, "  deepest chain (node(kind,line), nearest first):");
                for (int p = maxi; p >= 0 && p < root->node_count && c < 48; p = nn[p].parent, c++)
                    fprintf(stderr, " %d(k%d,L%d)", p, nn[p].kind, nn[p].line_start);
                fprintf(stderr, "%s\n", c >= 48 ? " ..." : "");
            }
        }
        /* =2: verify byte offsets by slicing the parsed text directly —
         * off_* address source_code (the buffer TCC lexed), NOT the
         * original file; that is the whole point of carrying them. */
        const char* lvl = getenv("CC_DEBUG_STUB_NODES");
        if (lvl && lvl[0] == '2' && source_code) {
            size_t srclen = strlen(source_code);
            int shown = 0;
            for (int i = 0; i < root->node_count && shown < 24; i++) {
                if (nn[i].off_start < 0 || nn[i].off_end <= nn[i].off_start) continue;
                if ((size_t)nn[i].off_end > srclen) continue;
                long a = nn[i].off_start, b = nn[i].off_end;
                long w = b - a; if (w > 40) w = 40;
                fprintf(stderr, "  node[%d] kind=%d L%d:%d off=%ld..%ld |%.*s|\n",
                        i, nn[i].kind, nn[i].line_start, nn[i].col_start,
                        a, b, (int)w, source_code + a);
                shown++;
            }
        }
    }
    return root;
}

void cc_tcc_bridge_free_ast(CCASTRoot* root) {
    if (!root) return;
    const char* keep_pp = getenv("CC_KEEP_PP");
    if (root->lowered_is_temp && root->lowered_path && !(keep_pp && keep_pp[0] == '1')) {
        unlink(root->lowered_path);
    }
    if (root->lowered_path) {
        free(root->lowered_path);
        root->lowered_path = NULL;
    }
    if (root->tcc_root) {
        cc_tcc_free_ast((struct CCASTStubRoot*)root->tcc_root);
    }
    if (root->parse_buffer) {
        free(root->parse_buffer);
        root->parse_buffer = NULL;
        root->parse_buffer_len = 0;
    }
    if (root->parse_buffer_pre_relower) {
        free(root->parse_buffer_pre_relower);
        root->parse_buffer_pre_relower = NULL;
        root->parse_buffer_pre_relower_len = 0;
    }
    if (root->codegen_buffer) {
        free(root->codegen_buffer);
        root->codegen_buffer = NULL;
        root->codegen_buffer_len = 0;
    }
    free(root);
}
#else

CCASTRoot* cc_tcc_bridge_parse_to_ast(const char* preprocessed_path, const char* original_path, CCSymbolTable* symbols) {
    (void)preprocessed_path;
    (void)original_path;
    (void)symbols;
    return NULL;
}

CCASTRoot* cc_tcc_bridge_parse_string_to_ast(const char* source_code, const char* virtual_filename, const char* original_path, CCSymbolTable* symbols) {
    (void)source_code;
    (void)virtual_filename;
    (void)original_path;
    (void)symbols;
    return NULL;
}

void cc_tcc_bridge_free_ast(CCASTRoot* root) {
    free(root);
}

#endif

