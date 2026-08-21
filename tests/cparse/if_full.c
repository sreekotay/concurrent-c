typedef struct FullIf {
#if 1 ? 1 : 0
    int ternary_live;
#endif
#if 0 ? 1 / 0 : 1
    int ternary_else;
#endif
#if (1 + 2) * 3 == 9 && (8 >> 2) == 2 && (3 & 1) == 1 && (~0 != 0)
    int arith_live;
#endif
#if defined(__has_feature) && __has_feature(thread_sanitizer)
    int tsan;
#else
    int no_tsan;
#endif
#if __has_builtin(__builtin_add_overflow)
    int has_addovf;
#endif
#if __has_include("if_full_inc.h")
    int has_quote;
#endif
#if __has_include("if_full_missing.h")
    int missing;
#endif
    int after;
} FullIf;
