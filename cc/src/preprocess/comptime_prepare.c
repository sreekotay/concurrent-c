#include "comptime_prepare.h"

#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"

#include <stdlib.h>
#include <string.h>

int cc_comptime_unified_exec_enabled(void) {
    const char* flag = getenv("CC_COMPTIME_UNIFIED_EXEC");
    if (!flag || !flag[0]) return 1;
    if (flag[0] == '0' && flag[1] == '\0') return 0;
    if (flag[0] == '1' && flag[1] == '\0') return 1;
    return 1;
}

int cc_comptime_prepare_source(char** inout_buf, size_t* inout_len,
                               const char* input_path) {
    char* resolved;
    char* templ;
    if (!inout_buf || !*inout_buf || !inout_len) return -1;

    resolved = cc__resolve_comptime_if(*inout_buf, *inout_len, input_path);
    if (resolved == (char*)-1) return -1;
    if (resolved) {
        free(*inout_buf);
        *inout_buf = resolved;
        *inout_len = strlen(resolved);
    }

    templ = cc_rewrite_string_templates_text(*inout_buf, *inout_len, input_path);
    if (templ == (char*)-1) return -1;
    if (templ) {
        free(*inout_buf);
        *inout_buf = templ;
        *inout_len = strlen(templ);
    }
    return 0;
}
