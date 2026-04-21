/* IR differential verifier implementation.  See verifier.h. */
#include "ir/verifier.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cache of the env-var lookup.  -1 = uninitialised, 0 = inactive,
 * 1 = active.  We never recompute; the env var is read at most once
 * per process. */
static int cc_ir_verify_cache = -1;

int cc_ir_verify_active(void) {
    if (cc_ir_verify_cache < 0) {
        const char* s = getenv("CC_VERIFY_IR");
        cc_ir_verify_cache = (s && *s && strcmp(s, "0") != 0) ? 1 : 0;
    }
    return cc_ir_verify_cache;
}

/* Locate the first differing byte; -1 if equal.  Length mismatch is
 * reported at min(a_len, b_len). */
static long cc_ir_verify_first_diff(const char* a, size_t a_len,
                                    const char* b, size_t b_len) {
    size_t m = a_len < b_len ? a_len : b_len;
    for (size_t i = 0; i < m; ++i) {
        if (a[i] != b[i]) return (long)i;
    }
    if (a_len != b_len) return (long)m;
    return -1;
}

static void cc_ir_verify_write_dump(const char* dir, const char* pass_name,
                                    const char* tag,
                                    const char* buf, size_t len) {
    if (!dir || !pass_name || !tag) return;
    char path[1024];
    int  rc = snprintf(path, sizeof(path), "%s/%s.%s.txt",
                       dir, pass_name, tag);
    if (rc <= 0 || (size_t)rc >= sizeof(path)) return;
    FILE* fp = fopen(path, "wb");
    if (!fp) return;
    if (buf && len > 0) fwrite(buf, 1, len, fp);
    fclose(fp);
}

int cc_ir_verify_diff(const char* pass_name,
                      const char* a, size_t a_len,
                      const char* b, size_t b_len) {
    if (!cc_ir_verify_active()) return 0;
    long d = cc_ir_verify_first_diff(a, a_len, b, b_len);
    if (d < 0) return 0;

    /* Contextual diagnostic: show ~64 bytes of context around the
     * first mismatch so we can eyeball small drifts without needing
     * a dump directory. */
    size_t ctx_before = (size_t)d > 32 ? 32 : (size_t)d;
    size_t start_a    = (size_t)d - ctx_before;
    size_t start_b    = (size_t)d - ctx_before;
    size_t show_a     = a_len - start_a > 96 ? 96 : a_len - start_a;
    size_t show_b     = b_len - start_b > 96 ? 96 : b_len - start_b;

    fprintf(stderr,
            "[CC_VERIFY_IR] pass=%s mismatch at byte %ld (a_len=%zu b_len=%zu)\n",
            pass_name ? pass_name : "?", d, a_len, b_len);
    fprintf(stderr, "  a: \"");
    for (size_t i = 0; i < show_a; ++i) {
        char c = a[start_a + i];
        if (c == '\n') fputs("\\n", stderr);
        else if (c == '\t') fputs("\\t", stderr);
        else if (c >= 32 && c < 127) fputc(c, stderr);
        else fprintf(stderr, "\\x%02x", (unsigned char)c);
    }
    fprintf(stderr, "\"\n  b: \"");
    for (size_t i = 0; i < show_b; ++i) {
        char c = b[start_b + i];
        if (c == '\n') fputs("\\n", stderr);
        else if (c == '\t') fputs("\\t", stderr);
        else if (c >= 32 && c < 127) fputc(c, stderr);
        else fprintf(stderr, "\\x%02x", (unsigned char)c);
    }
    fprintf(stderr, "\"\n");

    const char* dump_dir = getenv("CC_VERIFY_IR_DUMP");
    if (dump_dir && *dump_dir) {
        cc_ir_verify_write_dump(dump_dir, pass_name, "legacy", a, a_len);
        cc_ir_verify_write_dump(dump_dir, pass_name, "ir",     b, b_len);
        fprintf(stderr,
                "[CC_VERIFY_IR] dumped %s.legacy.txt and %s.ir.txt to %s\n",
                pass_name ? pass_name : "?", pass_name ? pass_name : "?",
                dump_dir);
    }
    return -1;
}
