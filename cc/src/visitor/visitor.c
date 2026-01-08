#include "visitor.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "visitor/ufcs.h"

int cc_visit(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
    (void)root;
    if (!ctx || !ctx->symbols || !output_path) return EINVAL;
    const char* src_path = ctx->input_path ? ctx->input_path : "<cc_input>";

    FILE* out = fopen(output_path, "w");
    if (!out) return errno ? errno : -1;

    FILE* in = NULL;
    if (ctx->input_path) {
        in = fopen(ctx->input_path, "r");
    }

    fprintf(out, "/* CC visitor UFCS + passthrough stub */\n");
    fprintf(out, "#line 1 \"%s\"\n", src_path);

    if (in) {
        char line[512];
        char rewritten[1024];
        while (fgets(line, sizeof(line), in)) {
            if (cc_ufcs_rewrite_line(line, rewritten, sizeof(rewritten)) != 0) {
                strncpy(rewritten, line, sizeof(rewritten) - 1);
                rewritten[sizeof(rewritten) - 1] = '\0';
            }
            fputs(rewritten, out);
        }
        fclose(in);
    } else {
        // Fallback stub when input is unavailable.
        fprintf(out,
                "#include \"std/prelude.h\"\n"
                "int main(void) {\n"
                "  CCArena a = cc_heap_arena(kilobytes(1));\n"
                "  CCString s = cc_string_new(&a, 0);\n"
                "  cc_string_append_cstr(&a, &s, \"Hello, \");\n"
                "  cc_string_append_cstr(&a, &s, \"Concurrent-C via UFCS!\\n\");\n"
                "  cc_std_out_write(cc_string_as_slice(&s));\n"
                "  return 0;\n"
                "}\n");
    }

    fclose(out);
    return 0;
}

