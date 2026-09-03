#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char* path = "/tmp/cc_io_smoke.txt";
    const char* msg = "io smoke ok\n";
    size_t msg_len = strlen(msg);

    CCResult_CCFile_CCIoError born = cc_file_create(cc_slice_cstr(path));
    if (!born.ok) return 1;
    CCFile f = born.u.value;

    CCSlice data = cc_slice_from_buffer((void*)msg, msg_len);
    CCResult_size_t_CCIoError w = cc_file_write(&f, data);
    if (!w.ok || w.u.value != msg_len) { cc_file_close(&f); return 2; }
    cc_file_close(&f);

    born = cc_file_open(cc_slice_cstr(path));
    if (!born.ok) return 3;
    f = born.u.value;

    CCArena arena = cc_arena_heap(kilobytes(4));
    if (!arena.base) { cc_file_close(&f); return 4; }

    CCResult_CCSlice_CCIoError r = cc_file_read_all(&f, arena);
    if (!r.ok) { cc_file_close(&f); cc_arena_free(&arena); return 5; }

    CCSlice out = r.u.value;
    bool match = out.len == msg_len && memcmp(out.ptr, msg, msg_len) == 0;
    cc_file_close(&f);
    cc_arena_free(&arena);
    if (!match) return 6;

    cc_std_out_write(cc_slice_from_buffer("io smoke ok\n", 13));
    return 0;
}

