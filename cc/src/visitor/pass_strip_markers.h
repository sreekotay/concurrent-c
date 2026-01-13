#ifndef CC_PASS_STRIP_MARKERS_H
#define CC_PASS_STRIP_MARKERS_H

#include <stddef.h>

/* Marker stripping pass: removes @async, @noblock, @latency_sensitive markers from function declarations */
int cc__strip_cc_decl_markers(const char* in, size_t in_len, char** out, size_t* out_len);

#endif /* CC_PASS_STRIP_MARKERS_H */