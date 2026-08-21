#define T void *
#ifdef _WIN32
T handle;
#else
int posix_pid;
#endif
#define JOIN(a, b) a##b
int JOIN(pos, ix);
#define STR(x) #x
const char *s = STR(hello);
#define WRAP(...) f(__VA_ARGS__)
WRAP(1, 2);
#define E E
E;
#ifndef CC_M
#define CC_M 1
int once;
#endif
