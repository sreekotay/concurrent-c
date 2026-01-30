#ifndef CC_VISITOR_FILEUTIL_H
#define CC_VISITOR_FILEUTIL_H

#include <stddef.h>

/* Internal visitor helpers (extracted from visitor.c). */
int cc__read_entire_file(const char* path, char** out_buf, size_t* out_len);
char* cc__write_temp_c_file(const char* buf, size_t len, const char* original_path);
char* cc__prepend_reparse_prelude(const char* buf, size_t len, size_t* out_len);

#endif /* CC_VISITOR_FILEUTIL_H */

