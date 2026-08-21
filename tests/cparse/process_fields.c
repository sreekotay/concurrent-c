#ifndef CC_STD_PROCESS_H
#define CC_STD_PROCESS_H

typedef struct CCProcess {
#ifdef _WIN32
    void* handle;
    uint32_t pid;
#else
    int posix_pid;
#endif
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
} CCProcess;

#endif
