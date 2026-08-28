#ifndef CC_UTIL_IO_H
#define CC_UTIL_IO_H

#include <stddef.h>

// Minimal file copy utility used by the stub compiler driver.
// Returns 0 on success, non-zero on error.
int cc_copy_file(const char *src_path, const char *dst_path);

// Write `data` to `path` only if the file is missing or the bytes differ.
// Leaves mtime unchanged when content matches, so make .d edges stay quiet.
int cc_write_file_if_changed(const char *path, const void *data, size_t n);

#endif // CC_UTIL_IO_H

