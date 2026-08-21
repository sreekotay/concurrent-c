typedef struct ElifFields {
#ifdef _WIN32
    void* handle;
#elif defined(__linux__)
    int linux_fd;
#else
    int posix_pid;
#endif
    int stdin_fd;
} ElifFields;
