#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cccn/ast/ast_new.h"
#include "cccn/codegen/codegen.h"
#include "cccn/passes/passes.h"
#include "driver.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: cccn <input.ccs>\n");
        return 1;
    }

    const char* input_path = argv[1];
    printf("cccn: processing %s...\n", input_path);

    /* 
     * TODO: Integration with TCC parser to build CCCNRoot.
     * For now, this is a skeleton showing the pipeline.
     */
    
    // 1. Parse into CCCN AST
    // CCCNRoot* root = cccn_parse(input_path);
    
    // 2. Run Lowering Passes
    // cccn_pass_lower_ufcs(root);
    // cccn_pass_lower_closures(root);
    // cccn_pass_lower_async(root);
    
    // 3. Codegen to C
    // cccn_emit_c(root, stdout);

    printf("cccn: refactor in progress. Use 'ccc' for stable builds.\n");
    return 0;
}
