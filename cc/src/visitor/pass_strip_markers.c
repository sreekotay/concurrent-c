#include "pass_strip_markers.h"
#include "util/text_scan.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int cc__strip_cc_decl_markers(const char* in, size_t in_len, char** out, size_t* out_len) {
    if (!in || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;

    /* Remove only these markers: @async, @noblock/@nonblocking,
       @blocking, @latency_sensitive, @as (named is-a embed marker).
       This is a conservative text pass so the generated C compiles; real semantics
       will be implemented by async lowering later.

       Skip markers inside strings, char literals, comments AND preprocessor-
       directive bodies (so `#define M @async ...` macro definitions don't
       have their `@async` swallowed before any expansion site is seen).
       Routed through `CCInertScan` for shared state-machine logic — see
       `cc/src/util/text_scan.h` and the invariant in PASS_INVENTORY.md. */
    char* buf = (char*)malloc(in_len + 1);
    if (!buf) return 0;
    size_t w = 0;

    CCInertScan scan;
    cc_inert_scan_init(&scan, NULL);
    size_t i = 0;
    while (i < in_len) {
        size_t before = i;
        if (cc_inert_scan_step(&scan, in, in_len, &i)) {
            size_t take = i - before;
            /* milestone 4b: drop closure-ID marker comments (/\*CC_CLO:N*\/)
             * so they never reach the emitted C.  The closure-literal pass
             * normally neutralizes them to spaces in its own working copy,
             * but strip them here too as a belt-and-suspenders net for any
             * code path that leaves a marker behind in `src_ufcs`. */
            if (take >= 11 && in[before] == '/' && in[before + 1] == '*' &&
                memcmp(in + before + 2, "CC_CLO:", 7) == 0) {
                continue;
            }
            /* Inert region: copy through verbatim. */
            memcpy(buf + w, in + before, take);
            w += take;
            continue;
        }
        char c = in[i];
        if (c == '@') {
            const char* kw = NULL;
            size_t kw_len = 0;
            if (i + 6 <= in_len && memcmp(in + i + 1, "async", 5) == 0) { kw = "async"; kw_len = 5; }
            else if (i + 8 <= in_len && memcmp(in + i + 1, "noblock", 7) == 0) { kw = "noblock"; kw_len = 7; }
            else if (i + 12 <= in_len && memcmp(in + i + 1, "nonblocking", 11) == 0) { kw = "nonblocking"; kw_len = 11; }
            else if (i + 9 <= in_len && memcmp(in + i + 1, "blocking", 8) == 0) { kw = "blocking"; kw_len = 8; }
            else if (i + 18 <= in_len && memcmp(in + i + 1, "latency_sensitive", 17) == 0) { kw = "latency_sensitive"; kw_len = 17; }
            else if (i + 3 <= in_len && memcmp(in + i + 1, "as", 2) == 0) { kw = "as"; kw_len = 2; }
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