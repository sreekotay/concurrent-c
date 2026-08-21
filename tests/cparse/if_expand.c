#define FOO 0
#define N 2
typedef struct IfExp {
#if FOO
    int foo_live;
#else
    int foo_dead;
#endif
#if N == 2
    int n_ok;
#endif
#if defined(FOO)
    int foo_defined;
#endif
#if BAR
    int bar_live;
#endif
    int after;
} IfExp;
