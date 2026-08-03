#include "cc_closure_markers.h"

#include "../util/text.h"
#include "../util/text_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Local helpers (mirror of the small functions in
 * pass_closure_literal_ast.c — duplicated here to keep the
 * markers module standalone; both copies are ~10 lines).
 * ---------------------------------------------------------------- */

static int cc__cm_is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Walk backwards from `arrow_off` (which points at `=` of `=>`)
 * to find the start of the closure literal:
 *   - Skip ASCII whitespace.
 *   - If the previous non-whitespace char is `)`, find the matching
 *     opening `(` and return its offset (parameter list form).
 *   - If it's an identifier char, scan back to the start of the
 *     identifier (single-param `x => expr` form).
 *   - Otherwise return the arrow offset itself (degenerate; the
 *     caller will discard such matches).
 *
 * `span_lo` bounds the backward scan (we never look at bytes
 * below `span_lo`).  Returns `arrow_off` when no valid start is
 * found so the caller can detect "give up" via equality.
 */
static size_t cc__cm_find_closure_start_from_arrow(const char* src,
                                                    size_t span_lo,
                                                    size_t arrow_off) {
    if (!src || arrow_off <= span_lo) return arrow_off;
    size_t j = cc_rskip_ws_and_comments(src, arrow_off);
    if (j < span_lo) j = span_lo;
    if (j <= span_lo) return arrow_off;
    char prev = src[j - 1];
    if (prev == ')') {
        /* Masked backward scan: a paren inside a comment or string in the
         * parameter list must not count toward the match. */
        size_t lp = cc_rfind_char_top_level(src, span_lo, j - 1, "");
        if (lp > span_lo && src[lp - 1] == '(') return lp - 1;
        return arrow_off;
    }
    if (cc__cm_is_ident_char(prev)) {
        size_t k = j - 1;
        while (k > span_lo && cc__cm_is_ident_char(src[k - 1])) k--;
        return k;
    }
    return arrow_off;
}

/* ----------------------------------------------------------------
 * Marker collection (pass 1)
 * ---------------------------------------------------------------- */

typedef struct {
    size_t off;   /* insertion offset (closure literal start) */
    int    id;    /* 1-based marker number */
} CCMarkerSite;

static int cc__cm_reserve(CCMarkerSite** xs, size_t* len, size_t* cap, size_t extra) {
    if (*len + extra <= *cap) return 0;
    size_t nc = *cap ? *cap : 16;
    while (nc < *len + extra) nc *= 2;
    CCMarkerSite* nb = (CCMarkerSite*)realloc(*xs, nc * sizeof(CCMarkerSite));
    if (!nb) return -1;
    *xs = nb;
    *cap = nc;
    return 0;
}

/* Scan `src` and return an array of marker insertion points in
 * source order.  Caller frees the returned array; `*out_n` holds
 * the count. */
static CCMarkerSite* cc__cm_collect_sites(const char* src, size_t src_len,
                                          size_t* out_n) {
    *out_n = 0;
    CCMarkerSite* sites = NULL;
    size_t sl = 0, sc = 0;

    CCInertScan s;
    cc_inert_scan_init(&s, NULL);

    int id = 0;
    size_t i = 0;
    while (i + 1 < src_len) {
        size_t before = i;
        if (cc_inert_scan_step(&s, src, src_len, &i)) {
            if (i == before) i++;
            continue;
        }
        /* Real code at src[i]. */
        if (src[i] == '=' && src[i + 1] == '>') {
            size_t start_off = cc__cm_find_closure_start_from_arrow(src, 0, i);
            /* Discard degenerate matches where we couldn't find a
             * plausible closure head (start == arrow). */
            if (start_off < i) {
                if (cc__cm_reserve(&sites, &sl, &sc, 1)) {
                    free(sites);
                    return NULL;
                }
                sites[sl].off = start_off;
                sites[sl].id = ++id;
                sl++;
            }
            i += 2;  /* past `=>` */
            continue;
        }
        i++;
    }

    *out_n = sl;
    return sites;
}

/* ----------------------------------------------------------------
 * Emission (pass 2)
 * ---------------------------------------------------------------- */

static int cc__cm_reserve_out(char** buf, size_t* len, size_t* cap, size_t extra) {
    if (*len + extra + 1 <= *cap) return 0;
    size_t nc = *cap ? *cap : 256;
    while (nc < *len + extra + 1) nc *= 2;
    char* nb = (char*)realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

static int cc__cm_append(char** buf, size_t* len, size_t* cap,
                          const char* s, size_t n) {
    if (cc__cm_reserve_out(buf, len, cap, n)) return -1;
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
    return 0;
}

char* cc_inject_closure_markers(const char* src, size_t src_len, size_t* out_len) {
    if (out_len) *out_len = src_len;
    if (!src) return NULL;

    size_t nsites = 0;
    CCMarkerSite* sites = cc__cm_collect_sites(src, src_len, &nsites);
    if (nsites == 0) {
        free(sites);
        return NULL;  /* no closures — caller continues with original buffer */
    }

    char* out = NULL;
    size_t ol = 0, oc = 0;
    /* Pre-size: original + roughly 16 bytes per marker.  Cheap
     * estimate; the realloc fallback in cc__cm_append handles any
     * undercount. */
    if (cc__cm_reserve_out(&out, &ol, &oc, src_len + nsites * 16)) {
        free(sites);
        return NULL;
    }

    size_t cursor = 0;
    char marker[32];
    for (size_t k = 0; k < nsites; k++) {
        size_t target = sites[k].off;
        /* Defensive: monotonic non-decreasing offsets expected from
         * the linear scan; if a marker would land before the cursor
         * (e.g. due to nested `=>` with the same start_off) skip it. */
        if (target < cursor) continue;
        if (cc__cm_append(&out, &ol, &oc, src + cursor, target - cursor)) {
            free(sites);
            free(out);
            return NULL;
        }
        int mlen = snprintf(marker, sizeof(marker), "/*CC_CLO:%d*/", sites[k].id);
        if (mlen < 0 || mlen >= (int)sizeof(marker)) {
            /* Truncation should be impossible (an int fits in well
             * under 16 chars after the prefix), but guard anyway. */
            free(sites);
            free(out);
            return NULL;
        }
        if (cc__cm_append(&out, &ol, &oc, marker, (size_t)mlen)) {
            free(sites);
            free(out);
            return NULL;
        }
        cursor = target;
    }
    /* Tail */
    if (cursor < src_len) {
        if (cc__cm_append(&out, &ol, &oc, src + cursor, src_len - cursor)) {
            free(sites);
            free(out);
            return NULL;
        }
    }

    free(sites);
    if (out_len) *out_len = ol;
    return out;
}
