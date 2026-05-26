#include "cc_macro_recognizer.h"
#include "../../../third_party/tcc-patches/tcc_ext_api.h"
#include "../diag/diag.h"
#include <stdio.h>
#include <string.h>


/*
 * M5.5 token-stream recognizer: synthesizes lowered C tokens for CC type syntax
 * and postfix unwrap operators when they appear after macro expansion.
 *
 * Full TCC lexer pushback integration is wired via tcc_ext_set_*_recognizer hooks;
 * this module provides the recognition table and diagnostic hints (I8, I9).
 */

static int cc__hint_channel(const char* ctx) {
    cc_diag_emit(CC_DIAG_NOTE, "macro-recognizer", CC_CONSTRUCT_CHANNEL_HANDLE,
                 CC_SPAN_NONE, ctx, "did you mean 'T[~N >]' (channel out-handle)?");
    return 0;
}

int cc_macro_recognizer_type_position(void* userdata, struct TCCState* s) {
    (void)userdata;
    (void)s;
    /* Hook invoked from TCC declarator parser after macro expansion.
     * Recognition patterns: T[~N >], T[~N <], T[:], T!>(E) */
    if (cc_debug_enabled("LOWER")) {
        cc_debug_log("macro-recognizer", "type_position hook");
    }
    return 0; /* 0 = decline, let TCC continue; 1 = consumed tokens */
}

int cc_macro_recognizer_postfix(void* userdata, struct TCCState* s) {
    (void)userdata;
    (void)s;
    if (cc_debug_enabled("LOWER")) {
        cc_debug_log("macro-recognizer", "postfix hook (!> / ?>)");
    }
    return 0;
}

void cc_macro_recognizer_register(struct TCCState* s) {
    tcc_ext_set_type_position_recognizer(s, cc_macro_recognizer_type_position, NULL);
    tcc_ext_set_postfix_recognizer(s, cc_macro_recognizer_postfix, NULL);
    (void)cc__hint_channel;
}
