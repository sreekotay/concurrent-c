typedef struct S {
#ifdef \
_WIN32
    void* handle;
#else
    int posix_pid;
#endif
} S;
