#include "../../../third_party/tcc-patches/tcc_ext_api.h"
#include "../diag/diag.h"
#include "util/text.h"
#include <string.h>

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>
#endif

static TCCExtDiagFn g_diag_fn;
static void* g_diag_ud;
static TCCExtTypePositionRecognizerFn g_type_recognizer;
static void* g_type_recognizer_ud;
static TCCExtPostfixRecognizerFn g_postfix_recognizer;
static void* g_postfix_recognizer_ud;

void tcc_ext_api_get_version(int* major, int* minor) {
    if (major) *major = TCC_EXT_API_VERSION_MAJOR;
    if (minor) *minor = TCC_EXT_API_VERSION_MINOR;
}

#ifdef CC_TCC_EXT_AVAILABLE
TCCState* tcc_ext_get_state(void) {
    extern TCCState* tcc_state;
    return tcc_state;
}
#else
TCCState* tcc_ext_get_state(void) { return NULL; }
#endif

int tcc_ext_current_token(void) {
#ifdef CC_TCC_EXT_AVAILABLE
    extern int tok;
    return tok;
#else
    return 0;
#endif
}

const char* tcc_ext_current_token_str(void) {
#ifdef CC_TCC_EXT_AVAILABLE
    extern int tok;
    extern CValue tokc;
    extern const char* get_tok_str(int v, CValue* cv);
    return get_tok_str(tok, &tokc);
#else
    return "";
#endif
}

int tcc_ext_current_line(void) {
    /* Line/column come from TCC parser state at hook invocation sites (M5). */
    return 0;
}

int tcc_ext_current_col(void) {
    (void)0;
    return 0;
}

void tcc_ext_set_diag_callback(TCCState* s, TCCExtDiagFn fn, void* userdata) {
    (void)s;
    g_diag_fn = fn;
    g_diag_ud = userdata;
}

void tcc_ext_set_type_position_recognizer(TCCState* s, TCCExtTypePositionRecognizerFn fn, void* userdata) {
    (void)s;
    g_type_recognizer = fn;
    g_type_recognizer_ud = userdata;
}

void tcc_ext_set_postfix_recognizer(TCCState* s, TCCExtPostfixRecognizerFn fn, void* userdata) {
    (void)s;
    g_postfix_recognizer = fn;
    g_postfix_recognizer_ud = userdata;
}

int tcc_ext_run_type_position_recognizer(TCCState* s) {
    if (g_type_recognizer) return g_type_recognizer(g_type_recognizer_ud, s);
    return 0;
}

int tcc_ext_run_postfix_recognizer(TCCState* s) {
    if (g_postfix_recognizer) return g_postfix_recognizer(g_postfix_recognizer_ud, s);
    return 0;
}

size_t tcc_ext_find_char_top_level(const char* src, size_t len, size_t start, char ch) {
    return cc_find_char_top_level(src, start, len, ch);
}

size_t tcc_ext_find_ident_top_level(const char* src, size_t len, size_t start, const char* ident) {
    size_t ilen = ident ? strlen(ident) : 0;
    return cc_find_ident_top_level(src, start, len, ident, ilen);
}

size_t tcc_ext_find_substr_top_level(const char* src, size_t len, size_t start, const char* needle) {
    size_t nlen = needle ? strlen(needle) : 0;
    return cc_find_substr_top_level(src, start, len, needle, nlen);
}

void tcc_ext_emit_diag(const char* msg, int line, int col, const char* file) {
    if (g_diag_fn) {
        g_diag_fn(g_diag_ud, msg, line, col, file);
        return;
    }
    cc_diag_translate_tcc_error(msg, line, col, file, NULL, NULL);
}
