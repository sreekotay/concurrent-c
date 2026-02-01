#include <ccc/cc_io_error.cch>
#include <ccc/cc_arena.cch>
#include <ccc/cc_slice.cch>
#include <ccc/std/io.cch>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cc_file_open(CCFile *file, const char *path, const char *mode) {
    if (!file) return -1;
    file->handle = NULL;
    if (!path || !mode) return -1;
    FILE *f = fopen(path, mode);
    if (!f) return -1;
    file->handle = f;
    return 0;
}

void cc_file_close(CCFile *file) {
    if (!file || !file->handle) return;
    fclose(file->handle);
    file->handle = NULL;
}

CCResult_CCSlice_CCIoError cc_file_read_all(CCFile *file, CCArena *arena) {
    if (!file || !file->handle || !arena) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(EINVAL));
    }

    if (fseek(file->handle, 0, SEEK_END) != 0) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }
    long end = ftell(file->handle);
    if (end < 0) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }
    if (fseek(file->handle, 0, SEEK_SET) != 0) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }

    size_t len = (size_t)end;
    char *buf = (char *)cc_arena_alloc(arena, len + 1, sizeof(char));
    if (!buf) {
        return cc_err_CCResult_CCSlice_CCIoError((CCIoError){CC_IO_OUT_OF_MEMORY, ENOMEM});
    }

    size_t read = fread(buf, 1, len, file->handle);
    if (read != len && ferror(file->handle)) {
        return cc_err_CCResult_CCSlice_CCIoError(cc_io_from_errno(errno));
    }
    buf[read] = '\0';
    CCSlice slice = cc_slice_from_parts(buf, read, CC_SLICE_ID_UNTRACKED, read + 1);
    return cc_ok_CCResult_CCSlice_CCIoError(slice);
}

CCResult_size_t_CCIoError cc_file_write(CCFile *file, CCSlice data) {
    if (!file || !file->handle) {
        return cc_err_CCResult_size_t_CCIoError(cc_io_from_errno(EINVAL));
    }
    size_t written = fwrite(data.ptr, 1, data.len, file->handle);
    if (written != data.len) {
        if (ferror(file->handle)) {
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

#ifdef CC_ENABLE_ASYNC
static int cc__async_complete(CCAsyncHandle* h, int err) {
    if (!h) return EINVAL;
    CC_ASYNC_HANDLE_ALLOC(h, 1);
    return cc_chan_send(h->done, &err, sizeof(int));
}

int cc_file_open_async(CCExec* ex, CCFile *file, const char *path, const char *mode, CCAsyncHandle* h) {
    (void)ex;
    int err = cc_file_open(file, path, mode);
    return cc__async_complete(h, err);
}

int cc_file_close_async(CCExec* ex, CCFile *file, CCAsyncHandle* h) {
    (void)ex;
    cc_file_close(file);
    return cc__async_complete(h, 0);
}

int cc_file_read_all_async(CCExec* ex, CCFile *file, CCArena *arena, CCSlice* out, CCAsyncHandle* h) {
    (void)ex;
    if (!out) return EINVAL;
    CCResult_CCSlice_CCIoError r = cc_file_read_all(file, arena);
    if (r.ok) *out = r.u.value;
    int err = !r.ok ? r.u.error.os_code : 0;
    return cc__async_complete(h, err);
}

int cc_file_read_async(CCExec* ex, CCFile *file, CCArena *arena, size_t n, CCSlice* out, CCAsyncHandle* h) {
    (void)ex;
    if (!out) return EINVAL;
    CCResult_bool_CCIoError r = cc_file_read(file, arena, n, out);
    int err = !r.ok ? r.u.error.os_code : 0;
    return cc__async_complete(h, err);
}

int cc_file_read_line_async(CCExec* ex, CCFile *file, CCArena *arena, CCSlice* out, CCAsyncHandle* h) {
    (void)ex;
    if (!out) return EINVAL;
    CCResult_bool_CCIoError r = cc_file_read_line(file, arena, out);
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

