#include "cc_async_backend_poll.cch"
#include "cc_async_backend.cch"
#include "cc_async_runtime.cch"
#include "std/io.cch"
#include "std/async_io.cch"
#include "cc_sched.cch"

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return errno;
    if (flags & O_NONBLOCK) return 0;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return errno;
    return 0;
}

static int wait_poll(int fd, short events, const CCDeadline* d) {
    struct pollfd p = {.fd = fd, .events = events};
    int timeout = -1;
    if (d && d->deadline.tv_sec) {
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        long ms = (d->deadline.tv_sec - now.tv_sec) * 1000L + (d->deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (ms <= 0) return ETIMEDOUT;
        timeout = (ms > INT_MAX) ? INT_MAX : (int)ms;
    }
    int r = poll(&p, 1, timeout);
    if (r == 0) return ETIMEDOUT;
    if (r < 0) return errno;
    if (p.revents & (POLLERR | POLLNVAL)) return EIO;
    return 0;
}

static int backend_read_all(void* ctx, CCFile *file, CCArena *arena, CCSlice* out, CCAsyncHandle* h, const CCDeadline* d) {
    (void)ctx;
    if (!file || !file->handle || !arena || !out || !h) return EINVAL;
    int fd = fileno(file->handle);
    if (fd < 0) return EBADF;
    int nb = set_nonblock(fd); if (nb != 0) return nb;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return ENOTSUP;
    size_t sz = (size_t)st.st_size;
    void* buf = cc_arena_alloc(arena, sz, 1);
    if (!buf) return ENOMEM;
    size_t off = 0;
    while (off < sz) {
        int w = wait_poll(fd, POLLIN, d);
        if (w != 0) return w;
        ssize_t n = read(fd, (char*)buf + off, sz - off);
        if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) continue; return errno; }
        if (n == 0) break;
        off += (size_t)n;
    }
    out->ptr = buf;
    out->len = off;
    out->id = 0; out->alen = off;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    int err = 0;
    cc_chan_send(h->done, &err, sizeof(int));
    return 0;
}

static int backend_read(void* ctx, CCFile *file, CCArena *arena, size_t n, CCFileReadResult* out, CCAsyncHandle* h, const CCDeadline* d) {
    (void)ctx;
    if (!file || !file->handle || !arena || !out || !h) return EINVAL;
    int fd = fileno(file->handle); if (fd < 0) return EBADF;
    int nb = set_nonblock(fd); if (nb != 0) return nb;
    void* buf = cc_arena_alloc(arena, n, 1);
    if (!buf) return ENOMEM;
    size_t off = 0;
    int eof = 0;
    while (off < n) {
        int w = wait_poll(fd, POLLIN, d);
        if (w != 0) { if (w == ETIMEDOUT) break; return w; }
        ssize_t r = read(fd, (char*)buf + off, n - off);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return errno;
        }
        if (r == 0) { eof = 1; break; }
        off += (size_t)r;
        if (cc_deadline_expired(d)) break;
    }
    out->data.ptr = buf; out->data.len = off; out->data.id = 0; out->data.alen = n;
    out->bytes_read = off; out->eof = eof;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    int err = 0;
    cc_chan_send(h->done, &err, sizeof(int));
    return 0;
}

static int backend_read_line(void* ctx, CCFile *file, CCArena *arena, CCFileReadResult* out, CCAsyncHandle* h, const CCDeadline* d) {
    (void)ctx;
    if (!file || !file->handle || !arena || !out || !h) return EINVAL;
    int fd = fileno(file->handle); if (fd < 0) return EBADF;
    int nb = set_nonblock(fd); if (nb != 0) return nb;
    size_t cap = 256;
    char* buf = (char*)cc_arena_alloc(arena, cap, 1);
    if (!buf) return ENOMEM;
    size_t len = 0;
    int eof = 0;
    for (;;) {
        int w = wait_poll(fd, POLLIN, d);
        if (w != 0) { if (w == ETIMEDOUT) break; return w; }
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return errno;
        }
        if (r == 0) { eof = 1; break; }
        if (len + 1 > cap) { return ENOMEM; }
        buf[len++] = c;
        if (c == '\n') break;
    }
    out->data.ptr = buf; out->data.len = len; out->data.id = 0; out->data.alen = cap;
    out->bytes_read = len; out->eof = eof && len == 0;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    int err = 0;
    cc_chan_send(h->done, &err, sizeof(int));
    return 0;
}

static int backend_write(void* ctx, CCFile *file, CCSlice data, size_t* out_written, CCAsyncHandle* h, const CCDeadline* d) {
    (void)ctx;
    if (!file || !file->handle || !h) return EINVAL;
    int fd = fileno(file->handle); if (fd < 0) return EBADF;
    int nb = set_nonblock(fd); if (nb != 0) return nb;
    size_t off = 0;
    while (off < data.len) {
        int w = wait_poll(fd, POLLOUT, d);
        if (w != 0) return w;
        ssize_t n = write(fd, (const char*)data.ptr + off, data.len - off);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return errno;
        }
        off += (size_t)n;
        if (cc_deadline_expired(d)) break;
    }
    if (out_written) *out_written = off;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    int err = 0;
    cc_chan_send(h->done, &err, sizeof(int));
    return 0;
}

static int backend_open(void* ctx, CCFile *file, const char *path, const char *mode, CCAsyncHandle* h, const CCDeadline* d) {
    (void)ctx; (void)d;
    if (!file || !path || !mode || !h) return EINVAL;
    FILE* f = fopen(path, mode);
    if (!f) return errno;
    int fd = fileno(f);
    if (fd >= 0) set_nonblock(fd);
    file->handle = f;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    int err = 0;
    cc_chan_send(h->done, &err, sizeof(int));
    return 0;
}

static int backend_close(void* ctx, CCFile *file, CCAsyncHandle* h, const CCDeadline* d) {
    (void)ctx; (void)d;
    if (!file || !file->handle || !h) return EINVAL;
    fclose(file->handle);
    file->handle = NULL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    int err = 0;
    cc_chan_send(h->done, &err, sizeof(int));
    return 0;
}

static const CCAsyncBackendOps g_poll_ops = {
    .open = backend_open,
    .close = backend_close,
    .read_all = backend_read_all,
    .read = backend_read,
    .read_line = backend_read_line,
    .write = backend_write,
};

int cc_async_backend_poll_register(void) {
    return cc_async_runtime_set_backend(&g_poll_ops, NULL, "poll");
}

