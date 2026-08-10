#ifndef _STDARG_H
#define _STDARG_H

/*
 * TCC-oriented stdarg shim for pigz_cc (included before system headers in
 * pigz_cc.cch). Use function-like macros matching host/Clang/GCC so when the
 * lowerer harvests these `#define`s into emit.c they are identical to the
 * system definitions and do not trigger -Wmacro-redefined.
 */
typedef __builtin_va_list va_list;
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_copy(dest, src) __builtin_va_copy(dest, src)
#define va_end(ap) __builtin_va_end(ap)

/* fix a buggy dependency on GCC in libio.h */
typedef va_list __gnuc_va_list;
#define _VA_LIST_DEFINED

#endif /* _STDARG_H */
