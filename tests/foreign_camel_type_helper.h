/* Ordinary C header — passthrough, not spliced. */
#pragma once

typedef int ForeignCamelCode;

static inline ForeignCamelCode foreign_camel_add(ForeignCamelCode a,
                                                ForeignCamelCode b) {
    return a + b;
}
