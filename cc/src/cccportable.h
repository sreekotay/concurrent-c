#ifndef CC_CCCPORTABLE_H
#define CC_CCCPORTABLE_H

#include <stddef.h>

/* Consumer host-C snapshot (not a compiler sysroot). */

int cc_portable_install(const char* dir,
                        const char* lowered_include,
                        const char* runtime_c,
                        const char* repo_root,
                        const char* version_line,
                        char* err, size_t err_cap);

/* 0 = ok to print; -1 = missing tree or stamp mismatch. */
int cc_portable_check_tree(const char* dir, const char* version_line,
                           char* err, size_t err_cap);

void cc_portable_print_cflags(const char* dir);
void cc_portable_print_libs(const char* dir);

/* Scan argv for --cccportable DIR. Returns 1 if consumed, 0 if not, -1 on error.
 * *cli_set is set when the flag (not env) supplied the directory. */
int cc_take_cccportable_flag(int argc, char** argv, int* i,
                             const char** dir_out, int* cli_set);

/* File-start `#pragma(@prelude) off` / `#pragma(@linenumbers) off` /
 * `#pragma(@per_tu)` after the unit header. Returns 0, or -1 and fills
 * err on ill-formed operands. `per_tu` is presence (no `off`). */
int cc_file_start_pragmas(const char* src, size_t n,
                          int* prelude_off, int* linenumbers_off,
                          int* per_tu,
                          char* err, size_t err_cap);

/* Rewrite emitted .c in place: version comment always; strip libc banner
 * includes if prelude_off; strip #line / CC_LN if no_line. */
int cc_emit_polish_c(const char* path, const char* version,
                     int prelude_off, int no_line);

int cc_path_under_dir(const char* path, const char* dir);

#endif
