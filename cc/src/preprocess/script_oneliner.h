#ifndef CC_SCRIPT_ONELINER_H
#define CC_SCRIPT_ONELINER_H

#include <stddef.h>

/*
 * -e / -E helpers: token-gated predecls, expr-print / line-loop desugar,
 * toolbox @task emission. Result is an ordinary .shcc unit.
 */

typedef struct {
    int want_a;
    int want_io;
    int want_in;
    int want_args;
    int want_line;
    int want_nr;
} CCScriptOnelinerPredecls;

typedef struct {
    int expr_print; /* -E */
    int line_loop;  /* -n or -p */
    int line_print; /* -p */
} CCScriptOnelinerOpts;

void cc_script_oneliner_scan_predecls(const char* src, size_t len,
                                      CCScriptOnelinerPredecls* out);

char* cc_script_oneliner_lower(const char* src, size_t len,
                               const CCScriptOnelinerOpts* opts,
                               size_t* out_len);

char* cc_script_oneliner_inject_predecls(const char* src, size_t len,
                                         size_t* out_len);

char* cc_script_oneliner_first_line(const char* src, size_t len);

int cc_script_oneliner_is_ident(const char* name);

int cc_script_oneliner_task_exists(const char* toolbox, size_t len,
                                   const char* name);

char* cc_script_oneliner_format_task(const char* name, const char* doc,
                                     const char* program, size_t program_len,
                                     const CCScriptOnelinerOpts* opts,
                                     size_t* out_len);

#endif /* CC_SCRIPT_ONELINER_H */
