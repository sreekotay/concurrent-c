#ifndef CC_VISITOR_TEXT_SPAN_H
#define CC_VISITOR_TEXT_SPAN_H

#include <stddef.h>

/* Shared line/col -> byte offset helpers used across visitor passes.
   These are static inline to avoid link-time coupling between passes. */

static inline size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no) {
    if (!s || line_no <= 1) return 0;
    int cur = 1;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            cur++;
            if (cur == line_no) return i + 1;
        }
    }
    return len;
}

static inline size_t cc__offset_of_line_col_1based(const char* s, size_t len, int line_no, int col_no) {
    if (!s) return 0;
    if (line_no <= 1 && col_no <= 1) return 0;
    if (col_no <= 1) return cc__offset_of_line_1based(s, len, line_no);
    size_t loff = cc__offset_of_line_1based(s, len, line_no);
    size_t off = loff + (size_t)(col_no - 1);
    if (off > len) off = len;
    return off;
}

#endif /* CC_VISITOR_TEXT_SPAN_H */