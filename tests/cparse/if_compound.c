typedef struct CompIf {
#if defined(_WIN32) || defined(_WIN64)
    void* handle;
#elif defined(__linux__) && !defined(__ANDROID__)
    int linux_fd;
#elif UINTPTR_MAX == 0xFFFFFFFF
    int ilp32;
#else
    int posix_pid;
#endif
    int stdin_fd;
} CompIf;
