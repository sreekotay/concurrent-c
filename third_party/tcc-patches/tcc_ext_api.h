#ifndef TCC_EXT_API_H
#define TCC_EXT_API_H

/* Versioned hermetic boundary between Concurrent-C and the patched TCC fork (M5). */
#define TCC_EXT_API_VERSION_MAJOR 1
#define TCC_EXT_API_VERSION_MINOR 0

#include <stddef.h>

typedef struct TCCState TCCState;
typedef struct Token Token;

typedef void (*TCCExtDiagFn)(void* userdata,
                             const char* msg,
                             int line,
                             int col,
                             const char* file);

typedef int (*TCCExtTypePositionRecognizerFn)(void* userdata, TCCState* s);
typedef int (*TCCExtPostfixRecognizerFn)(void* userdata, TCCState* s);

void tcc_ext_api_get_version(int* major, int* minor);

TCCState* tcc_ext_get_state(void);

int tcc_ext_current_token(void);
const char* tcc_ext_current_token_str(void);
int tcc_ext_current_line(void);
int tcc_ext_current_col(void);

void tcc_ext_set_diag_callback(TCCState* s, TCCExtDiagFn fn, void* userdata);
void tcc_ext_set_type_position_recognizer(TCCState* s, TCCExtTypePositionRecognizerFn fn, void* userdata);
void tcc_ext_set_postfix_recognizer(TCCState* s, TCCExtPostfixRecognizerFn fn, void* userdata);

/* Comment-aware top-level search (F6/F11/F12 family) */
size_t tcc_ext_find_char_top_level(const char* src, size_t len, size_t start, char ch);
size_t tcc_ext_find_ident_top_level(const char* src, size_t len, size_t start, const char* ident);
size_t tcc_ext_find_substr_top_level(const char* src, size_t len, size_t start, const char* needle);

#endif /* TCC_EXT_API_H */
