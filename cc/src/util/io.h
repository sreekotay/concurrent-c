#ifndef CC_UTIL_IO_H
#define CC_UTIL_IO_H

#include <stddef.h>

// Minimal file copy utility used by the stub compiler driver.
// Returns 0 on success, non-zero on error.
int cc_copy_file(const char *src_path, const char *dst_path);

#endif // CC_UTIL_IO_H

