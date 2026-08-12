#ifndef CC_UNIT_HEADER_H
#define CC_UNIT_HEADER_H

#include <stddef.h>

/*
 * First-line translation-unit header. Kind is not an extension property:
 * the line names ccs / cch / shcc, and emit replaces it with the generated
 * C/H banner. Scripts use an OS shebang so the kernel can exec them.
 *
 *   #!ccc ccs [version=MAJOR[.MINOR[.PATCH[-SEED]]]]
 *   #!ccc cch [version=…]
 *   #!/usr/bin/env -S ./cc/bin/ccc [--as=shcc] [version=…]
 */

typedef enum {
    CC_UNIT_KIND_UNKNOWN = 0,
    CC_UNIT_KIND_CCS = 1,
    CC_UNIT_KIND_CCH = 2,
    CC_UNIT_KIND_SHCC = 3
} CCUnitKind;

typedef struct {
    CCUnitKind kind;   /* UNKNOWN if the line is not a unit header */
    int is_os_shebang; /* 1: kernel shebang (script); 0: #!ccc magic */
    char version[64];  /* empty if unpinned */
    int ill_formed;    /* 1: looked like a header but failed to parse */
    char err[192];
} CCUnitHeader;

const char* cc_unit_kind_name(CCUnitKind k);
CCUnitKind cc_unit_kind_from_name(const char* s);
CCUnitKind cc_unit_kind_from_ext(const char* path);

/* Parse one line (no requirement that it include the trailing newline). */
int cc_unit_header_parse_line(const char* line, size_t n, CCUnitHeader* out);

/* Read the first line of path. Returns 0 even when there is no header
 * (kind UNKNOWN). Returns -1 only on I/O failure. */
int cc_unit_header_from_file(const char* path, CCUnitHeader* out);

/* Byte offset after a recognized unit header's first line; 0 if none. */
size_t cc_unit_header_skip(const char* src, size_t n);

/* Combine CLI --as / version= with the file header and path suffix.
 * Disagreement is an error. Missing header falls back to the suffix.
 * Returns 0 on success. */
int cc_unit_resolve(const char* path, CCUnitKind cli_kind,
                    const char* cli_version, CCUnitKind* kind_out,
                    char version_out[64], char* err, size_t err_cap);

/* Pin form: MAJOR[.MINOR[.PATCH[-SEED]]]  (e.g. 0.3.2 or 0.3.2-121).
 * Omitted trailing components are -1. */
int cc_ccc_version_parse(const char* s, int* major, int* minor, int* patch,
                         int* seed);
void cc_ccc_version_current(char* dst, size_t cap);
int cc_ccc_version_current_seed(void);
int cc_ccc_version_equal(const char* a, const char* b);
/* 1 if pin is a component prefix of candidate (`0.3.2` matches `0.3.2-121`;
 * `0.3.2-12` does not). */
int cc_ccc_version_matches(const char* pin, const char* candidate);
/* <0 / 0 / >0 like strcmp. Unspecified components sort below specified. */
int cc_ccc_version_cmp(const char* a, const char* b);

/* 1 if arg is `--as=KIND` or `--as` (caller then reads the next token). */
int cc_unit_cli_is_as(const char* arg);
int cc_unit_cli_parse_as_value(const char* val, CCUnitKind* out, char* err,
                               size_t err_cap);

/* 1 if arg is `version=PIN` or `--ccc-version=PIN` or `--ccc-version`. */
int cc_unit_cli_is_version(const char* arg);
int cc_unit_cli_parse_version_value(const char* val, char* pin, size_t cap,
                                    char* err, size_t err_cap);

#endif /* CC_UNIT_HEADER_H */
