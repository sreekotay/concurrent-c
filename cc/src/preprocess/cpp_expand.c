/* Run TCC's preprocessor on a source buffer and return the expanded text.
 * Lets existing text passes see post-CPP source so macro-generated CC syntax
 * (e.g. #define CHAN(T) T[~4 >]) works without forking TCC.
 *
 * This is now the only initial-parse path: pre-expansion is unconditional
 * (the CC_PRE_EXPAND opt-out was collapsed 2026-05-29). */

#include "cpp_expand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Map a lowered-header path (out/include/.../X.h) back to the .cch source the
 * user wrote.  Declared here (rather than pulling in preprocess.h) to keep this
 * module's include surface minimal; defined in preprocess.c. */
extern const char* cc_lowered_header_source_for(const char* lowered_path);

/* Minimal growable byte buffer.  Returns 0 on success, -1 on OOM. */
static int cc__sb_put(char** d, size_t* dl, size_t* dc, const char* s, size_t n) {
    if (*dl + n + 1 > *dc) {
        size_t nc = *dc ? *dc : 256;
        while (nc < *dl + n + 1) nc *= 2;
        char* nv = (char*)realloc(*d, nc);
        if (!nv) return -1;
        *d = nv;
        *dc = nc;
    }
    if (n) memcpy(*d + *dl, s, n);
    *dl += n;
    (*d)[*dl] = '\0';
    return 0;
}

#ifdef CC_TCC_EXT_AVAILABLE
#include <tcc.h>
#ifdef malloc
#undef malloc
#endif
#ifdef free
#undef free
#endif
#ifdef strdup
#undef strdup
#endif
#endif

static void cc__cpp_err_silent(void* opaque, const char* msg) {
    (void)opaque;
    if (getenv("CC_DEBUG_PRE_EXPAND")) {
        fprintf(stderr, "[cc:pre-expand] tcc: %s\n", msg);
    }
}

/* Rewrite GCC-style line markers `# N "file" flags...` to bare C99
 * `#line N "file"` form. When TCC's preprocessor emits the GCC variant,
 * a subsequent TCC parse re-encounters the marker and treats the trailing
 * flags (esp. `1` = file-start, `3` = system header) as a re-entry into
 * that file, which can trigger double-decl errors (e.g. `__mbstate_t`
 * from Apple SDK headers). The C99 form has no side effects beyond
 * updating diagnostic location, which is all we want here.
 *
 * Returns a freshly-malloc'd buffer (caller frees old `buf`); writes
 * the new length to `*out_len`. Returns NULL on OOM.
 *
 * Sizing: rewriting `# N "file" flags...` to `#line N "file"` replaces the
 * 2-byte `# ` prefix with the 6-byte `#line ` prefix (+4 bytes) while only
 * dropping the trailing flags (as little as ` 1`, -2 bytes), so a marker
 * line can grow by up to +4 bytes. A same-size buffer is therefore NOT
 * sufficient; reserve 4 extra bytes per line (every '\n' plus the final
 * unterminated line) as a safe upper bound. */
static char* cc__normalize_line_markers(const char* buf, size_t len, size_t* out_len) {
    if (!buf) { if (out_len) *out_len = 0; return NULL; }
    char* dst = NULL;
    size_t dl = 0, dc = 0;
    const char* in = buf;
    const char* end = buf + len;
    while (in < end) {
        const char* line_start = in;
        const char* line_end = (const char*)memchr(in, '\n', (size_t)(end - in));
        if (!line_end) line_end = end;

        int is_gcc_marker = 0;
        const char* digit_end = NULL;
        const char* quote_open = NULL;
        const char* quote_close = NULL;
        if (line_end - line_start >= 4 && line_start[0] == '#' && line_start[1] == ' ') {
            const char* p = line_start + 2;
            while (p < line_end && (*p >= '0' && *p <= '9')) p++;
            if (p > line_start + 2 && p < line_end && *p == ' ') {
                digit_end = p;
                p++;
                if (p < line_end && *p == '"') {
                    quote_open = p;
                    const char* q = (const char*)memchr(p + 1, '"', (size_t)(line_end - p - 1));
                    if (q) {
                        quote_close = q;
                        const char* after_q = q + 1;
                        while (after_q < line_end && (*after_q == ' ' || *after_q == '\t')) after_q++;
                        if (after_q < line_end && *after_q >= '0' && *after_q <= '9') {
                            is_gcc_marker = 1;
                        }
                    }
                }
            }
        }

        int oom = 0;
        if (is_gcc_marker) {
            const char* digit_start = line_start + 2;
            while (digit_start < digit_end && *digit_start == ' ') digit_start++;
            /* Remap a lowered-header filename back to its .cch source so
               diagnostics blame the file the user wrote, not the generated
               out/include/.../X.h.  Line numbers are kept as-is (lowering of
               plain header content is line-preserving). */
            size_t fn_len = (size_t)(quote_close - (quote_open + 1));
            const char* fname = quote_open + 1;
            const char* mapped = NULL;
            if (fn_len < PATH_MAX) {
                char fnbuf[PATH_MAX];
                memcpy(fnbuf, fname, fn_len);
                fnbuf[fn_len] = '\0';
                mapped = cc_lowered_header_source_for(fnbuf);
            }
            oom |= cc__sb_put(&dst, &dl, &dc, "#line ", 6);
            oom |= cc__sb_put(&dst, &dl, &dc, digit_start, (size_t)(digit_end - digit_start));
            oom |= cc__sb_put(&dst, &dl, &dc, " \"", 2);
            if (mapped) oom |= cc__sb_put(&dst, &dl, &dc, mapped, strlen(mapped));
            else        oom |= cc__sb_put(&dst, &dl, &dc, fname, fn_len);
            oom |= cc__sb_put(&dst, &dl, &dc, "\"", 1);
        } else {
            oom |= cc__sb_put(&dst, &dl, &dc, line_start, (size_t)(line_end - line_start));
        }
        if (oom) { free(dst); return NULL; }

        if (line_end < end) {
            if (cc__sb_put(&dst, &dl, &dc, "\n", 1)) { free(dst); return NULL; }
            in = line_end + 1;
        } else {
            in = line_end;
        }
    }
    if (!dst) { dst = (char*)malloc(1); if (dst) dst[0] = '\0'; }
    if (out_len) *out_len = dl;
    return dst;
}

char* cc_cpp_expand(const char* src, size_t src_len,
                    const char* input_path, size_t* out_len) {
    return cc_cpp_expand_ex(src, src_len, input_path, out_len, 0);
}

char* cc_cpp_expand_ex(const char* src, size_t src_len,
                       const char* input_path, size_t* out_len, int keep_defines) {
    if (out_len) *out_len = 0;
    if (!src) return NULL;

#ifndef CC_TCC_EXT_AVAILABLE
    (void)src_len; (void)input_path; (void)keep_defines;
    return NULL;
#else
    TCCState* s = tcc_new();
    if (!s) return NULL;

    tcc_set_error_func(s, NULL, cc__cpp_err_silent);

    /* Mirror the parser-mode define so any #ifdef CC_PARSER_MODE branches
     * collapse the same way the existing path would have seen them. */
    tcc_define_symbol(s, "CC_PARSER_MODE", "1");
    tcc_define_symbol(s, "__CC__", "1");

    if (tcc_set_output_type(s, TCC_OUTPUT_PREPROCESS) < 0) {
        tcc_delete(s);
        return NULL;
    }
    /* keep_defines mirrors `-dD`: emit retained `#define` directives alongside
     * the expanded output.  Used to flatten the constant reparse prelude once
     * while preserving the macros AND include guards the user body relies on
     * (so the body's own #includes self-skip on the cached re-parse). */
    if (keep_defines) s->dflag = 3;

    /* Match the include path setup used by cc_init_parser_state in TCC. */
    const char* env_inc = getenv("CC_INCLUDE_PATH");
    if (env_inc && env_inc[0]) {
        tcc_add_include_path(s, env_inc);
        tcc_add_sysinclude_path(s, env_inc);
    }
    const char* user_inc = getenv("CC_USER_INCLUDE_PATH");
    if (user_inc && user_inc[0]) {
        char* paths = strdup(user_inc);
        if (paths) {
            char* p = paths; char* sep;
            while (p && *p) {
                sep = strchr(p, ':');
                if (sep) *sep = '\0';
                if (*p) { tcc_add_include_path(s, p); tcc_add_sysinclude_path(s, p); }
                p = sep ? sep + 1 : NULL;
            }
            free(paths);
        }
    }
    tcc_add_include_path(s, ".");
    tcc_add_include_path(s, "include");
    tcc_add_include_path(s, "cc/include");
    tcc_add_sysinclude_path(s, ".");
    tcc_add_sysinclude_path(s, "include");
    tcc_add_sysinclude_path(s, "cc/include");
    tcc_add_sysinclude_path(s, "third_party/tcc/include");
    tcc_add_sysinclude_path(s, "../third_party/tcc/include");
    tcc_add_sysinclude_path(s, "../../third_party/tcc/include");
#ifdef __APPLE__
    tcc_add_sysinclude_path(s, "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include");
    tcc_add_sysinclude_path(s, "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include");
#endif
    tcc_add_sysinclude_path(s, "/usr/include");
    if (input_path && input_path[0]) {
        const char* slash = strrchr(input_path, '/');
        if (slash && slash != input_path) {
            size_t n = (size_t)(slash - input_path);
            if (n < 1023) {
                char dir[1024];
                memcpy(dir, input_path, n);
                dir[n] = '\0';
                tcc_add_include_path(s, dir);
                tcc_add_sysinclude_path(s, dir);
            }
        }
    }

    char* buf = NULL;
    size_t buf_size = 0;
    FILE* mem = open_memstream(&buf, &buf_size);
    if (!mem) {
        tcc_delete(s);
        return NULL;
    }

    /* Redirect TCC's preprocess output to our memstream. ppfp is set to
     * stdout in tcc_set_output_type; override after. */
    s->ppfp = mem;

    /* Prepend #line so diagnostics from later passes still map to the
     * user's source. */
    char* prefixed = NULL;
    size_t plen = 0;
    if (input_path) {
        plen = strlen(input_path) + 64 + src_len;
        prefixed = (char*)malloc(plen + 1);
        if (!prefixed) {
            fclose(mem);
            free(buf);
            tcc_delete(s);
            return NULL;
        }
        int n = snprintf(prefixed, plen + 1, "#line 1 \"%s\"\n", input_path);
        memcpy(prefixed + n, src, src_len);
        prefixed[n + src_len] = '\0';
        plen = (size_t)n + src_len;
    } else {
        prefixed = (char*)malloc(src_len + 1);
        if (!prefixed) { fclose(mem); free(buf); tcc_delete(s); return NULL; }
        memcpy(prefixed, src, src_len);
        prefixed[src_len] = '\0';
        plen = src_len;
    }

    int rc = tcc_compile_string(s, prefixed);
    free(prefixed);

    fflush(mem);
    fclose(mem);

    if (rc < 0) {
        free(buf);
        tcc_delete(s);
        return NULL;
    }

    /* Normalize GCC-style `# N "..." flags` markers to bare `#line N "..."`
     * so the second-pass TCC parse doesn't re-trigger system-header
     * inclusion logic. */
    {
        size_t norm_len = 0;
        char* norm = cc__normalize_line_markers(buf, buf_size, &norm_len);
        if (norm) {
            free(buf);
            buf = norm;
            buf_size = norm_len;
        }
    }

    /* Re-pack `buf` into a fresh malloc allocation.  open_memstream(3) on
     * macOS hands back a buffer whose reserved capacity extends past the
     * logical end, and a subsequent malloc() can land inside that
     * capacity.  When the caller later writes its own malloc'd buffer,
     * it silently scribbles over `buf`'s trailing NUL, which then makes
     * `strlen(buf)` walk into adjacent memory and downstream text
     * passes consume a corrupted, much larger buffer.  Copying into a
     * properly-sized allocation prevents the overlap. */
    {
        char* tight = (char*)malloc(buf_size + 1);
        if (tight) {
            memcpy(tight, buf, buf_size);
            tight[buf_size] = '\0';
            free(buf);
            buf = tight;
        }
    }

    if (out_len) *out_len = buf_size;
    tcc_delete(s);
    return buf;
#endif
}
