/*
 * Parse-Time Stubs for cccn
 *
 * This header provides minimal placeholder definitions that allow TCC to parse
 * Concurrent-C code. The real CC headers (cc_result.cch, cc_optional.cch) already
 * have their own CC_PARSER_MODE support with __CCResultGeneric/__CCOptionalGeneric.
 *
 * This file only defines things that aren't already in the real headers.
 *
 * These stubs are ONLY active when CC_PARSER_MODE is defined (set by TCC
 * during parsing).
 */
#ifndef CC_PARSE_STUBS_H
#define CC_PARSE_STUBS_H

#ifdef CC_PARSER_MODE

#include <stdint.h>

/* ============================================================================
 * Stub functions for constructs that aren't in the real CC headers
 * ============================================================================ */

/* These are just dummy functions to satisfy TCC during parsing.
 * The real implementations are in the CC runtime. */

static inline void __cc_parse_stub_void(void) {}
static inline int __cc_parse_stub_int(void) { return 0; }

/* cc_try stub - parse-time placeholder, real implementation in runtime */
#ifndef cc_try
#define cc_try(expr) ((void)(expr), 0)
#endif

#endif /* CC_PARSER_MODE */

#endif /* CC_PARSE_STUBS_H */
