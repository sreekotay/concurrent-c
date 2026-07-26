#ifndef CC_VISITOR_TEXT_SPAN_H
#define CC_VISITOR_TEXT_SPAN_H

#include <stddef.h>
#include <string.h>

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

/* ---- The verified-candidate coordinate bridge (phase 2, by design review:
 * NO anchor map — less code, one pattern).  AST nodes speak LOGICAL
 * coordinates (TCC honors #line directives), but an edited buffer's spliced
 * regions can materialize one logical (file,line) at SEVERAL physical
 * offsets.  These helpers enumerate every candidate; the CALLER arbitrates
 * by probing each candidate for the construct's actual bytes (see
 * pass_ufcs.c cc__span_at_candidate for the reference consumer).  The
 * coordinates propose, the bytes dispose. ---- */

static inline int cc_span_file_matches(const char* cur, const char* want) {
    if (!want || !want[0]) return 1;
    if (!cur || !cur[0]) return 1;   /* pre-directive prefix: the main file */
    if (strcmp(cur, want) == 0) return 1;
    {   /* tolerate absolute-vs-relative spellings: compare basenames */
        const char* cb = strrchr(cur, '/');
        const char* wb = strrchr(want, '/');
        return strcmp(cb ? cb + 1 : cur, wb ? wb + 1 : want) == 0;
    }
}

/* Collect EVERY physical offset in s whose #line-derived logical
 * coordinates equal (want_file, want_line), in buffer order.  Returns the
 * number stored (at most cap). */
static inline int cc_span_logical_line_candidates(const char* s, size_t n,
                                                  const char* want_file, int want_line,
                                                  size_t* out, int cap) {
    char cur_file[1024] = {0};
    int cur_line = 1;
    size_t i = 0;
    int count = 0;
    while (i < n) {
        /* line-start directive? `# <num> ["file"]` or `#line <num> ["file"]` */
        size_t j = i;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j < n && s[j] == '#') {
            size_t k = j + 1;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k + 4 <= n && strncmp(s + k, "line", 4) == 0) k += 4;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k < n && s[k] >= '0' && s[k] <= '9') {
                int ln = 0;
                while (k < n && s[k] >= '0' && s[k] <= '9') ln = ln * 10 + (s[k++] - '0');
                while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
                if (k < n && s[k] == '"') {
                    size_t fs = ++k, o = 0;
                    while (k < n && s[k] != '"') k++;
                    for (size_t m = fs; m < k && o + 1 < sizeof(cur_file); m++)
                        cur_file[o++] = s[m];
                    cur_file[o] = '\0';
                }
                cur_line = ln;
                while (i < n && s[i] != '\n') i++;
                if (i < n) i++;
                continue;
            }
        }
        if (cur_line == want_line && cc_span_file_matches(cur_file, want_file) &&
            count < cap) {
            out[count++] = i;
        }
        while (i < n && s[i] != '\n') i++;
        if (i < n) i++;
        cur_line++;
    }
    return count;
}

/* Map an AST logical (file, line[, col]) to a buffer offset.  When the
 * buffer carries `#line` / `# <n> "file"` directives (impl-grade .cch
 * splices, grammar expansions, …), raw newline counting is wrong —
 * TCC's stub AST speaks logical lines.  Prefer the last ledger match
 * (outer/main-file occurrence after nested splices), then fall back to
 * physical newlines when the buffer has no matching directive. */
static inline size_t cc_offset_for_logical_line(const char* s, size_t n,
                                               const char* want_file, int want_line) {
    size_t cand[32];
    int nc = cc_span_logical_line_candidates(s, n, want_file, want_line, cand, 32);
    if (nc > 0) return cand[nc - 1];
    return cc__offset_of_line_1based(s, n, want_line);
}

static inline size_t cc_offset_for_logical_line_col(const char* s, size_t n,
                                                   const char* want_file,
                                                   int want_line, int col_no) {
    size_t loff = cc_offset_for_logical_line(s, n, want_file, want_line);
    if (col_no <= 1) return loff;
    size_t off = loff + (size_t)(col_no - 1);
    if (off > n) off = n;
    return off;
}

#endif /* CC_VISITOR_TEXT_SPAN_H */