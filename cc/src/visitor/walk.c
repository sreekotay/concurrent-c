#include "walk.h"

#include <errno.h>
#include <stdio.h>

int cc_visit_ast(const CCASTRoot* root, CCSymbolTable* symbols, const char* input_path, const char* output_path) {
    /* Prefer the pre-relower buffer (still has `[~ ... >]` chan brackets)
     * for visitor span passes that need to recover bracket specs from
     * macro-generated decls. Fall back to the post-relower buffer when
     * the pre-relower copy is unavailable. */
    const char* pe_buf = NULL;
    size_t      pe_len = 0;
    if (root) {
        if (root->parse_buffer_pre_relower) {
            pe_buf = root->parse_buffer_pre_relower;
            pe_len = root->parse_buffer_pre_relower_len;
        } else if (root->parse_buffer) {
            pe_buf = root->parse_buffer;
            pe_len = root->parse_buffer_len;
        }
    }
    CCVisitorCtx ctx = {
        .symbols = symbols,
        .input_path = input_path,
        .pre_expanded_buf = pe_buf,
        .pre_expanded_len = pe_len,
    };
    return cc_visit(root, &ctx, output_path);
}

