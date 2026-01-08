/*
 * Generic Result helpers for generated C.
 *
 * Pattern:
 *   CC_DECL_RESULT(CCResultIntErr, int, CCIoError)
 *   CCResultIntErr r = cc_ok_CCResultIntErr(123);
 *   if (r.is_err) { ... r.err ... } else { ... r.ok ... }
 *
 * The union layout is stable and POD-only to keep ABI simple.
 */
#ifndef CC_RESULT_H
#define CC_RESULT_H

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

// Define a Result-like struct and constructors.
#define CC_DECL_RESULT(name, OkType, ErrType)                                       \
    typedef struct {                                                                \
        bool is_err;                                                                \
        union {                                                                     \
            OkType ok;                                                              \
            ErrType err;                                                            \
        };                                                                          \
    } name;                                                                         \
    static inline name cc_ok_##name(OkType value) {                                 \
        name r;                                                                     \
        r.is_err = false;                                                           \
        r.ok = value;                                                               \
        return r;                                                                   \
    }                                                                               \
    static inline name cc_err_##name(ErrType error) {                               \
        name r;                                                                     \
        r.is_err = true;                                                            \
        r.err = error;                                                              \
        return r;                                                                   \
    }

#endif // CC_RESULT_H


