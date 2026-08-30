#include <ccc/cc_io_error.cch>
#include <ccc/cc_arena.cch>
#include <ccc/cc_slice.cch>
#include <ccc/std/io.cch>

#undef cc_file_read_all
#undef cc_file_read_all_async
#undef cc_file_read_all_async_deadline
#undef cc_file_read_async
#undef cc_file_read_async_deadline
#undef cc_file_read_line_async
#undef cc_file_read_line_async_deadline
#undef cc_file_read_into
#undef cc_file_read_line_into
#undef cc_path_join
#undef cc_path_dirname
#undef cc_path_basename
#undef cc_buf_writer_init
#undef cc_string_push_slice

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static FILE *cc__file_fp(CCFile *file) {
    return file ? (FILE *)file->handle : NULL;
}

static size_t cc__fgets_payload_len(const char *buf, size_t cap) {
    size_t n;
    for (n = 0; n < cap; n++) {
        size_t k;
        if (buf[n] != '\0') continue;
        k = n + 1;
        while (k < cap && (unsigned char)buf[k] == (unsigned char)CC__FGETS_FILL) k++;
        if (k == cap) return n;
    }
    return 0;
}

int cc_file_open(CCFile *file, CCSlice path_sl, const char *mode) {
    const char *path = path_sl.ptr ? (const char *)path_sl.ptr : NULL;
    if (!file) return -1;
    file->handle = NULL;
    if (!path || !mode) return -1;
    FILE *f = fopen(path, mode);
    if (!f) return -1;
    file->handle = (void *)f;
    return 0;
}

void cc_file_close(CCFile *file) {
    FILE *fp = cc__file_fp(file);
    if (!fp) return;
    fclose(fp);
    file->handle = NULL;
}

CCResult_CCSlice_CCIoError cc_file_read_all(CCFile *file, CCArena arena) {
    FILE *fp = cc__file_fp(file);
    if (!fp || !cc_arena_is_live(arena)) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(EINVAL));
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }
    long end = ftell(fp);
    if (end < 0) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }

    size_t len = (size_t)end;
    char *buf = (char *)cc_arena_alloc(arena, len + 1, sizeof(char));
    if (!buf) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_error_os(CC_IO_OUT_OF_MEMORY, ENOMEM));
    }

    size_t read = fread(buf, 1, len, fp);
    if (read != len && ferror(fp)) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }
    buf[read] = '\0';
    CCSlice slice = cc_slice_from_parts(buf, read, CC_SLICE_ID_UNTRACKED);
    return cc_ok_CCResult_CCSlice_CCIoError(slice);
}

CCResult_size_t_CCIoError cc_file_write(CCFile *file, CCSlice data) {
    FILE *fp = cc__file_fp(file);
    size_t written;
    if (!fp) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    }
    written = fwrite(data.ptr, 1, data.len, fp);
    if (written != data.len) {
        if (ferror(fp)) {
            return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
        }
    }
    return cc_ok_CCResult_size_t_CCIoError(written);
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
    FILE *fp = cc__file_fp(file);
    char *buf;
    size_t got;
    if (!fp || !cc_arena_is_live(arena) || n == 0 || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }
    buf = (char *)cc_arena_alloc(arena, n, sizeof(char));
    if (!buf) {
        return cc_err_CCResult_bool_CCIoError(
            cc_io_error_os(CC_IO_OUT_OF_MEMORY, ENOMEM));
    }
    got = fread(buf, 1, n, fp);
    if (got == 0) {
        if (ferror(fp)) {
            return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(errno));
        }
        *out = cc_slice_empty();
        return cc_ok_CCResult_bool_CCIoError(false);
    }
    *out = cc_slice_from_parts(buf, got, CC_SLICE_ID_UNTRACKED);
    return cc_ok_CCResult_bool_CCIoError(true);
}

CCResult_bool_CCIoError cc_file_read_line_into(CCFile *file, CCArena arena,
                                              CCSlice *out) {
    FILE *fp = cc__file_fp(file);
    CCString line;
    char buf[8192];
    if (!fp || !cc_arena_is_live(arena) || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }
    line = cc_string_new();
    for (;;) {
        size_t n;
        memset(buf, CC__FGETS_FILL, sizeof(buf));
        if (!fgets(buf, (int)sizeof(buf), fp)) {
            if (ferror(fp)) {
                return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(errno));
            }
            if (line.len == 0) {
                *out = cc_slice_empty();
                return cc_ok_CCResult_bool_CCIoError(false);
            }
            break;
        }
        n = cc__fgets_payload_len(buf, sizeof(buf));
        if (!cc_string_push_slice(&line, cc_slice_from_buffer(buf, n), arena)) {
            return cc_err_CCResult_bool_CCIoError(
                cc_io_error_os(CC_IO_OUT_OF_MEMORY, ENOMEM));
        }
        if (n > 0 && buf[n - 1] == '\n') break;
    }
    *out = cc_string_as_slice(&line);
    return cc_ok_CCResult_bool_CCIoError(true);
}

CCResult_bool_CCIoError cc_file_read_buf_into(CCFile *file, void *buf, size_t n,
                                             size_t *out) {
    FILE *fp = cc__file_fp(file);
    size_t got;
    if (!fp || !buf || n == 0 || !out) {
        return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(EINVAL));
    }
    got = fread(buf, 1, n, fp);
    if (got == 0) {
        if (ferror(fp)) {
            return cc_err_CCResult_bool_CCIoError(cc_io_from_errno(errno));
        }
        *out = 0;
        return cc_ok_CCResult_bool_CCIoError(false);
    }
    *out = got;
    return cc_ok_CCResult_bool_CCIoError(true);
}

CCResult_size_t_CCIoError cc_file_write_buf(CCFile *file, const void *buf,
                                           size_t n) {
    FILE *fp = cc__file_fp(file);
    size_t written;
    if (!fp || !buf) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    }
    if (n == 0) return cc_ok_CCResult_size_t_CCIoError(0);
    written = fwrite(buf, 1, n, fp);
    if (written != n && ferror(fp)) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    }
    return cc_ok_CCResult_size_t_CCIoError(written);
}

CCResult_size_t_CCIoError cc_file_sync(CCFile *file) {
    FILE *fp = cc__file_fp(file);
    if (!fp) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    if (fflush(fp) != 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    return cc_ok_CCResult_size_t_CCIoError(0);
}

CCResult_size_t_CCIoError cc_file_seek(CCFile *file, long offset, int whence) {
    FILE *fp = cc__file_fp(file);
    if (!fp) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    if (fseek(fp, offset, whence) != 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    return cc_ok_CCResult_size_t_CCIoError(0);
}

CCResult_size_t_CCIoError cc_file_tell(CCFile *file) {
    FILE *fp = cc__file_fp(file);
    long pos;
    if (!fp) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    pos = ftell(fp);
    if (pos < 0)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    return cc_ok_CCResult_size_t_CCIoError((size_t)pos);
}

CCResult_size_t_CCIoError cc_file_size(CCFile *file) {
    FILE *fp = cc__file_fp(file);
    long cur, end;
    if (!fp) return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    cur = ftell(fp);
    if (cur < 0) return cc_ok_CCResult_size_t_CCIoError(0);
    if (fseek(fp, 0, SEEK_END) != 0) return cc_ok_CCResult_size_t_CCIoError(0);
    end = ftell(fp);
    (void)fseek(fp, cur, SEEK_SET);
    if (end < 0) return cc_ok_CCResult_size_t_CCIoError(0);
    return cc_ok_CCResult_size_t_CCIoError((size_t)end);
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
    FILE *fp;
    size_t written;
    if (!w || !w->file || !w->file->handle)
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    fp = (FILE *)w->file->handle;
    written = fwrite(w->buf, 1, w->len, fp);
    if (written != w->len && ferror(fp))
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(errno));
    w->len = 0;
    return cc_ok_CCResult_size_t_CCIoError(written);
}

CCResult_size_t_CCIoError cc_buf_writer_write(CCBufWriter *w, CCSlice data) {
    size_t off;
    if (!w || !w->buf || !w->file || !w->file->handle)
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

int cc_file_open_async(CCExec* ex, CCFile *file, CCSlice path, const char *mode, CCAsyncHandle* h) {
    (void)ex;
    int err = cc_file_open(file, path, mode);
    return cc__async_complete(h, err);
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

