/*
 * Concurrent-C Process Runtime
 *
 * Cross-platform: POSIX (macOS, Linux, BSD) and Windows.
 */

#include <ccc/std/process.cch>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Use cc_io_from_errno() from cc_io.cch for error conversion. */

/* ============================================================================
 * Process Spawning - POSIX
 * ============================================================================ */

#ifndef _WIN32

CCResultProcessIoError cc_process_spawn(const CCProcessConfig* config) {
    CCProcess proc = {.pid = -1, .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1};

    if (!config || !config->program || !config->args) {
        return cc_err_CCResultProcessIoError(cc_io_from_errno(EINVAL));
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};

    /* Create pipes as needed */
    if (config->pipe_stdin) {
        if (pipe(stdin_pipe) < 0) {
            return cc_err_CCResultProcessIoError(cc_io_from_errno(errno));
        }
    }
    if (config->pipe_stdout) {
        if (pipe(stdout_pipe) < 0) {
            if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
            return cc_err_CCResultProcessIoError(cc_io_from_errno(errno));
        }
    }
    if (config->pipe_stderr && !config->merge_stderr) {
        if (pipe(stderr_pipe) < 0) {
            if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
            if (stdout_pipe[0] >= 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
            return cc_err_CCResultProcessIoError(cc_io_from_errno(errno));
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
        if (stdout_pipe[0] >= 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
        if (stderr_pipe[0] >= 0) { close(stderr_pipe[0]); close(stderr_pipe[1]); }
        return cc_err_CCResultProcessIoError(cc_io_from_errno(err));
    }

    if (pid == 0) {
        /* Child process */

        /* Set up stdin */
        if (config->pipe_stdin) {
            close(stdin_pipe[1]);  /* Close write end */
            dup2(stdin_pipe[0], STDIN_FILENO);
            close(stdin_pipe[0]);
        }

        /* Set up stdout */
        if (config->pipe_stdout) {
            close(stdout_pipe[0]);  /* Close read end */
            dup2(stdout_pipe[1], STDOUT_FILENO);
            close(stdout_pipe[1]);
        }

        /* Set up stderr */
        if (config->merge_stderr && config->pipe_stdout) {
            dup2(STDOUT_FILENO, STDERR_FILENO);
        } else if (config->pipe_stderr) {
            close(stderr_pipe[0]);  /* Close read end */
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stderr_pipe[1]);
        }

        /* Change directory if specified */
        if (config->cwd) {
            if (chdir(config->cwd) < 0) {
                _exit(127);
            }
        }

        /* Execute */
        if (config->env) {
            execve(config->program, (char* const*)config->args, (char* const*)config->env);
        } else {
            execvp(config->program, (char* const*)config->args);
        }

        /* If we get here, exec failed */
        _exit(127);
    }

    /* Parent process */
    proc.pid = pid;

    if (config->pipe_stdin) {
        close(stdin_pipe[0]);  /* Close read end */
        proc.stdin_fd = stdin_pipe[1];
    }
    if (config->pipe_stdout) {
        close(stdout_pipe[1]);  /* Close write end */
        proc.stdout_fd = stdout_pipe[0];
    }
    if (config->pipe_stderr && !config->merge_stderr) {
        close(stderr_pipe[1]);  /* Close write end */
        proc.stderr_fd = stderr_pipe[0];
    }

    return cc_ok_CCResultProcessIoError(proc);
}

CCResultStatusIoError cc_process_wait(CCProcess* proc) {
    CCProcessStatus status = {0};

    if (!proc || proc->pid <= 0) {
        return cc_err_CCResultStatusIoError(cc_io_from_errno(EINVAL));
    }

    int wstatus;
    if (waitpid(proc->pid, &wstatus, 0) < 0) {
        return cc_err_CCResultStatusIoError(cc_io_from_errno(errno));
    }

    if (WIFEXITED(wstatus)) {
        status.exited = true;
        status.exit_code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        status.signaled = true;
        status.exit_code = WTERMSIG(wstatus);
    }

    proc->pid = -1;  /* Mark as completed */
    return cc_ok_CCResultStatusIoError(status);
}

CCResultStatusIoError cc_process_try_wait(CCProcess* proc) {
    CCProcessStatus status = {0};

    if (!proc || proc->pid <= 0) {
        return cc_err_CCResultStatusIoError(cc_io_from_errno(EINVAL));
    }

    int wstatus;
    pid_t result = waitpid(proc->pid, &wstatus, WNOHANG);

    if (result < 0) {
        return cc_err_CCResultStatusIoError(cc_io_from_errno(errno));
    }

    if (result == 0) {
        /* Process still running */
        CCIoError busy = {.kind = CC_IO_BUSY, .os_code = 0};
        return cc_err_CCResultStatusIoError(busy);
    }

    if (WIFEXITED(wstatus)) {
        status.exited = true;
        status.exit_code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        status.signaled = true;
        status.exit_code = WTERMSIG(wstatus);
    }

    proc->pid = -1;
    return cc_ok_CCResultStatusIoError(status);
}

CCResultBoolIoError cc_process_kill(CCProcess* proc, int sig) {
    if (!proc || proc->pid <= 0) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }

    if (kill(proc->pid, sig) < 0) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(errno));
    }

    return cc_ok_CCResultBoolIoError(true);
}

#else /* _WIN32 */

/* ============================================================================
 * Process Spawning - Windows
 * ============================================================================ */

CCResultProcessIoError cc_process_spawn(const CCProcessConfig* config) {
    CCProcess proc = {.handle = NULL, .pid = 0, .stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1};

    if (!config || !config->program || !config->args) {
        return cc_err_CCResultProcessIoError(cc_io_from_errno(EINVAL));
    }

    HANDLE stdin_read = NULL, stdin_write = NULL;
    HANDLE stdout_read = NULL, stdout_write = NULL;
    HANDLE stderr_read = NULL, stderr_write = NULL;

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};

    /* Create pipes */
    if (config->pipe_stdin) {
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
            CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
            return cc_err_CCResultProcessIoError(e);
        }
        SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    }
    if (config->pipe_stdout) {
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
            if (stdin_read) CloseHandle(stdin_read);
            if (stdin_write) CloseHandle(stdin_write);
            CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
            return cc_err_CCResultProcessIoError(e);
        }
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    }
    if (config->pipe_stderr && !config->merge_stderr) {
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) {
            if (stdin_read) CloseHandle(stdin_read);
            if (stdin_write) CloseHandle(stdin_write);
            if (stdout_read) CloseHandle(stdout_read);
            if (stdout_write) CloseHandle(stdout_write);
            CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
            return cc_err_CCResultProcessIoError(e);
        }
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    }

    /* Build command line */
    char cmdline[32768];
    size_t off = 0;
    for (size_t i = 0; config->args[i]; i++) {
        if (i > 0) cmdline[off++] = ' ';
        /* Simple quoting: wrap in quotes if contains space */
        const char* arg = config->args[i];
        int needs_quote = strchr(arg, ' ') != NULL;
        if (needs_quote) cmdline[off++] = '"';
        size_t len = strlen(arg);
        memcpy(cmdline + off, arg, len);
        off += len;
        if (needs_quote) cmdline[off++] = '"';
    }
    cmdline[off] = '\0';

    STARTUPINFOA si = {sizeof(STARTUPINFOA)};
    PROCESS_INFORMATION pi = {0};

    if (config->pipe_stdin || config->pipe_stdout || config->pipe_stderr) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = stdin_read ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = stdout_write ? stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
        if (config->merge_stderr) {
            si.hStdError = stdout_write ? stdout_write : GetStdHandle(STD_ERROR_HANDLE);
        } else {
            si.hStdError = stderr_write ? stderr_write : GetStdHandle(STD_ERROR_HANDLE);
        }
    }

    BOOL success = CreateProcessA(
        NULL,           /* Use command line */
        cmdline,        /* Command line */
        NULL,           /* Process security */
        NULL,           /* Thread security */
        TRUE,           /* Inherit handles */
        0,              /* Creation flags */
        (LPVOID)config->env,  /* Environment */
        config->cwd,    /* Working directory */
        &si,
        &pi
    );

    /* Close child-side handles */
    if (stdin_read) CloseHandle(stdin_read);
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);

    if (!success) {
        if (stdin_write) CloseHandle(stdin_write);
        if (stdout_read) CloseHandle(stdout_read);
        if (stderr_read) CloseHandle(stderr_read);
        CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
        return cc_err_CCResultProcessIoError(e);
    }

    CloseHandle(pi.hThread);

    proc.handle = pi.hProcess;
    proc.pid = pi.dwProcessId;
    proc.stdin_fd = stdin_write ? _open_osfhandle((intptr_t)stdin_write, 0) : -1;
    proc.stdout_fd = stdout_read ? _open_osfhandle((intptr_t)stdout_read, 0) : -1;
    proc.stderr_fd = stderr_read ? _open_osfhandle((intptr_t)stderr_read, 0) : -1;

    return cc_ok_CCResultProcessIoError(proc);
}

CCResultStatusIoError cc_process_wait(CCProcess* proc) {
    CCProcessStatus status = {0};

    if (!proc || !proc->handle) {
        return cc_err_CCResultStatusIoError(cc_io_from_errno(EINVAL));
    }

    WaitForSingleObject(proc->handle, INFINITE);

    DWORD exit_code;
    if (GetExitCodeProcess(proc->handle, &exit_code)) {
        status.exited = true;
        status.exit_code = (int)exit_code;
    }

    CloseHandle(proc->handle);
    proc->handle = NULL;
    proc->pid = 0;

    return cc_ok_CCResultStatusIoError(status);
}

CCResultStatusIoError cc_process_try_wait(CCProcess* proc) {
    CCProcessStatus status = {0};

    if (!proc || !proc->handle) {
        return cc_err_CCResultStatusIoError(cc_io_from_errno(EINVAL));
    }

    DWORD result = WaitForSingleObject(proc->handle, 0);
    if (result == WAIT_TIMEOUT) {
        CCIoError busy = {.kind = CC_IO_BUSY, .os_code = 0};
        return cc_err_CCResultStatusIoError(busy);
    }

    DWORD exit_code;
    if (GetExitCodeProcess(proc->handle, &exit_code)) {
        status.exited = true;
        status.exit_code = (int)exit_code;
    }

    CloseHandle(proc->handle);
    proc->handle = NULL;
    proc->pid = 0;

    return cc_ok_CCResultStatusIoError(status);
}

CCResultBoolIoError cc_process_kill(CCProcess* proc, int sig) {
    if (!proc || !proc->handle) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }

    /* On Windows, treat SIGKILL/SIGTERM as TerminateProcess */
    (void)sig;
    if (!TerminateProcess(proc->handle, 1)) {
        CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
        return cc_err_CCResultBoolIoError(e);
    }

    return cc_ok_CCResultBoolIoError(true);
}

#endif /* _WIN32 */

/* ============================================================================
 * Process I/O (shared implementation)
 * ============================================================================ */

CCResultSizeIoError cc_process_write(CCProcess* proc, CCSlice data) {
    if (!proc || proc->stdin_fd < 0) {
        return cc_err_CCResultSizeIoError(cc_io_from_errno(EINVAL));
    }

#ifdef _WIN32
    DWORD written;
    HANDLE h = (HANDLE)_get_osfhandle(proc->stdin_fd);
    if (!WriteFile(h, data.ptr, (DWORD)data.len, &written, NULL)) {
        CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
        return cc_err_CCResultSizeIoError(e);
    }
    return cc_ok_CCResultSizeIoError((size_t)written);
#else
    ssize_t n = write(proc->stdin_fd, data.ptr, data.len);
    if (n < 0) {
        return cc_err_CCResultSizeIoError(cc_io_from_errno(errno));
    }
    return cc_ok_CCResultSizeIoError((size_t)n);
#endif
}

CCResultSliceIoError cc_process_read(CCProcess* proc, CCArena* arena, size_t max_bytes) {
    CCSlice result = {0};

    if (!proc || proc->stdout_fd < 0 || !arena) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(EINVAL));
    }

    char* buf = cc_arena_alloc(arena, max_bytes, 1);
    if (!buf) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(ENOMEM));
    }

#ifdef _WIN32
    DWORD n;
    HANDLE h = (HANDLE)_get_osfhandle(proc->stdout_fd);
    if (!ReadFile(h, buf, (DWORD)max_bytes, &n, NULL)) {
        CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
        return cc_err_CCResultSliceIoError(e);
    }
#else
    ssize_t n = read(proc->stdout_fd, buf, max_bytes);
    if (n < 0) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(errno));
    }
#endif

    result.ptr = buf;
    result.len = (size_t)n;
    return cc_ok_CCResultSliceIoError(result);
}

CCResultSliceIoError cc_process_read_stderr(CCProcess* proc, CCArena* arena, size_t max_bytes) {
    CCSlice result = {0};

    if (!proc || proc->stderr_fd < 0 || !arena) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(EINVAL));
    }

    char* buf = cc_arena_alloc(arena, max_bytes, 1);
    if (!buf) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(ENOMEM));
    }

#ifdef _WIN32
    DWORD n;
    HANDLE h = (HANDLE)_get_osfhandle(proc->stderr_fd);
    if (!ReadFile(h, buf, (DWORD)max_bytes, &n, NULL)) {
        CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
        return cc_err_CCResultSliceIoError(e);
    }
#else
    ssize_t n = read(proc->stderr_fd, buf, max_bytes);
    if (n < 0) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(errno));
    }
#endif

    result.ptr = buf;
    result.len = (size_t)n;
    return cc_ok_CCResultSliceIoError(result);
}

void cc_process_close_stdin(CCProcess* proc) {
    if (proc && proc->stdin_fd >= 0) {
#ifdef _WIN32
        _close(proc->stdin_fd);
#else
        close(proc->stdin_fd);
#endif
        proc->stdin_fd = -1;
    }
}

CCResultSliceIoError cc_process_read_all(CCProcess* proc, CCArena* arena) {
    if (!proc || proc->stdout_fd < 0 || !arena) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(EINVAL));
    }

    /* Read in chunks and accumulate */
    size_t total_cap = 4096;
    size_t total_len = 0;
    char* total = cc_arena_alloc(arena, total_cap, 1);
    if (!total) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(ENOMEM));
    }

    while (1) {
        char buf[4096];
#ifdef _WIN32
        DWORD n;
        HANDLE h = (HANDLE)_get_osfhandle(proc->stdout_fd);
        if (!ReadFile(h, buf, sizeof(buf), &n, NULL)) {
            break;
        }
        if (n == 0) break;
#else
        ssize_t n = read(proc->stdout_fd, buf, sizeof(buf));
        if (n <= 0) break;
#endif

        /* Grow buffer if needed */
        while (total_len + (size_t)n > total_cap) {
            size_t new_cap = total_cap * 2;
            char* new_buf = cc_arena_alloc(arena, new_cap, 1);
            if (!new_buf) {
                CCSlice result = {.ptr = total, .len = total_len};
                return cc_ok_CCResultSliceIoError(result);
            }
            memcpy(new_buf, total, total_len);
            total = new_buf;
            total_cap = new_cap;
        }

        memcpy(total + total_len, buf, (size_t)n);
        total_len += (size_t)n;
    }

    CCSlice result = {.ptr = total, .len = total_len};
    return cc_ok_CCResultSliceIoError(result);
}

CCResultSliceIoError cc_process_read_all_stderr(CCProcess* proc, CCArena* arena) {
    if (!proc || proc->stderr_fd < 0 || !arena) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(EINVAL));
    }

    size_t total_cap = 4096;
    size_t total_len = 0;
    char* total = cc_arena_alloc(arena, total_cap, 1);
    if (!total) {
        return cc_err_CCResultSliceIoError(cc_io_from_errno(ENOMEM));
    }

    while (1) {
        char buf[4096];
#ifdef _WIN32
        DWORD n;
        HANDLE h = (HANDLE)_get_osfhandle(proc->stderr_fd);
        if (!ReadFile(h, buf, sizeof(buf), &n, NULL)) {
            break;
        }
        if (n == 0) break;
#else
        ssize_t n = read(proc->stderr_fd, buf, sizeof(buf));
        if (n <= 0) break;
#endif

        while (total_len + (size_t)n > total_cap) {
            size_t new_cap = total_cap * 2;
            char* new_buf = cc_arena_alloc(arena, new_cap, 1);
            if (!new_buf) {
                CCSlice result = {.ptr = total, .len = total_len};
                return cc_ok_CCResultSliceIoError(result);
            }
            memcpy(new_buf, total, total_len);
            total = new_buf;
            total_cap = new_cap;
        }

        memcpy(total + total_len, buf, (size_t)n);
        total_len += (size_t)n;
    }

    CCSlice result = {.ptr = total, .len = total_len};
    return cc_ok_CCResultSliceIoError(result);
}

/* ============================================================================
 * Convenience: Spawn Simple/Shell
 * ============================================================================ */

CCResultProcessIoError cc_process_spawn_simple(const char* program, const char** args) {
    CCProcessConfig config = {
        .program = program,
        .args = args,
        .env = NULL,
        .cwd = NULL,
        .pipe_stdin = false,
        .pipe_stdout = false,
        .pipe_stderr = false,
        .merge_stderr = false
    };
    return cc_process_spawn(&config);
}

CCResultProcessIoError cc_process_spawn_shell(const char* command) {
#ifdef _WIN32
    const char* args[] = {"cmd", "/c", command, NULL};
    CCProcessConfig config = {
        .program = "cmd",
        .args = args,
        .pipe_stdout = true,
        .pipe_stderr = true
    };
#else
    const char* args[] = {"/bin/sh", "-c", command, NULL};
    CCProcessConfig config = {
        .program = "/bin/sh",
        .args = args,
        .pipe_stdout = true,
        .pipe_stderr = true
    };
#endif
    return cc_process_spawn(&config);
}

/* ============================================================================
 * Convenience: Run and Capture
 * ============================================================================ */

CCResultProcessOutputIoError cc_process_run(CCArena* arena, const char* program, const char** args) {
    CCProcessOutput output = {0};

    CCProcessConfig config = {
        .program = program,
        .args = args,
        .pipe_stdout = true,
        .pipe_stderr = true
    };

    CCResultProcessIoError spawn_res = cc_process_spawn(&config);
    if (spawn_res.is_err) {
        return cc_err_CCResultProcessOutputIoError(spawn_res.err);
    }

    CCProcess proc = spawn_res.ok;

    /* Read stdout */
    CCResultSliceIoError stdout_res = cc_process_read_all(&proc, arena);
    if (!stdout_res.is_err) {
        output.stdout_data = stdout_res.ok;
    }

    /* Read stderr */
    CCResultSliceIoError stderr_res = cc_process_read_all_stderr(&proc, arena);
    if (!stderr_res.is_err) {
        output.stderr_data = stderr_res.ok;
    }

    /* Wait for exit */
    CCResultStatusIoError wait_res = cc_process_wait(&proc);
    if (!wait_res.is_err) {
        output.status = wait_res.ok;
    }

    return cc_ok_CCResultProcessOutputIoError(output);
}

CCResultProcessOutputIoError cc_process_run_shell(CCArena* arena, const char* command) {
#ifdef _WIN32
    const char* args[] = {"cmd", "/c", command, NULL};
    return cc_process_run(arena, "cmd", args);
#else
    const char* args[] = {"/bin/sh", "-c", command, NULL};
    return cc_process_run(arena, "/bin/sh", args);
#endif
}

/* ============================================================================
 * Environment
 * ============================================================================ */

CCSlice cc_env_get(CCArena* arena, const char* name) {
    CCSlice result = {0};
    if (!arena || !name) return result;

    const char* value = getenv(name);
    if (!value) return result;

    size_t len = strlen(value);
    char* copy = cc_arena_alloc(arena, len + 1, 1);
    if (!copy) return result;
    memcpy(copy, value, len + 1);

    result.ptr = copy;
    result.len = len;
    return result;
}

CCResultBoolIoError cc_env_set(const char* name, const char* value) {
    if (!name) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }

#ifdef _WIN32
    if (!SetEnvironmentVariableA(name, value)) {
        CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
        return cc_err_CCResultBoolIoError(e);
    }
#else
    if (setenv(name, value ? value : "", 1) < 0) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(errno));
    }
#endif

    return cc_ok_CCResultBoolIoError(true);
}

CCResultBoolIoError cc_env_unset(const char* name) {
    if (!name) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(EINVAL));
    }

#ifdef _WIN32
    if (!SetEnvironmentVariableA(name, NULL)) {
        CCIoError e = {.kind = CC_IO_OTHER, .os_code = (int)GetLastError()};
        return cc_err_CCResultBoolIoError(e);
    }
#else
    if (unsetenv(name) < 0) {
        return cc_err_CCResultBoolIoError(cc_io_from_errno(errno));
    }
#endif

    return cc_ok_CCResultBoolIoError(true);
}
