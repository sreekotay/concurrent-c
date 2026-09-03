#include <ccc/cc_io_error.cch>
#include <ccc/cc_arena.cch>
#include <ccc/cc_slice.cch>
#include <ccc/std/io.cch>

#undef cc_file_read_all
#undef cc_file_read_all_async
#undef cc_file_read_async
#undef cc_file_read_line_async
#undef cc_file_read_into
#undef cc_file_read_line_into
#undef cc_path_join
#undef cc_path_dirname
#undef cc_path_basename
#undef cc_buf_writer_init
#undef cc_string_push_slice

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(__TINYC__)
CCIoError cc_io_from_errno(int err) {
    CCErrorKind kind = CC_ERR_IO;
    switch (err) {
        case EACCES: kind = CC_ERR_PERMISSION; break;
        case ENOENT: kind = CC_ERR_NOT_FOUND; break;
        case EINVAL: kind = CC_ERR_INVALID_ARG; break;
        case EINTR:  kind = CC_ERR_INTERRUPTED; break;
        case ENOMEM: kind = CC_ERR_OUT_OF_MEMORY; break;
        case EBUSY:  kind = CC_ERR_WOULD_BLOCK; break;
        case EPIPE:  kind = CC_ERR_CLOSED; break;
#if defined(EAGAIN)
        case EAGAIN: kind = CC_ERR_WOULD_BLOCK; break;
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || EWOULDBLOCK != EAGAIN)
        case EWOULDBLOCK: kind = CC_ERR_WOULD_BLOCK; break;
#endif
#if defined(ECANCELED)
        case ECANCELED: kind = CC_ERR_CANCELLED; break;
#endif
        case 0:      kind = CC_ERR_IO; break;
        default:     kind = CC_ERR_IO; break;
    }
    return cc_io_error_os(kind, err);
}
#endif

enum { CC__PATH_MAX = 4096 };

static int cc__file_fd(const CCFile *file) {
    return file ? file->fd : -1;
}

static int cc__path_c(CCSlice path, char *buf, size_t cap) {
    if (!path.ptr || path.len == 0 || path.len >= cap) return -1;
    memcpy(buf, path.ptr, path.len);
    buf[path.len] = '\0';
    return 0;
}

static CCResult_CCFile_CCIoError cc__file_open_flags(CCSlice path, int flags, int mode) {
    char pbuf[CC__PATH_MAX];
    int fd;
    CCFile f;
    if (cc__path_c(path, pbuf, sizeof(pbuf)) != 0) {
        return cc_err_CCResult_CCFile_CCIoError(cc_io_from_errno(EINVAL));
    }
    do {
        fd = open(pbuf, flags, mode);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return cc_err_CCResult_CCFile_CCIoError(cc_io_from_errno(errno));
    f.fd = fd;
    return cc_ok_CCResult_CCFile_CCIoError(f);
}

CCResult_CCFile_CCIoError cc_file_open(CCSlice path) {
    return cc__file_open_flags(path, O_RDONLY, 0);
}

CCResult_CCFile_CCIoError cc_file_create(CCSlice path) {
    return cc__file_open_flags(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

void cc_file_close(CCFile *file) {
    int fd = cc__file_fd(file);
    if (fd < 0) return;
    while (close(fd) != 0 && errno == EINTR) {}
    file->fd = -1;
}

static CCResult_size_t_CCIoError cc__write_all(int fd, const void *buf, size_t n) {
    size_t off = 0;
    const char *p = (const char *)buf;
    if (fd < 0) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    if (n == 0) return cc_ok_CCResult_size_t_CCIoError(0);
    if (!buf) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
        }
        if (w == 0)
            return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EIO));
        off += (size_t)w;
    }
    return cc_ok_CCResult_size_t_CCIoError(n);
}

CCResult_CCSlice_CCIoError cc_file_read_all(CCFile *file, CCArena arena) {
    int fd = cc__file_fd(file);
    struct stat st;
    size_t len;
    size_t off;
    char *buf;
    if (fd < 0 || !cc_arena_is_live(arena)) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(EINVAL));
    }
    if (fstat(fd, &st) != 0) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(ESPIPE));
    }
    len = (size_t)st.st_size;
    buf = (char *)cc_arena_alloc(arena, len + 1, sizeof(char));
    if (!buf) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_error_os(CC_IO_OUT_OF_MEMORY, ENOMEM));
    }
    off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    buf[off] = '\0';
    return cc_ok_CCResult_CCSlice_CCIoError(
        cc_slice_from_parts(buf, off, CC_SLICE_ID_UNTRACKED));
}

CCResult_size_t_CCIoError cc_file_write(CCFile *file, CCSlice data) {
    return cc__write_all(cc__file_fd(file), data.ptr, data.len);
}

CCResult_size_t_CCIoError cc_std_out_write(CCSlice data) {
    if (!data.ptr || data.len == 0) return cc_ok_CCResult_size_t_CCIoError(0);
    size_t written = fwrite(data.ptr, 1, data.len, stdout);
    if (written != data.len && ferror(stdout)) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    }
    return cc_ok_CCResult_size_t_CCIoError(written);
}

CCResult_size_t_CCIoError cc_std_err_write(CCSlice data) {
    if (!data.ptr || data.len == 0) return cc_ok_CCResult_size_t_CCIoError(0);
    size_t written = fwrite(data.ptr, 1, data.len, stderr);
    if (written != data.len && ferror(stderr)) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    }
    return cc_ok_CCResult_size_t_CCIoError(written);
}

CCResult_bool_CCIoError cc_file_read_into(CCFile *file, CCArena arena, size_t n,
                                         CCSlice *out) {
    int fd = cc__file_fd(file);
    char *buf;
    size_t got = 0;
    if (fd < 0 || !cc_arena_is_live(arena) || n == 0 || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }
    buf = (char *)cc_arena_alloc(arena, n, sizeof(char));
    if (!buf) {
        return cc_err_CCResult_bool_CCIoError(
            cc_io_error_os(CC_IO_OUT_OF_MEMORY, ENOMEM));
    }
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(errno));
        }
        if (r == 0) break;
        got += (size_t)r;
        break; /* one read(2); further bytes stay for the next call */
    }
    if (got == 0) {
        *out = cc_slice_empty();
        return cc_ok_CCResult_bool_CCIoError(false);
    }
    *out = cc_slice_from_parts(buf, got, CC_SLICE_ID_UNTRACKED);
    return cc_ok_CCResult_bool_CCIoError(true);
}

CCResult_bool_CCIoError cc_file_read_line_into(CCFile *file, CCArena arena,
                                              CCSlice *out) {
    int fd = cc__file_fd(file);
    CCString line;
    if (fd < 0 || !cc_arena_is_live(arena) || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }
    line = cc_string_new();
    for (;;) {
        unsigned char c;
        ssize_t n;
        do {
            n = read(fd, &c, 1);
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(errno));
        if (n == 0) {
            if (line.len == 0) {
                *out = cc_slice_empty();
                return cc_ok_CCResult_bool_CCIoError(false);
            }
            break;
        }
        if (c == '\n') break;
        if (!cc_string_push_slice(&line, cc_slice_from_buffer(&c, 1), arena)) {
            return cc_err_CCResult_bool_CCIoError(
                cc_io_error_os(CC_IO_OUT_OF_MEMORY, ENOMEM));
        }
    }
    {
        CCSlice cur = cc_string_as_slice(&line);
        if (cur.len > 0 && ((char *)cur.ptr)[cur.len - 1] == '\r')
            line.len--;
    }
    *out = cc_string_as_slice(&line);
    return cc_ok_CCResult_bool_CCIoError(true);
}

CCResult_bool_CCIoError cc_file_read_buf_into(CCFile *file, void *buf, size_t n,
                                             size_t *out) {
    int fd = cc__file_fd(file);
    ssize_t got;
    if (fd < 0 || !buf || n == 0 || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }
    do {
        got = read(fd, buf, n);
    } while (got < 0 && errno == EINTR);
    if (got < 0)
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(errno));
    if (got == 0) {
        *out = 0;
        return cc_ok_CCResult_bool_CCIoError(false);
    }
    *out = (size_t)got;
    return cc_ok_CCResult_bool_CCIoError(true);
}

CCResult_size_t_CCIoError cc_file_write_buf(CCFile *file, const void *buf,
                                           size_t n) {
    int fd = cc__file_fd(file);
    ssize_t written;
    if (fd < 0 || (n > 0 && !buf)) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    }
    if (n == 0) return cc_ok_CCResult_size_t_CCIoError(0);
    do {
        written = write(fd, buf, n);
    } while (written < 0 && errno == EINTR);
    if (written < 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    return cc_ok_CCResult_size_t_CCIoError((size_t)written);
}

CCResult_size_t_CCIoError cc_file_write_some(CCFile *file, CCSlice data) {
    return cc_file_write_buf(file, data.ptr, data.len);
}

CCResult_size_t_CCIoError cc_file_read_at(CCFile *file, void *buf, size_t n,
                                         int64_t offset) {
    int fd = cc__file_fd(file);
    ssize_t got;
    if (fd < 0 || offset < 0 || (n > 0 && !buf)) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    }
    if (n == 0) return cc_ok_CCResult_size_t_CCIoError(0);
    do {
        got = pread(fd, buf, n, (off_t)offset);
    } while (got < 0 && errno == EINTR);
    if (got < 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    return cc_ok_CCResult_size_t_CCIoError((size_t)got);
}

CCResult_size_t_CCIoError cc_file_write_at(CCFile *file, CCSlice data,
                                          int64_t offset) {
    int fd = cc__file_fd(file);
    size_t off = 0;
    const char *p;
    if (fd < 0 || offset < 0) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    }
    if (data.len == 0) return cc_ok_CCResult_size_t_CCIoError(0);
    if (!data.ptr)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    p = (const char *)data.ptr;
    while (off < data.len) {
        ssize_t w = pwrite(fd, p + off, data.len - off, (off_t)offset + (off_t)off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
        }
        if (w == 0)
            return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EIO));
        off += (size_t)w;
    }
    return cc_ok_CCResult_size_t_CCIoError(data.len);
}

CCResult_size_t_CCIoError cc_file_sync(CCFile *file) {
    int fd = cc__file_fd(file);
    if (fd < 0) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    if (fsync(fd) != 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    return cc_ok_CCResult_size_t_CCIoError(0);
}

CCResult_size_t_CCIoError cc_file_seek(CCFile *file, long offset, int whence) {
    int fd = cc__file_fd(file);
    if (fd < 0) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    if (lseek(fd, (off_t)offset, whence) < 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    return cc_ok_CCResult_size_t_CCIoError(0);
}

CCResult_size_t_CCIoError cc_file_tell(CCFile *file) {
    int fd = cc__file_fd(file);
    off_t pos;
    if (fd < 0) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    return cc_ok_CCResult_size_t_CCIoError((size_t)pos);
}

CCResult_size_t_CCIoError cc_file_size(CCFile *file) {
    int fd = cc__file_fd(file);
    struct stat st;
    if (fd < 0) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    if (fstat(fd, &st) != 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    if (!S_ISREG(st.st_mode) || st.st_size < 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(ESPIPE));
    return cc_ok_CCResult_size_t_CCIoError((size_t)st.st_size);
}

CCSlice cc_path_join(CCArena arena, CCSlice a, CCSlice b) {
    size_t need_sep, total, off;
    CCSlice out;
    char *buf;
    if (!cc_arena_is_live(arena)) return cc_slice_empty();
    need_sep = (a.len > 0 && ((char *)a.ptr)[a.len - 1] != cc_path_sep()) ? 1 : 0;
    total = a.len + need_sep + b.len;
    out = cc_arena_alloc_slice_bytes(arena, total + 1);
    buf = (char *)out.ptr;
    if (!buf) return cc_slice_empty();
    off = 0;
    if (a.len) {
        memcpy(buf + off, a.ptr, a.len);
        off += a.len;
    }
    if (need_sep) buf[off++] = cc_path_sep();
    if (b.len) {
        memcpy(buf + off, b.ptr, b.len);
        off += b.len;
    }
    buf[off] = '\0';
    out.len = off;
    return out;
}

CCSlice cc_path_dirname(CCArena arena, CCSlice path) {
    const char *p;
    size_t len, i;
    CCSlice out;
    char *buf;
    if (!cc_arena_is_live(arena) || !path.ptr || path.len == 0)
        return cc_slice_from_static(".", 1);
    p = (const char *)path.ptr;
    len = path.len;
    while (len > 0 && p[len - 1] == cc_path_sep()) len--;
    if (len == 0) return cc_slice_from_static("/", 1);
    i = len;
    while (i > 0 && p[i - 1] != cc_path_sep()) i--;
    if (i == 0) return cc_slice_from_static(".", 1);
    while (i > 1 && p[i - 1] == cc_path_sep()) i--;
    out = cc_arena_alloc_slice_bytes(arena, i + 1);
    buf = (char *)out.ptr;
    if (!buf) return cc_slice_empty();
    memcpy(buf, p, i);
    buf[i] = '\0';
    out.len = i;
    return out;
}

CCSlice cc_path_basename(CCArena arena, CCSlice path) {
    const char *p;
    size_t len, end, start, i, out_len;
    CCSlice out;
    char *buf;
    if (!cc_arena_is_live(arena) || !path.ptr || path.len == 0)
        return cc_slice_empty();
    p = (const char *)path.ptr;
    len = path.len;
    while (len > 0 && p[len - 1] == cc_path_sep()) len--;
    end = len;
    start = 0;
    for (i = len; i > 0; --i) {
        if (p[i - 1] == cc_path_sep()) {
            start = i;
            break;
        }
    }
    out_len = (end > start) ? (end - start) : 0;
    out = cc_arena_alloc_slice_bytes(arena, out_len + 1);
    buf = (char *)out.ptr;
    if (!buf) return cc_slice_empty();
    if (out_len) memcpy(buf, p + start, out_len);
    buf[out_len] = '\0';
    out.len = out_len;
    return out;
}

int cc_buf_writer_init(CCBufWriter *w, CCFile *f, CCArena arena, size_t cap) {
    if (!w || !f || !cc_arena_is_live(arena) || cap == 0) return -1;
    memset(w, 0, sizeof(*w));
    w->file = f;
    w->buf = (char *)cc_arena_alloc(arena, cap, sizeof(char));
    if (!w->buf) return -1;
    w->cap = cap;
    w->len = 0;
    return 0;
}

CCResult_size_t_CCIoError cc_buf_writer_flush(CCBufWriter *w) {
    CCSlice data;
    CCResult_size_t_CCIoError wr;
    if (!w || !w->file || w->file->fd < 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    if (w->len == 0) return cc_ok_CCResult_size_t_CCIoError(0);
    data = cc_slice_from_buffer(w->buf, w->len);
    wr = cc_file_write(w->file, data);
    if (wr.ok) w->len = 0;
    return wr;
}

CCResult_size_t_CCIoError cc_buf_writer_write(CCBufWriter *w, CCSlice data) {
    size_t off;
    if (!w || !w->buf || !w->file || w->file->fd < 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    off = 0;
    while (off < data.len) {
        size_t space = w->cap - w->len;
        size_t chunk;
        if (space == 0) {
            CCResult_size_t_CCIoError fl = cc_buf_writer_flush(w);
            if (!fl.ok) return fl;
            space = w->cap;
        }
        chunk = data.len - off;
        if (chunk > space) chunk = space;
        memcpy(w->buf + w->len, (char *)data.ptr + off, chunk);
        w->len += chunk;
        off += chunk;
    }
    return cc_ok_CCResult_size_t_CCIoError(data.len);
}

#ifdef CC_ENABLE_ASYNC
static int cc__async_complete(CCAsyncHandle* h, int err) {
    if (!h) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    return cc_chan_send(h->done, &err, sizeof(int));
}

int cc_file_open_async(CCExec* ex, CCFile *file, CCSlice path, CCAsyncHandle* h) {
    CCResult_CCFile_CCIoError r;
    (void)ex;
    r = cc_file_open(path);
    if (r.ok && file) *file = r.u.value;
    return cc__async_complete(h, r.ok ? 0 : -1);
}

int cc_file_close_async(CCExec* ex, CCFile *file, CCAsyncHandle* h) {
    (void)ex;
    cc_file_close(file);
    return cc__async_complete(h, 0);
}

int cc_file_read_all_async(CCExec* ex, CCFile *file, CCArena arena, CCSlice* out, CCAsyncHandle* h) {
    (void)ex;
    if (!out) return EINVAL;
    CCResult_CCSlice_CCIoError r = cc_file_read_all(file, arena);
    if (r.ok) *out = r.u.value;
    int err = !r.ok ? r.u.error.os_code : 0;
    return cc__async_complete(h, err);
}

int cc_file_read_async(CCExec* ex, CCFile *file, CCArena arena, size_t n, CCSlice* out, CCAsyncHandle* h) {
    (void)ex;
    if (!out) return EINVAL;
    CCResult_bool_CCIoError r = cc_file_read_into(file, arena, n, out);
    int err = !r.ok ? r.u.error.os_code : 0;
    return cc__async_complete(h, err);
}

int cc_file_read_line_async(CCExec* ex, CCFile *file, CCArena arena, CCSlice* out, CCAsyncHandle* h) {
    (void)ex;
    if (!out) return EINVAL;
    CCResult_bool_CCIoError r = cc_file_read_line_into(file, arena, out);
    int err = !r.ok ? r.u.error.os_code : 0;
    return cc__async_complete(h, err);
}

int cc_file_write_async(CCExec* ex, CCFile *file, CCSlice data, size_t* out_written, CCAsyncHandle* h) {
    (void)ex;
    CCResult_size_t_CCIoError res = cc_file_write(file, data);
    if (res.ok && out_written) *out_written = res.u.value;
    int err = !res.ok ? res.u.error.os_code : 0;
    return cc__async_complete(h, err);
}
#endif

