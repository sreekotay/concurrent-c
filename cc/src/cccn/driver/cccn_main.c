#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cccn/ast/ast.h"
#include "cccn/parser/parser.h"
#include "cccn/codegen/codegen.h"
#include "cccn/passes/passes.h"

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options] <input.ccs|input.cch>\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --dump-ast     Dump AST after parsing\n");
    fprintf(stderr, "  --emit-c       Emit lowered C to stdout\n");
    fprintf(stderr, "  -o <file>      Output to file (default: stdout)\n");
    fprintf(stderr, "  --help         Show this help\n");
    fprintf(stderr, "\nSupported extensions:\n");
    fprintf(stderr, "  .ccs           Concurrent-C source -> .c\n");
    fprintf(stderr, "  .cch           Concurrent-C header -> .h\n");
}

/* Check if path ends with given suffix */
static int ends_with(const char* path, const char* suffix) {
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    if (plen < slen) return 0;
    return strcmp(path + plen - slen, suffix) == 0;
}

/* Check if input is a header file */
static int is_header_file(const char* path) {
    return ends_with(path, ".cch") || ends_with(path, ".CCH");
}

int main(int argc, char** argv) {
    const char* input_path = NULL;
    const char* output_path = NULL;
    int dump_ast = 0;
    int emit_c = 0;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--dump-ast") == 0) {
            dump_ast = 1;
            continue;
        }
        if (strcmp(argv[i], "--emit-c") == 0) {
            emit_c = 1;
            continue;
        }
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: -o requires an argument\n");
                return 1;
            }
            output_path = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown option %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
        if (!input_path) {
            input_path = argv[i];
        } else {
            fprintf(stderr, "error: multiple input files not supported\n");
            return 1;
        }
    }
    
    if (!input_path) {
        fprintf(stderr, "error: no input file\n");
        usage(argv[0]);
        return 1;
    }
    
    /* Parse input file */
    fprintf(stderr, "cccn: parsing %s...\n", input_path);
    fflush(stderr);
    CCNFile* file = cc_parse_file(input_path);
    if (!file) {
        fprintf(stderr, "cccn: parse failed\n");
        return 1;
    }
    fprintf(stderr, "cccn: parsed successfully\n");
    
    /* Dump AST if requested */
    if (dump_ast) {
        fprintf(stderr, "\n=== AST ===\n");
        ccn_node_dump(file->root, 0);
        fprintf(stderr, "=== END AST ===\n\n");
    }
    
    /* Run lowering passes */
    fprintf(stderr, "cccn: running lowering passes...\n");
    
    if (cc_pass_lower_ufcs(file) != 0) {
        fprintf(stderr, "cccn: UFCS lowering failed\n");
        cc_file_free(file);
        return 1;
    }
    fprintf(stderr, "cccn: UFCS lowering done\n");
    
    /* Dump AST after lowering if requested */
    if (dump_ast) {
        fprintf(stderr, "\n=== AST (after lowering) ===\n");
        ccn_node_dump(file->root, 0);
        fprintf(stderr, "=== END AST ===\n\n");
    }
    
    if (cc_pass_lower_closures(file) != 0) {
        fprintf(stderr, "cccn: closure lowering failed\n");
        cc_file_free(file);
        return 1;
    }
    fprintf(stderr, "cccn: closure lowering done (%d closures)\n", file->closure_count);
    
    /* Dump final AST after all lowering */
    if (dump_ast) {
        fprintf(stderr, "\n=== AST (final) ===\n");
        ccn_node_dump(file->root, 0);
        fprintf(stderr, "=== END AST ===\n\n");
    }
    
    /* TODO: more passes
    if (cc_pass_lower_async(file) != 0) {
        fprintf(stderr, "cccn: async lowering failed\n");
        cc_file_free(file);
        return 1;
    }
    */
    
    /* Emit C if requested */
    if (emit_c) {
        FILE* out = stdout;
        if (output_path) {
            out = fopen(output_path, "w");
            if (!out) {
                fprintf(stderr, "cccn: cannot open %s for writing\n", output_path);
                cc_file_free(file);
                return 1;
            }
        }
        
        int is_header = is_header_file(input_path);
        fprintf(stderr, "cccn: emitting %s...\n", is_header ? "header" : "C");
        
        int result;
        if (is_header) {
            result = cc_emit_h(file, out, NULL);
        } else {
            result = cc_emit_c(file, out);
        }
        
        if (result != 0) {
            fprintf(stderr, "cccn: codegen failed\n");
            if (output_path) fclose(out);
            cc_file_free(file);
            return 1;
        }
        
        if (output_path) {
            fclose(out);
            fprintf(stderr, "cccn: wrote %s\n", output_path);
        }
    }
    
    cc_file_free(file);
    fprintf(stderr, "cccn: done\n");
    return 0;
}
