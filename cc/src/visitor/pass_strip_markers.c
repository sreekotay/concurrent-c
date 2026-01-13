#include "pass_strip_markers.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int cc__strip_cc_decl_markers(const char* in, size_t in_len, char** out, size_t* out_len) {
    if (!in || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;

    /* Remove only these markers: @async, @noblock, @latency_sensitive.
       This is a conservative text pass so the generated C compiles; real semantics
       will be implemented by async lowering later. */
    char* buf = (char*)malloc(in_len + 1);
    if (!buf) return 0;
    size_t w = 0;
    for (size_t i = 0; i < in_len; ) {
        if (in[i] == '@') {
            const char* kw = NULL;
            size_t kw_len = 0;
            if (i + 6 <= in_len && memcmp(in + i + 1, "async", 5) == 0) { kw = "async"; kw_len = 5; }
            else if (i + 8 <= in_len && memcmp(in + i + 1, "noblock", 7) == 0) { kw = "noblock"; kw_len = 7; }
            else if (i + 18 <= in_len && memcmp(in + i + 1, "latency_sensitive", 17) == 0) { kw = "latency_sensitive"; kw_len = 17; }
            if (kw) {
                size_t j = i + 1 + kw_len;
                /* Ensure keyword boundary */
                if (j == in_len || !(isalnum((unsigned char)in[j]) || in[j] == '_')) {
                    i = j;
                    /* swallow one following space to avoid `@asyncvoid` */
                    if (i < in_len && (in[i] == ' ' || in[i] == '\t')) i++;
                    continue;
                }
            }
        }
        buf[w++] = in[i++];
    }
    buf[w] = 0;
    *out = buf;
    *out_len = w;
    return 1;
}