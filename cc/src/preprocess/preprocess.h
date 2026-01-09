#ifndef CC_PREPROCESS_H
#define CC_PREPROCESS_H

#include <stddef.h>

// Preprocess a CC source file, rewriting CC syntax (e.g., @arena, UFCS) into
// plain C that TCC can parse. Writes to a temporary file path (returned via
// out_path), nul-terminated. Returns 0 on success; caller must unlink the
// temp file when done.
int cc_preprocess_file(const char* input_path, char* out_path, size_t out_path_sz);

#endif // CC_PREPROCESS_H

