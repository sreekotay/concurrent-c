#include "source_pipeline.h"

#include <string.h>

CCSourceStage cc_source_stage_detect(const char* src, size_t len) {
    if (!src || len == 0) return CC_SRC_STAGE_RAW;
    if (strstr(src, "/* --- end container declarations (post-prelude) --- */") ||
        strstr(src, "/* --- end result type declarations --- */")) {
        return CC_SRC_STAGE_EMIT_READY;
    }
    if (strstr(src, "#ifndef __CC__\n#define __CC__ 1\n#endif")) {
        return CC_SRC_STAGE_EMIT_READY;
    }
    (void)len;
    return CC_SRC_STAGE_RAW;
}
