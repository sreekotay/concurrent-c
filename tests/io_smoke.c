#define CC_ENABLE_SHORT_NAMES
#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char* path = "/tmp/cc_io_smoke.txt";
    const char* msg = "io smoke ok\n";
    size_t msg_len = strlen(msg);

    CCFile f;
    if (cc_file_open(&f, path, "w+") != 0) return 1;

    CCSlice data = cc_slice_from_buffer((void*)msg, msg_len);
    CCResultSizeIoError w = cc_file_write(&f, data);
    if (w.is_err || w.ok != msg_len) { cc_file_close(&f); return 2; }

    CCResultSizeIoError s = cc_file_seek(&f, 0, SEEK_SET);
    if (s.is_err) { cc_file_close(&f); return 3; }

    CCArena arena = cc_heap_arena(kilobytes(4));
    if (!arena.base) { cc_file_close(&f); return 4; }

    CCResultSliceIoError r = cc_file_read_all(&f, &arena);
    if (r.is_err) { cc_file_close(&f); cc_heap_arena_free(&arena); return 5; }

    CCSlice out = r.ok;
    bool match = out.len == msg_len && memcmp(out.ptr, msg, msg_len) == 0;
    cc_file_close(&f);
    cc_heap_arena_free(&arena);
    if (!match) return 6;

    cc_std_out_write(cc_slice_from_buffer("io smoke ok\n", 13));
    return 0;
}

