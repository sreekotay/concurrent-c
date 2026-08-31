/*
 * Compiler compatibility: stdbool, stdint, stddef.
 * Include this instead of duplicating the boilerplate.
 */
#pragma once
#ifndef CC_COMPAT_H
#define CC_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include(<stdbool.h>)
#include <stdbool.h>
#else
#ifndef __bool_true_false_are_defined
typedef int bool;
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif

/* Compile-time assertion usable where an expression is required, so a macro
 * that dispatches on type can reject an unsupported one AT THE CALL SITE.
 *
 * `why` is an identifier, not a string, and it is the whole diagnostic:
 *
 *     error: negative width in bit-field 'no_python_conversion_for_this_type'
 *     note:  in expansion of macro 'CC_PY_IN'          <- the caller's line
 *
 * A string message would read better, and `_Static_assert` takes one — but it
 * is a declaration, so reaching expression position needs a GNU statement
 * expression, and glibc replaces it with a message-losing compat macro
 * whenever the compiler does not advertise C11.  This form is C89: bitfields
 * and struct types inside `sizeof` both predate C99, and a negative width is a
 * constraint violation, so a diagnostic is required of every conforming
 * compiler.  One spelling, every target, message always survives.
 *
 * Pair it with a `default:` arm in the dispatch it guards: the assertion says
 * what is wrong, the arm's argument mismatch names the offending type. */
#define cc_static_assert(cond, why) \
    ((void)sizeof(struct { int why : (cond) ? 1 : -1; }))

/* Diverging helpers (`cc_error_exit`, …). Host C must see the same
 * noreturn the language already treats as a hard leave. */
#ifndef CC_NORETURN
#if defined(_MSC_VER)
#define CC_NORETURN __declspec(noreturn)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define CC_NORETURN _Noreturn
#elif defined(__GNUC__) || defined(__clang__) || defined(__TINYC__)
#define CC_NORETURN __attribute__((noreturn))
#else
#define CC_NORETURN
#endif
#endif

/* Compiler-emitted heap (closures, frames). Not malloc — that name stays
 * undeclared until the user includes <stdlib.h>. */
void *cc__heap_alloc(size_t n);
void *cc__heap_calloc(size_t n, size_t sz);
void cc__heap_free(void *p);

/* Generated zero/copy: `cc__bytes_zero` / `cc__bytes_copy` in cc_slice.cch.
 * Do not emit memset / memcpy / __builtin_memset — TinyCC has no builtins,
 * and default CCS TUs must not declare the libc names. */

#endif /* CC_COMPAT_H */
