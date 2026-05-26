/* M5.5/M6 alternative spike: run TCC's preprocessor on a source buffer and
 * return the expanded text. Lets existing text passes see post-CPP source so
 * macro-generated CC syntax (e.g. #define CHAN(T) T[~4 >]) works without
 * forking TCC.
 *
 * Enable via CC_PRE_EXPAND=1 (off by default; this is a spike). */

#include "cpp_expand.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

char* cc_cpp_expand(const char* src, size_t src_len,
                    const char* input_path, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!src) return NULL;

#ifndef CC_TCC_EXT_AVAILABLE
    (void)src_len; (void)input_path;
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

    if (out_len) *out_len = buf_size;
    tcc_delete(s);
    return buf;
#endif
}
