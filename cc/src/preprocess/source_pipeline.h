#ifndef CC_SOURCE_PIPELINE_H
#define CC_SOURCE_PIPELINE_H

#include <stddef.h>

/* Which transformation layers have been applied to a TU text buffer.
 * Callers set this explicitly when handing buffers between pipeline stages;
 * cc_source_stage_detect() is a fallback for buffers that already carry
 * emit-plan marker comments. */
typedef enum CCSourceStage {
    CC_SRC_STAGE_RAW = 0,
    CC_SRC_STAGE_COMPTIME_SEAM = 1,
    CC_SRC_STAGE_CANONICAL = 2,
    CC_SRC_STAGE_EMIT_READY = 3,
    CC_SRC_STAGE_PARSE_VIEW = 4,
} CCSourceStage;

static inline int cc_source_stage_at_least(CCSourceStage have, CCSourceStage need) {
    return (int)have >= (int)need;
}

/* Infer stage from well-known emit-plan marker comments. */
CCSourceStage cc_source_stage_detect(const char* src, size_t len);

#endif
